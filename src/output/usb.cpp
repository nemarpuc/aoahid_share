// SPDX-License-Identifier: MIT
#include "usb.hpp"

#include <aoahid_adb_proxy.h>

#include <algorithm>
#include <iterator>
#include <limits>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace aoas {
namespace {

// Every value below is this application's policy, stated explicitly rather
// than left to libaoahid's documented fallbacks (docs/API.md there).
constexpr uint32_t discover_timeout_ms = 500U;
constexpr uint32_t control_timeout_ms = 500U;
constexpr uint32_t send_timeout_ms = 500U;
constexpr uint32_t descriptor_policy_bytes = 4096U;
constexpr uint32_t report_policy_bytes = 1024U;
constexpr uint32_t pool_slots = 8U;
constexpr uint32_t close_drain_timeout_ms = 1000U;
constexpr uint32_t drain_deadline_ms = 600U;
// How long a Node added to an open phone waits for Android to register it.
constexpr int64_t node_settle_ns = 150'000'000;
constexpr uint32_t destroy_timeout_ms = 2000U;

enum : uint8_t { event_button, event_key, event_media, event_touch };

// Consumer page usages the toggle Node declares; the key map only produces
// these. Same list and order as aoahid_player's media_usages.
constexpr uint16_t media_usages[] = {0x00B5, 0x00B6, 0x00B7, 0x00CD, 0x00E2, 0x00E9, 0x00EA,
                                     0x0223, 0x0224, 0x0238, 0x0201, 0x006F, 0x0070};

aoahid_device_options device_options() noexcept {
    aoahid_device_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.control_timeout_ms = control_timeout_ms;
    options.send_timeout_ms = send_timeout_ms;
    options.descriptor_fragment_bytes = descriptor_policy_bytes;
    options.transfer_pool_slots = pool_slots;
    options.maximum_report_bytes = report_policy_bytes;
    options.close_drain_timeout_ms = close_drain_timeout_ms;
    // The specs are fixed and range-checked by the profile factories.
    options.validate_reports = 0U;
    options.aoa_descriptor_wire_policy_bytes = descriptor_policy_bytes;
    options.linux_descriptor_policy_bytes = descriptor_policy_bytes;
    // The Linux HID parser limits listed in libaoahid's docs/LIMITS.md.
    options.linux_hid_fields_per_report_policy = 256U;
    options.linux_hid_global_stack_depth_policy = 4U;
    options.linux_hid_usages_policy = 12288U;
    options.linux_hid_report_data_bits_policy = 65528U;
    options.linux_hid_report_size_bits_policy = 256U;
    options.target_ep0_data_policy_bytes = descriptor_policy_bytes;
    options.host_control_buffer_policy_bytes = descriptor_policy_bytes;
    options.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    options.interface_number = -1;
    return options;
}

aoahid_node_options node_options() noexcept {
    aoahid_node_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // One report in flight per Node; a reserved slot keeps the three Nodes
    // from competing for the pool.
    options.has_reserved_slots = 1U;
    options.reserved_slots = 1U;
    return options;
}

aoahid_integer_field field(const int32_t minimum, const int32_t maximum,
                           const uint32_t bits) noexcept {
    aoahid_integer_field value{};
    value.logical_minimum = minimum;
    value.logical_maximum = maximum;
    value.bit_width = bits;
    return value;
}

aoahid_result create_mouse(const unsigned buttons, aoahid_spec** spec) noexcept {
    aoahid_mouse_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.button_count = buttons;
    // One report carries up to this much; libaoahid keeps the rest pending.
    options.x = field(-32767, 32767, 16);
    options.y = field(-32767, 32767, 16);
    options.enable_wheel = 1U;
    options.wheel = field(-127, 127, 8);
    options.enable_pan = 1U;
    options.pan = field(-127, 127, 8);
    return aoahid_spec_create_mouse(&options, spec);
}

aoahid_result create_keyboard(aoahid_spec** spec) noexcept {
    aoahid_keyboard_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // Through LANG and the keypad extras; the modifiers 0xE0-0xE7 are always
    // declared by the profile.
    options.usage_minimum = 0x04;
    options.usage_maximum = 0xDD;
    return aoahid_spec_create_keyboard(&options, spec);
}

aoahid_result create_toggle(aoahid_spec** spec) noexcept {
    aoahid_toggle_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.application_page = 0x0CU;  // Consumer
    options.application_usage = 0x01U; // Consumer Control
    options.field_page = 0x0CU;
    // In media_usages order: Next, Prev, Stop, Play/Pause, Mute, Vol Up,
    // Vol Down, AC Home, AC Back, AC Pan, AC New, Brightness Up, Brightness Down.
    static const aoahid_usage_semantic semantics[] = {
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_ONE_SHOT,          AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_ON_OFF_MAINTAINED, AOAHID_USAGE_RETRIGGER,
        AOAHID_USAGE_RETRIGGER, AOAHID_USAGE_ONE_SHOT,          AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_ONE_SHOT,  AOAHID_USAGE_SELECTOR_BITMAP,   AOAHID_USAGE_RETRIGGER,
        AOAHID_USAGE_RETRIGGER};
    static const char* types[] = {"EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY",
                                  "EV_KEY", "EV_KEY", "EV_KEY", "EV_KEY", "EV_REL",
                                  "EV_KEY", "EV_KEY", "EV_KEY"};
    static const char* codes[] = {
        "KEY_NEXTSONG", "KEY_PREVIOUSSONG", "KEY_STOPCD",        "KEY_PLAYPAUSE", "KEY_MUTE",
        "KEY_VOLUMEUP", "KEY_VOLUMEDOWN",   "KEY_HOMEPAGE",      "KEY_BACK",      "REL_HWHEEL",
        "KEY_NEW",      "KEY_BRIGHTNESSUP", "KEY_BRIGHTNESSDOWN"};
    static_assert(std::size(semantics) == std::size(media_usages) &&
                  std::size(types) == std::size(media_usages) &&
                  std::size(codes) == std::size(media_usages));
    options.allowed_usages = media_usages;
    options.allowed_usage_count = std::size(media_usages);
    options.usage_semantics = semantics;
    options.expected_linux_event_types = types;
    options.expected_linux_codes = codes;
    return aoahid_spec_create_toggle(&options, spec);
}

// Smallest width that represents 0..maximum.
uint32_t bits_for(const int64_t maximum) noexcept {
    uint32_t bits = 1;
    while (bits < 32 && (int64_t{1} << bits) - 1 < maximum)
        ++bits;
    return bits;
}

aoahid_result create_touchscreen(const Size size, aoahid_spec** spec) noexcept {
    aoahid_touchscreen_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // Contact 0 is the click, contact 1 the scroll; one report carries both.
    options.maximum_contacts = 2U;
    options.contacts_per_report = 2U;
    options.contact_identifier = field(0, 15, 4);
    options.x = field(0, size.w - 1, bits_for(size.w - 1));
    options.y = field(0, size.h - 1, bits_for(size.h - 1));
    options.contact_count = field(0, 2, 2);
    return aoahid_spec_create_touchscreen(&options, spec);
}

std::string describe(const aoahid_result result) {
    const aoahid_error_detail* detail = aoahid_last_error();
    // Copy the borrowed strings before aoahid_result_name() replaces them.
    const std::string field_name =
        detail != nullptr && detail->field != nullptr ? detail->field : "no field";
    const std::string reason =
        detail != nullptr && detail->reason != nullptr ? detail->reason : "no detail";
    return std::string(aoahid_result_name(result)) + " (" + field_name + ": " + reason + ")";
}

int32_t narrow(const int64_t value) noexcept {
    return static_cast<int32_t>(std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(),
                                                    std::numeric_limits<int32_t>::max()));
}

