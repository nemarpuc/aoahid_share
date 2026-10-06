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
constexpr uint32_t destroy_timeout_ms = 2000U;

enum : uint8_t { event_button, event_key, event_media };

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

        const aoahid_node_options node = node_options();
        const auto add = [&](const aoahid_result created, aoahid_spec* spec, aoahid_node** out) {
            if (created != AOAHID_OK)
                return created;
            const aoahid_result opened = aoahid_node_open(link.device, spec, &node, out);
            // The Node keeps its own reference to the Spec.
            aoahid_spec_release(spec);
            if (opened != AOAHID_OK)
                *out = nullptr;
            return opened;
        };
        // Each Spec is created in its own statement: as a call argument it would
        // be read before the factory beside it had filled it in.
        aoahid_spec* spec = nullptr;
        result = create_mouse(options.buttons, &spec);
        result = add(result, spec, &link.mouse);
        if (result == AOAHID_OK && options.keyboard) {
            spec = nullptr;
            result = create_keyboard(&spec);
            result = add(result, spec, &link.keyboard);
            if (result == AOAHID_OK) {
                spec = nullptr;
                result = create_toggle(&spec);
                result = add(result, spec, &link.toggle);
            }
        }
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

void Usb::close_locked(Link& link) {
    link.open.store(false, std::memory_order_release);
    // The proxy's threads and Channel must be gone before the Device closes.
    aoahid_adb_proxy_stop(link.proxy);
    link.proxy = nullptr;
    // The first close consumes the Device and its Nodes for every result.
    if (link.device != nullptr)
        static_cast<void>(aoahid_device_close(link.device));
    link.device = nullptr;
    link.mouse = link.keyboard = link.toggle = nullptr;
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
    std::unique_lock<std::mutex> lock(link.queue);
    const auto pending = [&] {
        return link.count != 0 || !link.jumps.empty() || link.motion.any();
    };
    for (;;) {
        link.wake.wait(lock, [&] { return link.stopping || pending(); });
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

        const Motion motion = link.motion;
        link.motion = {};
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

        lock.unlock();
        deliver(link, motion, batch.data(), taken);
        lock.lock();
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

    bool mouse = false;
    bool keyboard = false;
    bool toggle = false;
    aoahid_result result = AOAHID_OK;
    if (motion.dx != 0 || motion.dy != 0) {
        result = aoahid_mouse_move(link.mouse, narrow(motion.dx), narrow(motion.dy));
        mouse = true;
    }
    if (result == AOAHID_OK && (motion.wheel != 0 || motion.pan != 0)) {
        result = aoahid_mouse_scroll(link.mouse, narrow(motion.wheel), narrow(motion.pan));
        mouse = true;
    }
    for (size_t index = 0; index < count && result == AOAHID_OK; ++index) {
        const Event& event = events[index];
        const uint32_t down = event.down ? 1U : 0U;
        switch (event.kind) {
        case event_button:
            result = aoahid_mouse_button(link.mouse, event.code, down);
            mouse = true;
            break;
        case event_key:
            if (link.keyboard != nullptr) {
                result = aoahid_kbd(link.keyboard, event.code, down);
                keyboard = true;
            }
            break;
        case event_media:
            if (link.toggle != nullptr) {
                result = aoahid_toggle(link.toggle, event.down ? event.code : uint16_t{0}, down);
                toggle = true;
            }
            break;
        default:
            break;
        }
    }

    // Queue every Node's report before waiting on any, so they overlap on
    // the wire instead of running one after another.
    aoahid_node* const nodes[] = {mouse ? link.mouse : nullptr, keyboard ? link.keyboard : nullptr,
                                  toggle ? link.toggle : nullptr};
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
    link.reports.fetch_add(1, std::memory_order_relaxed);
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