uint32_t micros(const int64_t from, const int64_t to) noexcept {
    const int64_t span = (to - from) / 1000;
    return static_cast<uint32_t>(
        std::clamp<int64_t>(span, 0, std::numeric_limits<uint32_t>::max()));
}

void note_worst(std::atomic<uint32_t>& worst, const uint32_t value) noexcept {
    uint32_t seen = worst.load(std::memory_order_relaxed);
    while (value > seen && !worst.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

uint32_t percentile(const std::vector<uint32_t>& sorted, const unsigned percent) noexcept {
    if (sorted.empty())
        return 0;
    const size_t index = (sorted.size() * percent + 99) / 100;
    return sorted[std::min(index, sorted.size()) - 1];
}

// The sender sleeps on timers and condition variables: a timer slack of one
// nanosecond keeps its wake-ups from being rounded up, and a real-time
// priority, where the user is allowed one, keeps another process from
// running in front of it. True when the priority was granted.
bool favour_this_thread() noexcept {
#ifdef __linux__
    static_cast<void>(prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL));
    sched_param parameter{};
    parameter.sched_priority = 20;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &parameter) == 0;
#else
    return false;
#endif
}

} // namespace

int64_t Usb::now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Usb::Usb() {
    for (int link = 0; link < max_links; ++link)
        links_[static_cast<size_t>(link)].sink.bind(this, link);
}

Usb::~Usb() { stop(); }

std::string Usb::start() {
    aoahid_context_options options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    // The ADB proxy needs the internal event thread.
    options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    options.log_level = AOAHID_LOG_DISABLED;
    const aoahid_result result = aoahid_context_create(&options, &scan_context_);
    if (result != AOAHID_OK) {
        scan_context_ = nullptr;
        return "libaoahid could not start: " + describe(result);
    }
    return {};
}

void Usb::stop() {
    for (Link& link : links_) {
        if (!link.sender.joinable())
            continue;
        {
            const std::lock_guard<std::mutex> lock(link.queue);
            link.stopping = true;
        }
        link.wake.notify_one();
        link.sender.join();
    }
    for (Link& link : links_) {
        {
            const std::lock_guard<std::mutex> lock(link.usb);
            close_locked(link);
        }
        if (link.context != nullptr)
            static_cast<void>(aoahid_context_destroy_blocking(link.context, destroy_timeout_ms));
        link.context = nullptr;
    }
    const std::lock_guard<std::mutex> lock(scan_lock_);
    if (discovery_ != nullptr)
        aoahid_discovery_destroy(discovery_);
    discovery_ = nullptr;
    if (scan_context_ != nullptr)
        static_cast<void>(aoahid_context_destroy_blocking(scan_context_, destroy_timeout_ms));
    scan_context_ = nullptr;
}

std::vector<PhoneInfo> Usb::scan(std::string& error) {
    std::vector<PhoneInfo> phones;
    const std::lock_guard<std::mutex> lock(scan_lock_);
    if (scan_context_ == nullptr) {
        error = "libaoahid is not running";
        return phones;
    }
    aoahid_discovery* found = nullptr;
    const aoahid_result result = aoahid_discover(scan_context_, discover_timeout_ms, &found);
    if (result != AOAHID_OK) {
        error = "the USB scan failed: " + describe(result);
        return phones;
    }
    if (discovery_ != nullptr)
        aoahid_discovery_destroy(discovery_);
    discovery_ = found;
    const size_t count = aoahid_discovery_count(discovery_);
    for (size_t index = 0; index < count; ++index) {
        const aoahid_device_info* info = aoahid_discovery_get(discovery_, index);
        PhoneInfo phone;
        if (info != nullptr) {
            phone.serial = info->serial != nullptr ? info->serial : "";
            phone.product = info->product != nullptr ? info->product : "";
            phone.vendor_id = info->vendor_id;
            phone.product_id = info->product_id;
        }
        phones.push_back(std::move(phone));
    }
    return phones;
}

int Usb::open(const size_t index, const LinkOptions& options, std::string& error) {
    // The entry of the last scan is borrowed for the whole call.
    const std::lock_guard<std::mutex> scanning(scan_lock_);
    const aoahid_device_info* info =
        discovery_ != nullptr ? aoahid_discovery_get(discovery_, index) : nullptr;
    if (info == nullptr) {
        error = "the phone is not in the last scan";
        return -1;
    }
    const std::string serial = info->serial != nullptr ? info->serial : "";

    // A phone goes back to the slot it had, then to a slot that has had none:
    // a Context hands out HID ids once, and Android tears the old ones down
    // slowly.
    int slot = -1;
    {
        const std::lock_guard<std::mutex> lock(slots_);
        const auto pick = [&](const auto& wanted) {
            for (int link = 0; link < max_links && slot < 0; ++link) {
                const Link& entry = links_[static_cast<size_t>(link)];
                if (!entry.taken && wanted(entry))
                    slot = link;
            }
        };
        pick([&](const Link& entry) { return !serial.empty() && entry.last_serial == serial; });
        pick([](const Link& entry) { return entry.last_serial.empty(); });
        pick([](const Link&) { return true; });
        if (slot >= 0)
            links_[static_cast<size_t>(slot)].taken = true;
    }
    if (slot < 0) {
        error = "too many phones are open";
        return -1;
    }
    Link& link = links_[static_cast<size_t>(slot)];
    const auto give_up = [&](const std::string& why) {
        error = why;
        const std::lock_guard<std::mutex> lock(slots_);
        link.taken = false;
        return -1;
    };

    {
        const std::lock_guard<std::mutex> lock(link.usb);
        if (link.context == nullptr) {
            aoahid_context_options context{};
            context.struct_size = static_cast<uint32_t>(sizeof(context));
            context.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
            context.log_level = AOAHID_LOG_DISABLED;
            const aoahid_result created = aoahid_context_create(&context, &link.context);
            if (created != AOAHID_OK) {
                link.context = nullptr;
                return give_up("libaoahid could not start: " + describe(created));
            }
        }

        const aoahid_device_options device = device_options();
        aoahid_result result = aoahid_device_open(link.context, info, &device, &link.device);
        if (result != AOAHID_OK) {
            link.device = nullptr;
            return give_up("the phone could not be opened: " + describe(result));
        }

        link.buttons = options.buttons;
        link.node_ready_ns = {};
        link.touch_down = {};
        result = sync_nodes_locked(link, options, 0);
        if (result != AOAHID_OK) {
            std::string why = "the input devices could not be registered: " + describe(result);
            close_locked(link);
            return give_up(why);
        }
    }

    {
        const std::lock_guard<std::mutex> lock(link.queue);
        link.motion = {};
        link.jumps.clear();
        link.hold_until = {};
        link.touch_queued_down = {};
        link.lift_owed = {};
        link.trace_until.store(0, std::memory_order_relaxed);
        link.head = link.count = 0;
        link.stopping = false;
    }
    {
        const std::lock_guard<std::mutex> lock(link.note);
        link.failure.clear();
    }
    link.reports.store(0, std::memory_order_relaxed);
    link.merged.store(0, std::memory_order_relaxed);
    link.ring_next.store(0, std::memory_order_relaxed);
    link.queue_worst.store(0, std::memory_order_relaxed);
    link.total_worst.store(0, std::memory_order_relaxed);
    link.failed.store(false, std::memory_order_relaxed);
    link.last_serial = serial;
    if (!link.sender.joinable())
        link.sender = std::thread([this, &link] { run(link); });
    link.open.store(true, std::memory_order_release);
    return slot;
}

aoahid_result Usb::sync_nodes_locked(Link& link, const LinkOptions& options,
                                     const int64_t ready_ns) {
    const aoahid_node_options node = node_options();
    // Each Spec is created in its own statement: as a call argument it would
    // be read before the factory beside it had filled it in.
    const auto sync = [&](const size_t kind, aoahid_node*& slot, const bool wanted,
                          const auto create) -> aoahid_result {
        if (wanted == (slot != nullptr))
            return AOAHID_OK;
        if (!wanted) {
            // Neutral reports first, then the unregister; an error leaves
            // the Node open.
            const aoahid_result closed = aoahid_node_close(slot);
            if (closed == AOAHID_OK)
                slot = nullptr;
            return closed;
        }
        aoahid_spec* spec = nullptr;
        aoahid_result result = create(&spec);
        if (result != AOAHID_OK)
            return result;
        result = aoahid_node_open(link.device, spec, &node, &slot);
        // The Node keeps its own reference to the Spec.
        aoahid_spec_release(spec);
        if (result != AOAHID_OK) {
            slot = nullptr;
            return result;
        }
        link.node_ready_ns[kind] = ready_ns;
        return AOAHID_OK;
    };
    aoahid_result result = sync(0, link.mouse, options.mouse, [&](aoahid_spec** spec) {
        return create_mouse(link.buttons, spec);
    });
    if (result == AOAHID_OK)
        result = sync(1, link.keyboard, options.keyboard, create_keyboard);
    if (result == AOAHID_OK)
        result = sync(2, link.toggle, options.media, create_toggle);
    // A size that changed means a new Spec: the old Node goes first.
    if (result == AOAHID_OK && link.touch != nullptr && options.touch &&
        (link.touch_size.w != options.touch_size.w || link.touch_size.h != options.touch_size.h)) {
        result = aoahid_node_close(link.touch);
        if (result == AOAHID_OK)
            link.touch = nullptr;
    }
    if (result == AOAHID_OK) {
        const bool had = link.touch != nullptr;
        result = sync(3, link.touch, options.touch, [&](aoahid_spec** spec) {
            return create_touchscreen(options.touch_size, spec);
        });
        if (result == AOAHID_OK) {
            link.touch_size = options.touch_size;
            if (!had)
                link.touch_down = {};
        }
    }
    return result;
}

void Usb::set_nodes(const int link, const LinkOptions& options) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.usb);
    if (entry.device == nullptr || !entry.open.load(std::memory_order_acquire))
        return;
    const aoahid_result result = sync_nodes_locked(entry, options, now_ns() + node_settle_ns);
    if (result != AOAHID_OK)
        fail(entry, result);
}

void Usb::close_locked(Link& link) {
    link.open.store(false, std::memory_order_release);
    // The proxy's threads and Channel must be gone before the Device closes.
    aoahid_adb_proxy_stop(link.proxy);
    link.proxy = nullptr;
    // The first close consumes the Device and its Nodes for every result.
    if (link.device != nullptr)
        static_cast<void>(aoahid_device_close(link.device));
    link.device = nullptr;
    link.mouse = link.keyboard = link.toggle = link.touch = nullptr;
    link.touch_down = {};
}

void Usb::close(const int link) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    {
        const std::lock_guard<std::mutex> lock(entry.usb);
        close_locked(entry);
    }
    const std::lock_guard<std::mutex> lock(slots_);
    entry.taken = false;
}

bool Usb::alive(const int link) const noexcept {
    if (link < 0 || link >= max_links)
        return false;
    const Link& entry = links_[static_cast<size_t>(link)];
    return entry.open.load(std::memory_order_acquire) &&
           !entry.failed.load(std::memory_order_acquire);
}

std::string Usb::failure(const int link) const {
    if (link < 0 || link >= max_links)
        return {};
    const Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.note);
    return entry.failure;
}

uint64_t Usb::reports(const int link) const noexcept {
    if (link < 0 || link >= max_links)
        return 0;
    return links_[static_cast<size_t>(link)].reports.load(std::memory_order_relaxed);
}

std::vector<std::string> Usb::trace(const int link) {
    std::vector<std::string> lines;
    if (link < 0 || link >= max_links)
        return lines;
    Link& entry = links_[static_cast<size_t>(link)];
    constexpr int64_t keep_on_ns = 2000000000;
    entry.trace_until.store(now_ns() + keep_on_ns, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(entry.trace_lock);
    const size_t size = entry.trace_ring.size();
    const uint64_t first = entry.trace_next > size ? entry.trace_next - size : 0;
    for (uint64_t number = first; number < entry.trace_next; ++number) {
        const Trace& call = entry.trace_ring[number % size];
        std::string text;
        switch (call.kind) {
        case Trace::mouse_move:
            text = "mouse move dx " + std::to_string(call.a) + " dy " + std::to_string(call.b);
            break;
        case Trace::mouse_scroll:
            text =
                "mouse scroll wheel " + std::to_string(call.a) + " pan " + std::to_string(call.b);
            break;
        case Trace::button:
            text = "mouse button " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::key:
            text = "key usage " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::media:
            text = "media usage " + std::to_string(call.a) + (call.phase != 0 ? " down" : " up");
            break;
        case Trace::touch:
            text = "touch contact " + std::to_string(call.a) +
                   (call.phase == 0   ? " lift"
                    : call.phase == 1 ? " down"
                                      : " move") +
                   " x " + std::to_string(call.b) + " y " + std::to_string(call.c);
            break;
        }
        lines.push_back(std::to_string(number) + " " + std::to_string(call.at / 1000000) + " " +
                        std::to_string(call.report) + " " + text);
    }
    return lines;
}

LinkStats Usb::stats(const int link) const {
    LinkStats stats;
    if (link < 0 || link >= max_links)
        return stats;
    const Link& entry = links_[static_cast<size_t>(link)];
    stats.reports = entry.reports.load(std::memory_order_relaxed);
    stats.merged = entry.merged.load(std::memory_order_relaxed);
    stats.queue_max = entry.queue_worst.load(std::memory_order_relaxed);
    stats.total_max = entry.total_worst.load(std::memory_order_relaxed);
    stats.realtime = entry.realtime.load(std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(entry.queue);
        stats.depth = static_cast<uint32_t>(entry.count + entry.jumps.size() +
                                            (entry.motion.any() ? 1U : 0U));
    }
    const size_t taken = static_cast<size_t>(
        std::min<uint64_t>(entry.ring_next.load(std::memory_order_relaxed), ring_size));
    std::vector<uint32_t> queued(taken);
    std::vector<uint32_t> total(taken);
    for (size_t at = 0; at < taken; ++at) {
        queued[at] = entry.queue_us[at].load(std::memory_order_relaxed);
        total[at] = entry.total_us[at].load(std::memory_order_relaxed);
    }
    std::sort(queued.begin(), queued.end());
    std::sort(total.begin(), total.end());
    stats.samples = static_cast<uint32_t>(taken);
    stats.queue_p50 = percentile(queued, 50);
    stats.queue_p99 = percentile(queued, 99);
    stats.total_p50 = percentile(total, 50);
    stats.total_p99 = percentile(total, 99);
    return stats;
}

Sink& Usb::sink(const int link) noexcept {
    return links_[static_cast<size_t>(std::clamp(link, 0, max_links - 1))].sink;
}

int Usb::start_proxy(const int link, const uint16_t port, bool& held, bool& absent) {
    held = absent = false;
    if (link < 0 || link >= max_links)
        return AOAHID_ADB_PROXY_ERR_ARGUMENT;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.usb);
    if (entry.device == nullptr || entry.proxy != nullptr)
        return AOAHID_ADB_PROXY_ERR_ARGUMENT;
    const int result = aoahid_adb_proxy_start(entry.device, port, &entry.proxy);
    if (result != AOAHID_ADB_PROXY_OK)
        entry.proxy = nullptr;
    if (result == AOAHID_ADB_PROXY_ERR_INTERFACE) {
        // The proxy makes no libaoahid call after a failed channel open, so
        // this thread's last diagnostic is that failure.
        const aoahid_error_detail* detail = aoahid_last_error();
        const int code = detail != nullptr ? detail->code : 0;
        // LIBUSB_ERROR_NOT_SUPPORTED is a driver problem; stopping adb cannot fix it.
        const bool driver = detail != nullptr && detail->libusb_status == -12;
        held = !driver && (code == AOAHID_ERR_BUSY || code == AOAHID_ERR_ACCESS);
        absent = !driver && code == AOAHID_ERR_UNSUPPORTED;
    }
    return result;
}

void Usb::stop_proxy(const int link) {
    if (link < 0 || link >= max_links)
        return;
    Link& entry = links_[static_cast<size_t>(link)];
    const std::lock_guard<std::mutex> lock(entry.usb);
    aoahid_adb_proxy_stop(entry.proxy);
    entry.proxy = nullptr;
}

void Usb::LinkSink::move(const Delta counts) {
    Link& link = usb_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // Behind a waiting key or button it is summed after all, and goes
        // out with that edge: the two queues keep no order between them.
        if (link.every.load(std::memory_order_relaxed) && link.count == 0) {
            queue_alone(link, Motion{counts.x, counts.y, 0, 0, at});
        } else {
            Motion& motion = link.motion;
            if (motion.any())
                link.merged.fetch_add(1, std::memory_order_relaxed);
            else
                motion.at = at;
            motion.dx += counts.x;
            motion.dy += counts.y;
        }
    }
    link.wake.notify_one();
}

void Usb::LinkSink::jump(const Delta counts) {
    Link& link = usb_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        queue_alone(link, Motion{counts.x, counts.y, 0, 0, at});
    }
    link.wake.notify_one();
}

void Usb::queue_alone(Link& link, const Motion& report) {
    // What was moved before it stays before it.
    if (link.motion.any()) {
        link.jumps.push_back(link.motion);
        link.motion = {};
    }
    link.jumps.push_back(report);
}

void Usb::LinkSink::scroll(const int wheel, const int pan) {
    Link& link = usb_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        if (link.every.load(std::memory_order_relaxed) && link.count == 0) {
            queue_alone(link, Motion{0, 0, wheel, pan, at});
        } else {
            Motion& motion = link.motion;
            if (motion.any())
                link.merged.fetch_add(1, std::memory_order_relaxed);
            else
                motion.at = at;
            motion.wheel += wheel;
            motion.pan += pan;
        }
    }
    link.wake.notify_one();
}

void Usb::LinkSink::button(const unsigned button, const bool down) {
    usb_->push(usb_->links_[link_],
               {event_button, link_, static_cast<uint16_t>(button), down, now_ns()});
}

void Usb::LinkSink::key(const uint16_t usage, const bool down) {
    usb_->push(usb_->links_[link_], {event_key, link_, usage, down, now_ns()});
}

void Usb::LinkSink::media(const uint16_t usage, const bool down) {
    usb_->push(usb_->links_[link_], {event_media, link_, usage, down, now_ns()});
}

void Usb::LinkSink::touch(const unsigned contact, const int x, const int y, const bool down) {
    if (contact > 1)
        return;
    Link& link = usb_->links_[link_];
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // A lift ends any wait to lift it.
        if (!down)
            link.hold_until[contact] = 0;
        usb_->queue_touch_locked(link, contact, x, y, down, now_ns());
    }
    link.wake.notify_one();
}

void Usb::LinkSink::touch_place(const unsigned contact, const int x, const int y,
                                const double gap_s) {
    if (contact > 1)
        return;
    Link& link = usb_->links_[link_];
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        usb_->queue_touch_locked(link, contact, x, y, true, now_ns(),
                                 static_cast<int64_t>(gap_s * 1e9));
    }
    link.wake.notify_one();
}

void Usb::LinkSink::touch_hold(const unsigned contact, const int x, const int y,
                               const double release_s) {
    if (contact > 1)
        return;
    Link& link = usb_->links_[link_];
    const int64_t at = now_ns();
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        link.hold_until[contact] = at + static_cast<int64_t>(release_s * 1e9);
        link.hold_x[contact] = x;
        link.hold_y[contact] = y;
        usb_->queue_touch_locked(link, contact, x, y, true, at);
    }
    link.wake.notify_one();
}

void Usb::queue_touch_locked(Link& link, const unsigned contact, const int x, const int y,
                             const bool down, const int64_t at, const int64_t gap_ns) {
    // Only a move merges with a move; the event that puts the contact down
    // stays an event of its own, so the placement is sent before the move.
    if (down && link.count != 0) {
        Event& last = link.events[(link.head + link.count - 1) % link.events.size()];
        if (last.kind == event_touch && last.code == contact && last.down && !last.placing) {
            last.x = x;
            last.y = y;
            return;
        }
    }
    // While a lift waits for room, nothing new is put down: it would be
    // lifted by that lift.
    if (down && link.lift_owed[contact])
        return;
    if (link.count == link.events.size()) {
        // A full queue means the phone stopped answering, and the link fails
        // on its own timeout, so the newest edge is dropped. A lift is kept,
        // or the contact would stay down on the device.
        if (!down) {
            link.lift_owed[contact] = true;
            link.lift_x[contact] = x;
            link.lift_y[contact] = y;
            link.touch_queued_down[contact] = false;
        }
        return;
    }
    Event& slot = link.events[(link.head + link.count) % link.events.size()];
    slot = Event{event_touch,
                 static_cast<uint8_t>(&link - links_.data()),
                 static_cast<uint16_t>(contact),
                 down,
                 at,
                 x,
                 y,
                 down && !link.touch_queued_down[contact],
                 gap_ns};
    link.touch_queued_down[contact] = down;
    ++link.count;
}

void Usb::push(Link& link, const Event& event) {
    {
        const std::lock_guard<std::mutex> lock(link.queue);
        // A full queue means the phone stopped answering; the link fails on
        // its own timeout, so the newest edge is dropped here.
        if (link.count == link.events.size())
            return;
        link.events[(link.head + link.count) % link.events.size()] = event;
        ++link.count;
    }
    link.wake.notify_one();
}

void Usb::run(Link& link) {
    using clock = std::chrono::steady_clock;
    link.realtime.store(favour_this_thread(), std::memory_order_relaxed);
    clock::time_point next_tick = clock::now();
    clock::time_point placed_until = next_tick;
    std::unique_lock<std::mutex> lock(link.queue);
    // Android drops a touch when a mouse report arrives while it is down, and
    // takes that for a cancel, not a lift. So movement waits while a contact
    // is down or a touch event is still queued, and goes out in a report
    // after the lift.
    const auto touching = [&] {
        if (link.touch_queued_down[0] || link.touch_queued_down[1])
            return true;
        for (size_t waiting = 0; waiting < link.count; ++waiting) {
            if (link.events[(link.head + waiting) % link.events.size()].kind == event_touch)
                return true;
        }
        return false;
    };
    const auto pending = [&] {
        return link.count != 0 || !link.jumps.empty() || (link.motion.any() && !touching());
    };
    for (;;) {
        for (;;) {
            // A held contact whose time has come is lifted like any other edge.
            const int64_t now = now_ns();
            int64_t next = 0;
            for (size_t contact = 0; contact < link.hold_until.size(); ++contact) {
                int64_t& until = link.hold_until[contact];
                if (until == 0)
                    continue;
                if (now >= until) {
                    until = 0;
                    queue_touch_locked(link, static_cast<unsigned>(contact), link.hold_x[contact],
                                       link.hold_y[contact], false, now);
                } else if (next == 0 || until < next) {
                    next = until;
                }
            }
            // A lift that found the queue full goes in as soon as there is room.
            for (size_t contact = 0; contact < link.lift_owed.size(); ++contact) {
                if (link.lift_owed[contact] && link.count < link.events.size()) {
                    link.lift_owed[contact] = false;
                    queue_touch_locked(link, static_cast<unsigned>(contact), link.lift_x[contact],
                                       link.lift_y[contact], false, now);
                }
            }
            if (link.stopping || pending())
                break;
            if (next == 0)
                link.wake.wait(lock);
            else
                link.wake.wait_until(lock, clock::time_point(std::chrono::nanoseconds(next)));
        }
        if (link.stopping)
            return;

        // A jump goes out alone and first: Android has to clamp it before
        // the movement queued behind it is applied.
        if (!link.jumps.empty()) {
            const Motion jump = link.jumps.front();
            link.jumps.pop_front();
            lock.unlock();
            deliver(link, jump, nullptr, 0);
            lock.lock();
            continue;
        }

        // Motion alone waits for the next tick of the configured rate; a key
        // or button edge goes out at once and takes the pending motion along.
        const unsigned rate = link.rate_hz.load(std::memory_order_relaxed);
        // What follows a finger's placement waits one report period (1 ms at
        // least), so Android sees the finger down before it moves.
        if (clock::now() < placed_until) {
            link.wake.wait_until(lock, placed_until,
                                 [&] { return link.stopping || !link.jumps.empty(); });
            if (link.stopping)
                return;
            if (!link.jumps.empty())
                continue;
        }
        if (rate != 0 && link.count == 0) {
            const auto period = std::chrono::nanoseconds(1000000000LL / rate);
            const clock::time_point now = clock::now();
            if (next_tick > now) {
                // A key, a button or a jump ends the wait: none of them is paced.
                link.wake.wait_until(lock, next_tick, [&] {
                    return link.stopping || link.count != 0 || !link.jumps.empty();
                });
                if (link.stopping)
                    return;
                if (!link.jumps.empty())
                    continue;
            }
            next_tick = std::max(next_tick, clock::now() - period) + period;
        }
        if (!pending())
            continue;

        // A report holds one state per key and button, so a second edge of
        // the same one waits for the next report.
        std::array<Event, 32> batch{};
        size_t taken = 0;
        while (link.count != 0 && taken < batch.size()) {
            const Event& front = link.events[link.head];
            const bool repeat =
                std::any_of(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(taken),
                            [&](const Event& earlier) {
                                return earlier.kind == front.kind &&
                                       (earlier.code == front.code || front.kind == event_media);
                            });
            if (repeat)
                break;
            batch[taken++] = front;
            link.head = (link.head + 1) % link.events.size();
            --link.count;
        }

        // The batch's own touch events are no longer queued, but they are
        // not yet delivered: movement still waits for them.
        Motion motion;
        const bool batch_touches =
            std::any_of(batch.begin(), batch.begin() + static_cast<std::ptrdiff_t>(taken),
                        [](const Event& each) { return each.kind == event_touch; });
        if (!batch_touches && !touching()) {
            motion = link.motion;
            link.motion = {};
        }
        // A finger that was put down holds the next report back: by what its
        // event says, or one report period (1 ms at least).
        int64_t wait_ns = 0;
        for (size_t index = 0; index < taken; ++index) {
            const Event& each = batch[index];
            if (each.kind != event_touch || !each.placing)
                continue;
            const int64_t period =
                rate != 0 ? 1000000000LL / static_cast<int64_t>(rate) : int64_t{0};
            wait_ns = std::max(wait_ns,
                               each.gap_ns >= 0 ? each.gap_ns : std::max(period, int64_t{1000000}));
        }
        lock.unlock();
        deliver(link, motion, batch.data(), taken);
        lock.lock();
        if (wait_ns > 0)
            placed_until = clock::now() + std::chrono::nanoseconds(wait_ns);
    }
}

void Usb::fail(Link& link, const aoahid_result result) {
    {
        const std::lock_guard<std::mutex> lock(link.note);
        link.failure = describe(result);
    }
    link.failed.store(true, std::memory_order_release);
}

void Usb::deliver(Link& link, const Motion& motion, const Event* events, const size_t count) {
    const std::lock_guard<std::mutex> usb(link.usb);
    if (!link.open.load(std::memory_order_acquire) || link.failed.load(std::memory_order_acquire))
        return;

    // Of everything this report carries, what came first sets its age.
    int64_t first = motion.at;
    for (size_t index = 0; index < count; ++index) {
        if (events[index].at != 0 && (first == 0 || events[index].at < first))
            first = events[index].at;
    }
    const int64_t started = now_ns();

    // A Node that is absent, or was added a moment ago, takes nothing.
    const auto usable = [&](aoahid_node* node, const size_t kind) {
        return node != nullptr && started >= link.node_ready_ns[kind];
    };
    const bool can_mouse = usable(link.mouse, 0);
    const bool can_keyboard = usable(link.keyboard, 1);
    const bool can_toggle = usable(link.toggle, 2);
    const bool can_touch = usable(link.touch, 3);

    bool mouse = false;
    bool keyboard = false;
    bool toggle = false;
    bool touch = false;
    aoahid_result result = AOAHID_OK;
    // What this report carries, kept only while someone is looking.
    const bool tracing = started < link.trace_until.load(std::memory_order_relaxed);
    std::array<Trace, 34> calls;
    size_t called = 0;
    const auto note_call = [&](const Trace::Kind kind, const uint8_t phase, const int32_t a,
                               const int32_t b = 0, const int32_t c = 0) {
        if (tracing && called < calls.size())
            calls[called++] = Trace{started, 0, kind, phase, a, b, c};
    };
    if (can_mouse && (motion.dx != 0 || motion.dy != 0)) {
        result = aoahid_mouse_move(link.mouse, narrow(motion.dx), narrow(motion.dy));
        note_call(Trace::mouse_move, 0, narrow(motion.dx), narrow(motion.dy));
        mouse = true;
    }
    if (can_mouse && result == AOAHID_OK && (motion.wheel != 0 || motion.pan != 0)) {
        result = aoahid_mouse_scroll(link.mouse, narrow(motion.wheel), narrow(motion.pan));
        note_call(Trace::mouse_scroll, 0, narrow(motion.wheel), narrow(motion.pan));
        mouse = true;
    }
    for (size_t index = 0; index < count && result == AOAHID_OK; ++index) {
        const Event& event = events[index];
        const uint32_t down = event.down ? 1U : 0U;
        switch (event.kind) {
        case event_button:
            if (can_mouse) {
                result = aoahid_mouse_button(link.mouse, event.code, down);
                note_call(Trace::button, event.down, event.code);
                mouse = true;
            }
            break;
        case event_key:
            if (can_keyboard) {
                result = aoahid_kbd(link.keyboard, event.code, down);
                note_call(Trace::key, event.down, event.code);
                keyboard = true;
            }
            break;
        case event_media:
            if (can_toggle) {
                result = aoahid_toggle(link.toggle, event.down ? event.code : uint16_t{0}, down);
                note_call(Trace::media, event.down, event.code);
                toggle = true;
            }
            break;
        case event_touch:
            if (can_touch && event.code < 2) {
                // A lift of a contact that was never placed (its down was
                // dropped while the Node settled) is not sent: libaoahid
                // rejects it and the link would fail.
                if (!event.down && !link.touch_down[event.code])
                    break;
                result = aoahid_touch(link.touch, event.code, down, event.x, event.y, nullptr);
                note_call(Trace::touch,
                          !event.down                   ? 0
                          : link.touch_down[event.code] ? 2
                                                        : 1,
                          event.code, event.x, event.y);
                link.touch_down[event.code] = event.down;
                touch = true;
            }
            break;
        default:
            break;
        }
    }

    // Queue every Node's report before waiting on any, so they overlap on
    // the wire instead of running one after another.
    aoahid_node* const nodes[] = {mouse ? link.mouse : nullptr, keyboard ? link.keyboard : nullptr,
                                  toggle ? link.toggle : nullptr, touch ? link.touch : nullptr};
    for (aoahid_node* node : nodes) {
        if (node != nullptr && result == AOAHID_OK)
            result = aoahid_node_submit(node);
    }
    for (aoahid_node* node : nodes) {
        // Also drains the fragments a large move still owes.
        if (node != nullptr && result == AOAHID_OK)
            result = aoahid_node_submit_blocking(node, drain_deadline_ms);
    }
    if (result != AOAHID_OK) {
        fail(link, result);
        return;
    }
    const uint64_t report = link.reports.fetch_add(1, std::memory_order_relaxed) + 1;
    if (called != 0) {
        const std::lock_guard<std::mutex> lock(link.trace_lock);
        for (size_t index = 0; index < called; ++index) {
            calls[index].report = report;
            link.trace_ring[link.trace_next++ % link.trace_ring.size()] = calls[index];
        }
    }
    if (first == 0)
        return;
    const int64_t done = now_ns();
    const size_t slot = link.ring_next.fetch_add(1, std::memory_order_relaxed) % ring_size;
    const uint32_t queued = micros(first, started);
    const uint32_t total = micros(first, done);
    link.queue_us[slot].store(queued, std::memory_order_relaxed);
    link.total_us[slot].store(total, std::memory_order_relaxed);
    note_worst(link.queue_worst, queued);
    note_worst(link.total_worst, total);
}

} // namespace aoas
