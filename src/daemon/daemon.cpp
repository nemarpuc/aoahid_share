// SPDX-License-Identifier: MIT
#include "daemon.hpp"

#include "store.hpp"

#include <aoahid_adb_proxy.h>

#include <ctime>

#include "paths.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace aoas {
namespace {

// A Node rejects or drops the first reports after it opens (libaoahid
// docs/TARGET_MATRIX.md), so a phone is not crossed to before this passes.
constexpr double settle_seconds = 0.15;
constexpr auto health_period = std::chrono::milliseconds(250);
// How long the adb server takes to let go of the phones once it is stopped.
constexpr auto adb_release_wait = std::chrono::milliseconds(800);
// A capture backend that would not start is tried again after 15 seconds,
// waited out in short steps so a quit is noticed.
constexpr auto retry_step = std::chrono::milliseconds(200);
constexpr int retry_steps = 75;
// Counts that take a cursor to a display's edge from anywhere, and the most
// one `probe` may move.
constexpr int probe_reach = 30000;
// After adb's server is stopped, the proxy is tried again this often.
constexpr int proxy_retries = 5;
constexpr auto proxy_retry_step = std::chrono::milliseconds(200);

// Next to the state file; the daemon has no terminal when it is started at
// login or from the settings window.
std::string log_file() { return (std::filesystem::path(settings_dir()) / "daemon.log").string(); }

// Files are written on a thread of their own: the capture thread handles
// the input, and a disk that is slow must not hold it. Jobs run in the order
// they were posted, and all of them run before the writer ends.
class Writer {
  public:
    Writer() : thread_([this] { loop(); }) {}
    ~Writer() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        wake_.notify_one();
        thread_.join();
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void post(std::function<void()> job) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

  private:
    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return done_ || !jobs_.empty(); });
            while (!jobs_.empty()) {
                std::function<void()> job = std::move(jobs_.front());
                jobs_.pop_front();
                lock.unlock();
                job();
                lock.lock();
            }
            if (done_)
                return;
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool done_{};
    std::thread thread_;
};

std::atomic<Writer*> writer{nullptr};

// Files are written through one Writer while a daemon runs.
struct WriterScope {
    Writer owned;
    WriterScope() { writer.store(&owned); }
    ~WriterScope() { writer.store(nullptr); }
};

void write_later(std::function<void()> job) {
    if (Writer* const active = writer.load())
        active->post(std::move(job));
    else
        job();
}

void log_line(const std::string& text) {
    std::fprintf(stderr, "aoahid_share: %s\n", text.c_str());
    write_later([text] {
        std::ofstream file(log_file(), std::ios::app);
        file << text << "\n";
    });
}

const char* mode_name(const AccelMode mode) noexcept {
    switch (mode) {
    case AccelMode::exact:
        return "exact";
    case AccelMode::curve:
        return "curve";
    case AccelMode::unknown:
        return "unknown";
    }
    return "unknown";
}

const char* side_text(const Side side) noexcept {
    switch (side) {
    case Side::left:
        return "left";
    case Side::right:
        return "right";
    case Side::top:
        return "top";
    case Side::bottom:
        return "bottom";
    }
    return "right";
}

// A string that came from outside (a USB descriptor, an error text) as it
// may appear in a status line: the answer is one `key=value` per line, so a
// line break inside a value would read as another key.
std::string one_line(std::string text) {
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            c = ' ';
    }
    return text;
}

#ifdef __linux__
// The most mouse buttons any connected pointing device reports, read from
// each input device's key capability bitmap; 0 when none could be read.
unsigned count_mouse_buttons() {
    unsigned most = 0;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/input", ignored)) {
        if (entry.path().filename().string().rfind("event", 0) != 0)
            continue;
        std::ifstream file(entry.path() / "device" / "capabilities" / "key");
        std::vector<unsigned long long> words;
        std::string word;
        // Hexadecimal words, the most significant first.
        while (file >> word)
            words.push_back(std::strtoull(word.c_str(), nullptr, 16));
        const auto bit = [&words](const unsigned index) {
            const size_t from_end = index / 64;
            return from_end < words.size() &&
                   ((words[words.size() - 1 - from_end] >> (index % 64)) & 1ULL) != 0;
        };
        // BTN_LEFT; a device without it is not a mouse.
        if (!bit(0x110))
            continue;
        unsigned buttons = 0;
        for (unsigned code = 0x110; code <= 0x117; ++code)
            buttons += bit(code) ? 1U : 0U;
        most = std::max(most, buttons);
    }
    return most;
}
#elif defined(_WIN32)
// Windows reports the mouse with the most buttons; 0 when there is none.
unsigned count_mouse_buttons() {
    return static_cast<unsigned>(std::clamp(GetSystemMetrics(SM_CMOUSEBUTTONS), 0, 8));
}
#else
unsigned count_mouse_buttons() { return 0; }
#endif

// What the phone is opened with.
LinkOptions link_options_for(const Config& config, const DeviceConfig& device) {
    LinkOptions options;
    options.buttons = config.mouse_buttons;
    options.mouse = mouse_of(config, device);
    options.keyboard = keyboard_of(config, device);
    options.media = media_of(config, device);
    // The touchscreen Spec needs the natural size; without it there is no
    // Node, and status says so (touch_state()).
    options.touch = touch_of(config, device).enabled && device.width >= 2 && device.height >= 2;
    options.touch_size = {device.width, device.height};
    return options;
}

// What status says about touch.
std::string touch_state(const Config& config, const DeviceConfig& device) {
    if (!touch_of(config, device).enabled)
        return "off";
    std::string missing;
    const auto lack = [&](const char* name) {
        missing += (missing.empty() ? "" : ", ") + std::string(name);
    };
    if (device.width < 2)
        lack("width");
    if (device.height < 2)
        lack("height");
    if (device.rotation < 0)
        lack("rotation");
    return missing.empty() ? "on" : "not set: " + missing;
}

} // namespace

Daemon::Daemon() = default;
Daemon::~Daemon() = default;

double Daemon::now() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Daemon::on_capture(const std::function<void()>& task) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();
    capture_->post([task, done] {
        task();
        done->set_value();
    });
    while (finished.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        if (finished_.load(std::memory_order_acquire))
            return;
    }
}

void Daemon::save_state(const std::string& key, const std::string& value) {
    std::string text;
    {
        const std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = state_set(state_, key, value);
        text = state_;
    }
    write_later([text] { static_cast<void>(write_file(state_file_path(), text)); });
}

std::string Daemon::load_config() {
    Config loaded;
    const std::string error = load_store(loaded);
    if (!error.empty())
        return error;
    adopt_config(std::move(loaded));
    return {};
}

void Daemon::adopt_config(Config loaded) {
    config_ = std::move(loaded);
    static_cast<void>(parse_hotkey(config_.toggle_hotkey, toggle_));
    static_cast<void>(parse_hotkey(config_.panic_hotkey, panic_));
    static_cast<void>(parse_hotkey(config_.pause_hotkey, pause_));
    static_cast<void>(parse_hotkey(config_.resync_hotkey, resync_));
    for (size_t index = 0; index < media_key_count; ++index)
        static_cast<void>(parse_hotkey(config_.media_hotkeys[index], media_hotkeys_[index]));
    device_hotkeys_.assign(config_.devices.size(), Hotkey{});
    for (size_t index = 0; index < config_.devices.size(); ++index)
        static_cast<void>(parse_hotkey(config_.devices[index].hotkey, device_hotkeys_[index]));
}

// What the phone is opened with. Only the number of buttons is fixed by it:
// the Nodes are added and removed on the open phone.
LinkOptions Daemon::link_options_of(const DeviceConfig& device) const {
    return link_options_for(config_, device);
}

void Daemon::rebuild_phones() {
    // Indexes into the old list end here.
    if (active_ >= 0)
        leave();
    std::vector<std::unique_ptr<Phone>> before = std::move(phones_);
    phones_.clear();
    // A phone the device thread is still opening belongs to the list that
    // is gone; install() recognises it by this number.
    ++generation_;
    for (size_t index = 0; index < config_.devices.size(); ++index) {
        const DeviceConfig& device = config_.devices[index];
        const uint16_t port = adb_port_of(config_, index);
        const LinkOptions options = link_options_of(device);
        std::unique_ptr<Phone> phone;
        // An open phone is not closed for a change that leaves how it was
        // opened as it is: placement, motion and values are applied to it
        // as it is.
        for (std::unique_ptr<Phone>& old : before) {
            if (old == nullptr || old->link < 0 || !device.enabled || device.serial.empty() ||
                old->serial != device.serial || old->port != port ||
                old->wants_proxy != device.adb_proxy ||
                old->link_options.buttons != options.buttons)
                continue;
            phone = std::move(old);
            break;
        }
        if (phone == nullptr)
            phone = std::make_unique<Phone>();
        phone->config = device;
        phone->port = port;
        // A phone that stays open is paced as the new files say, and has the
        // Nodes the files name.
        if (phone->link >= 0) {
            usb_.set_nodes(phone->link, options);
            phone->link_options = options;
            const ReportRate rate = report_rate_of(config_, device);
            usb_.set_rate(phone->link, rate.hz, rate.every);
        }
        if (phone->link < 0) {
            if (!device.enabled)
                phone->status = "disabled";
            else if (device.serial.empty())
                phone->status = "waits for a device: connect one to give it this profile";
        }
        phones_.push_back(std::move(phone));
    }
    for (const std::unique_ptr<Phone>& old : before) {
        if (old != nullptr)
            close_phone(*old);
    }
    relayout();
}

int Daemon::run(std::unique_ptr<Capture> capture) {
    // Each run starts the log over, so it never grows without bound.
    {
        const std::ofstream fresh(log_file(), std::ios::trunc);
    }
    const WriterScope files;
    config_error_ = load_config();
    if (!config_error_.empty())
        log_line(config_error_);
    static_cast<void>(read_file(state_file_path(), state_));
    last_used_ = state_get(state_, "last_used");

    std::string error = usb_.start();
    if (!error.empty()) {
        log_line(error);
        return 1;
    }

    // A backend may be unavailable only for now: the desktop's permission
    // dialog went unanswered, or the session is still coming up at login.
    // The daemon stays, says why through `status`, and tries again.
    const bool injected = capture != nullptr;
    for (;;) {
        CaptureEnv env;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            env.restore_token = state_get(state_, "restore_token");
        }
        env.save_restore_token = [this](const std::string& token) {
            save_state("restore_token", token);
        };
        std::vector<std::unique_ptr<Capture>> candidates;
        if (injected)
            candidates.push_back(std::move(capture));
        else
            candidates = capture_candidates(config_.backend, error);
        for (std::unique_ptr<Capture>& candidate : candidates) {
            error = candidate->start(*this, env);
            if (error.empty()) {
                capture_ = std::move(candidate);
                break;
            }
            error = std::string(candidate->name()) + ": " + error;
            log_line(error);
        }
        if (capture_ != nullptr)
            break;
        {
            const std::lock_guard<std::mutex> lock(state_mutex_);
            start_error_ = error;
        }
        if (injected)
            return 1;
        for (int step = 0; step < retry_steps && !quit_early_.load(); ++step)
            std::this_thread::sleep_for(retry_step);
        if (quit_early_.load()) {
            usb_.stop();
            return 0;
        }
    }
    log_line(std::string("capturing input with ") + capture_->name());
    monitors_ = capture_->monitors();
    rebuild_phones();

    watching_ = false;
    device_thread_ = std::thread([this] { device_loop(); });
    ready_.store(true, std::memory_order_release);
    // A quit that came while starting up had nothing to stop yet.
    if (!quit_early_.load(std::memory_order_acquire))
        capture_->run();

    finished_.store(true, std::memory_order_release);
    watch_.stop();
    {
        const std::lock_guard<std::mutex> lock(device_mutex_);
        stopping_ = true;
    }
    device_wake_.notify_all();
    device_thread_.join();

    // A reading through adb is ended, not waited for.
    fill_cancel_.store(true);
    if (fill_thread_.joinable())
        fill_thread_.join();
    if (active_ >= 0)
        leave();
    bool any_proxy = false;
    for (const std::unique_ptr<Phone>& phone : phones_) {
        any_proxy = any_proxy || phone->proxy;
        close_phone(*phone);
    }
    usb_.stop();
    // adb claims a phone only when it first appears, so its server has to
    // start over to see the phones this process served. Only done when one
    // was: adb is not otherwise touched.
    if (any_proxy && config_.adb_kill_server)
        Adb(config_.adb_path).kill_server();
    return exit_code_;
}

void Daemon::quit() {
    quit_early_.store(true, std::memory_order_release);
    if (ready_.load(std::memory_order_acquire))
        capture_->stop();
}

void Daemon::close_phone(Phone& phone) {
    if (phone.link >= 0) {
        usb_.close(phone.link);
    }
    phone.link = -1;
    phone.proxy = phone.wants_proxy = false;
    phone.session.reset();
    phone.layout_ok = phone.measured = false;
    phone.stage = Stage::absent;
    phone.status = "not connected";
}

void Daemon::request_scan() {
    {
        const std::lock_guard<std::mutex> lock(device_mutex_);
        wake_ = true;
        ++scans_requested_;
    }
    device_wake_.notify_one();
}

// Waits, a while at most, until every scan asked for so far has been made and
// its result published.
void Daemon::wait_scanned() {
    std::unique_lock<std::mutex> lock(device_mutex_);
    const uint64_t target = scans_requested_;
    scan_done_.wait_for(lock, std::chrono::seconds(3),
                        [&] { return scans_finished_ >= target || stopping_; });
}

void Daemon::device_loop() {
    bool first = true;
    // A scan was wanted while a phone had the input; it is made afterwards.
    bool postponed = false;
    // The adb server has to be stopped before a scan that was asked for, or
    // it keeps the phones it claimed out of sight.
    bool stop_adb = false;
    // The requests the next scan answers.
    uint64_t owed = 0;
    for (;;) {
        // A reload asks for a scan now, which the scan period does not hold
        // back. The first pass is a scan too: the daemon looks for devices
        // when it starts.
        bool asked = first;
        if (!first) {
            std::unique_lock<std::mutex> lock(device_mutex_);
            device_wake_.wait_for(lock, health_period, [this] { return stopping_ || wake_; });
            asked = wake_;
            wake_ = false;
            owed = scans_requested_;
            if (stopping_)
                return;
        }
        first = false;
        if (stopping_)
            return;

        struct Missing {
            size_t phone;
            DeviceConfig device;
            uint16_t port;
            uint64_t generation;
        };
        std::vector<Missing> missing;
        std::vector<std::string> claimed;
        Config config;
        bool remote = false;
        bool dropped = false;
        on_capture([&] {
            for (size_t index = 0; index < phones_.size(); ++index) {
                Phone& phone = *phones_[index];
                if (phone.link >= 0 && !usb_.alive(phone.link)) {
                    drop(index, usb_.failure(phone.link));
                    dropped = true;
                }
                if (phone.link >= 0)
                    claimed.push_back(phone.serial);
                else if (phone.config.enabled && !phone.config.serial.empty() &&
                         wanted_.count(phone.config.serial) != 0)
                    missing.push_back({index, phone.config, phone.port, generation_});
            }
            config = config_;
            remote = active_ >= 0;
        });

        if (!(asked || dropped || postponed))
            continue;
        stop_adb = stop_adb || asked;
        if (remote) {
            postponed = true;
            continue;
        }
        postponed = false;

        if (stop_adb && config.adb_kill_server && Adb(config.adb_path).kill_server())
            std::this_thread::sleep_for(adb_release_wait);
        stop_adb = false;

        std::string error;
        const std::vector<PhoneInfo> found = usb_.scan(error);

        on_capture([&] {
            for (size_t index = 0; index < phones_.size(); ++index) {
                Phone& phone = *phones_[index];
                if (phone.link >= 0) {
                    const bool still_present = std::any_of(
                        found.begin(), found.end(),
                        [&](const PhoneInfo& info) { return info.serial == phone.serial; });
                    if (!still_present)
                        drop(index, "unplugged");
                }
            }
            claimed.clear();
            missing.clear();
            for (size_t index = 0; index < phones_.size(); ++index) {
                Phone& phone = *phones_[index];
                if (phone.link >= 0) {
                    claimed.push_back(phone.serial);
                } else if (phone.config.enabled && !phone.config.serial.empty() &&
                           wanted_.count(phone.config.serial) != 0) {
                    missing.push_back({index, phone.config, phone.port, generation_});
                }
            }
        });

        for (const Missing& entry : missing) {
            for (size_t index = 0; index < found.size(); ++index) {
                const std::string& serial = found[index].serial;
                if (serial != entry.device.serial ||
                    std::find(claimed.begin(), claimed.end(), serial) != claimed.end())
                    continue;
                claimed.push_back(serial);
                Opened opened =
                    open_phone(entry.phone, index, config, entry.device, entry.port, serial);
                opened.generation = entry.generation;
                on_capture([&] { install(std::move(opened)); });
                break;
            }
        }
        // What is plugged in but has no file: shown, and left alone until
        // it is connected by hand.
        std::vector<PhoneInfo> unknown;
        for (const PhoneInfo& info : found) {
            const bool known = std::any_of(
                config.devices.begin(), config.devices.end(),
                [&](const DeviceConfig& device) { return device.serial == info.serial; });
            if (!known && valid_serial(info.serial))
                unknown.push_back(info);
        }
        on_capture([&] {
            plugged_.clear();
            for (const PhoneInfo& info : found)
                plugged_.insert(info.serial);
            new_ = std::move(unknown);
            publish_status();
        });
        {
            const std::lock_guard<std::mutex> lock(device_mutex_);
            scans_finished_ = std::max(scans_finished_, owed);
        }
        scan_done_.notify_all();
    }
}

Daemon::Opened Daemon::open_phone(const size_t phone, const size_t scan_index, const Config& config,
                                  const DeviceConfig& device, const uint16_t port,
                                  const std::string& serial) {
    Opened opened;
    opened.phone = phone;
    opened.serial = serial;
    opened.port = port;
    const LinkOptions options = link_options_for(config, device);
    opened.options = options;
    opened.wants_proxy = device.adb_proxy;
    opened.link = usb_.open(scan_index, options, opened.note);
    if (opened.link >= 0) {
        const ReportRate rate = report_rate_of(config, device);
        usb_.set_rate(opened.link, rate.hz, rate.every);
    }
    // The proxy is on only for a device it was switched on for by hand.
    if (opened.link < 0 || !device.adb_proxy)
        return opened;

    bool held = false;
    bool absent = false;
    int code = usb_.start_proxy(opened.link, port, held, absent);
    if (code != 0 && held && config.adb_kill_server) {
        // The adb server took the interface when the phone appeared. It lets
        // go a moment after it ends.
        Adb(config.adb_path).kill_server();
        for (int attempt = 0; attempt < proxy_retries && code != 0 && held; ++attempt) {
            std::this_thread::sleep_for(proxy_retry_step);
            code = usb_.start_proxy(opened.link, port, held, absent);
        }
    }
    if (code != 0) {
        // No ADB interface just means USB debugging is off.
        const std::string port_text = std::to_string(port);
        if (code == AOAHID_ADB_PROXY_ERR_BIND)
            opened.note = "port " + port_text + " is already in use; choose another";
        else if (code == AOAHID_ADB_PROXY_ERR_INTERFACE && held)
            opened.note = "the phone's ADB interface is held by another program";
        else if (code == AOAHID_ADB_PROXY_ERR_INTERFACE && absent)
            opened.note = "the phone has no ADB interface: turn USB debugging on";
        else
            opened.note = "the ADB proxy could not start on port " + port_text + " (code " +
                          std::to_string(code) + ")";
        return opened;
    }
    opened.proxy = true;
    return opened;
}

void Daemon::install(Opened opened) {
    // The list may have been rebuilt while the phone was being opened; what
    // was opened then is undone.
    const bool current = opened.generation == generation_ && opened.phone < phones_.size() &&
                         phones_[opened.phone]->config.serial == opened.serial &&
                         phones_[opened.phone]->link < 0;
    if (!current) {
        if (opened.link >= 0)
            usb_.close(opened.link);
        return;
    }
    Phone& phone = *phones_[opened.phone];
    if (opened.link < 0) {
        phone.status = opened.note;
        return;
    }
    phone.link = opened.link;
    phone.serial = opened.serial;
    phone.proxy = opened.proxy;
    phone.link_options = opened.options;
    phone.wants_proxy = opened.wants_proxy;
    phone.status = opened.note;
    phone.note = opened.note;
    phone.session = std::make_unique<Session>(usb_.sink(phone.link));
    phone.session->set_position_cell(phone.cell.get());
    phone.ready_at = now() + settle_seconds;
    relayout();
    log_line(one_line(label_of(phone.config)) + " (" + one_line(phone.serial) +
             "): " + (phone.layout_ok ? std::string("ready") : one_line(phone.status)));
}

void Daemon::drop(const size_t index, const std::string& why) {
    Phone& phone = *phones_[index];
    if (active_ == static_cast<int>(index))
        leave();
    // The phone is gone or stuck; there is nothing to write back to it.
    close_phone(phone);
    phone.status = why.empty() ? "disconnected" : "disconnected: " + why;
    log_line(one_line(label_of(phone.config)) + ": " + one_line(phone.status));
    relayout();
}

// What the phone itself is: its size, how it is held, and how far a count
// moves its cursor. Only the values in its file are used; nothing is read
// from the phone here. False, with what is missing in its status, when a
// value is.
bool Daemon::measure(Phone& phone) {
    const DeviceConfig& device = phone.config;
    std::string missing;
    const auto lack = [&](const char* name) {
        missing += (missing.empty() ? "" : ", ") + std::string(name);
    };
    if (device.width < 2)
        lack("width");
    if (device.height < 2)
        lack("height");
    if (device.rotation < 0)
        lack("rotation");
    if (device.diagonal_inch <= 0.0)
        lack("diagonal_inch");
    const double gain = effective_gain(device);
    if (gain <= 0.0)
        lack("gain, or the pointer speed and the density");
    if (device.accel == AccelSetting::automatic)
        lack("accel");
    if (!missing.empty()) {
        phone.status = "not set: " + missing +
                       ". Enter them, or turn the ADB proxy on for it and fill them from adb";
        return false;
    }
    phone.natural = {device.width, device.height};
    phone.rotation = device.rotation;
    phone.density = std::hypot(phone.natural.w, phone.natural.h) / (device.diagonal_inch * 25.4);
    phone.view = view_size(phone.natural, phone.rotation, device.mount_rotation);

    // How far one count moves the phone's cursor (docs/MATH.md, "Gain"): the
    // gain given is the one at low speed, and the only one with acceleration
    // off.
    phone.mode = device.accel == AccelSetting::on ? AccelMode::curve : AccelMode::exact;
    const double curve_scale = gain / curve_gain(device.pointer_speed, 0.0);
    phone.accel = AccelModel(phone.mode, device.pointer_speed, gain, gain, curve_scale, gain);
    phone.gain = phone.accel.slow_gain();
    return true;
}

bool Daemon::place_on_monitor(Phone& phone) {
    const DeviceConfig& device = phone.config;
    size_t monitor = 0;
    if (!device.monitor.empty()) {
        const auto found =
            std::find_if(monitors_.begin(), monitors_.end(),
                         [&](const Monitor& entry) { return entry.name == device.monitor; });
        if (found == monitors_.end()) {
            phone.status = "there is no monitor named " + device.monitor;
            return false;
        }
        monitor = static_cast<size_t>(found - monitors_.begin());
    }
    if (monitors_.empty()) {
        phone.status = "no monitor was found";
        return false;
    }
    const Rect& rect = monitors_[monitor].rect;
    // Pixels per millimetre along each axis: a monitor driven at a mode with
    // non-square pixels has two different ones. A diagonal given by hand
    // assumes square pixels; so does an axis the monitor reports no size for.
    double density_x = monitor_density(monitors_[monitor], true);
    double density_y = monitor_density(monitors_[monitor], false);
    if (device.monitor_diagonal_inch > 0.0)
        density_x = density_y = std::hypot(rect.w, rect.h) / (device.monitor_diagonal_inch * 25.4);
    if (density_x <= 0.0)
        density_x = density_y;
    if (density_y <= 0.0)
        density_y = density_x;
    const bool need_density = device.segment_length < 0;
    if (density_x <= 0.0 && need_density) {
        phone.status = "the monitor's physical size is not known: set monitor_diagonal_inch";
        return false;
    }
    // The density along the shared edge sizes the device against it.
    const double pc_density = vertical(device.side) ? density_y : density_x;

    PortalRequest request;
    request.monitor = monitor;
    request.side = device.side;
    request.android_length = vertical(device.side) ? phone.view.h : phone.view.w;
    request.anchor_length =
        device.segment_length > 0
            ? device.segment_length
            : static_cast<int>(std::lround(request.android_length / phone.density * pc_density));
    // The config counts from the start of the monitor's edge, so a monitor
    // left of or above the origin needs no negative numbers.
    request.centered = device.segment_start == centred_start;
    request.anchor_start = (vertical(device.side) ? rect.y : rect.x) + device.segment_start;
    request.corner_margin = device.corner_margin;
    const std::string error = resolve_portal(monitors_, request, phone.portal);
    if (!error.empty()) {
        phone.status = error;
        return false;
    }
    phone.has_pc = true;
    return true;
}

// Beside another phone: the same arithmetic as beside a monitor, with that
// phone's display in the monitor's place. `beside` keeps the span of the
// parent's edge this phone covers, in the parent's pixels.
bool Daemon::place_beside(Phone& phone, const Phone& parent) {
    const DeviceConfig& device = phone.config;
    const bool is_vertical = vertical(device.side);
    const int parent_length = is_vertical ? parent.view.h : parent.view.w;
    const int own_length = is_vertical ? phone.view.h : phone.view.w;
    const int length =
        device.segment_length > 0
            ? device.segment_length
            : static_cast<int>(std::lround(own_length / phone.density * parent.density));
    if (length < 2) {
        phone.status = "it is too short beside " + label_of(parent.config);
        return false;
    }
    const int start =
        device.segment_start == centred_start ? (parent_length - length) / 2 : device.segment_start;
    Portal& beside = phone.beside;
    beside.monitor = 0;
    beside.side = device.side;
    beside.anchor = {start, start + length - 1};
    beside.segment = {std::max(start, 0), std::min(start + length - 1, parent_length - 1)};
    beside.android_length = own_length;
    if (beside.segment.hi - beside.segment.lo < 1) {
        phone.status = "it does not touch " + label_of(parent.config) + " there";
        return false;
    }
    phone.has_pc = false;
    return true;
}

void Daemon::set_motion(Phone& phone) {
    const Motion wanted = motion_of(config_, phone.config);
    MotionConfig& motion = phone.motion;
    motion = {};
    // The movement is sent as the backend reports it, times the sensitivity:
    // the gain is not part of it (docs/MATH.md, "Sensitivity").
    motion.scale_x = wanted.sensitivity;
    motion.scale_y = wanted.sensitivity_y > 0.0 ? wanted.sensitivity_y : wanted.sensitivity;
    motion.scroll_scale = wanted.scroll_sensitivity;
    motion.always_corner = wanted.entry == EntryMode::corner;
    motion.park = wanted.entry == EntryMode::parked;
    motion.resync_width = wanted.resync_width;
    motion.enter_push = wanted.enter_push;
    motion.return_push = wanted.return_push;
    motion.no_cross_while_button = wanted.no_cross_while_button;
    motion.buttons = config_.mouse_buttons;
}

void Daemon::relayout() {
    // Every session is configured again below, which forgets where its
    // cursor is; none may have the input while that happens.
    if (active_ >= 0)
        leave();
    for (const std::unique_ptr<Phone>& phone : phones_) {
        phone->layout_ok = phone->has_pc = false;
        phone->parent = -1;
        phone->measured = phone->link >= 0 && phone->session != nullptr && measure(*phone);
        if (phone->link >= 0)
            phone->stage = Stage::error;
    }
    for (const std::unique_ptr<Phone>& phone : phones_) {
        if (phone->measured && phone->config.beside.empty())
            phone->layout_ok = place_on_monitor(*phone);
    }
    // A phone beside another is placed once that one is; a chain takes as
    // many rounds as it is long.
    for (bool progress = true; progress;) {
        progress = false;
        for (const std::unique_ptr<Phone>& phone : phones_) {
            if (!phone->measured || phone->layout_ok || phone->config.beside.empty())
                continue;
            const size_t parent = find_device(config_, phone->config.beside);
            if (parent >= phones_.size() || !phones_[parent]->layout_ok)
                continue;
            if (place_beside(*phone, *phones_[parent])) {
                phone->parent = static_cast<int>(parent);
                phone->layout_ok = true;
                progress = true;
            } else {
                // Its status says why; it is not tried again this round.
                phone->measured = false;
            }
        }
    }
    for (size_t index = 0; index < phones_.size(); ++index) {
        Phone& phone = *phones_[index];
        if (!phone.layout_ok) {
            if (phone.measured && !phone.config.beside.empty())
                phone.status =
                    "it sits beside " + phone.config.beside + ", which is not connected and ready";
            continue;
        }
        set_motion(phone);
        std::vector<Neighbour> neighbours;
        if (phone.parent >= 0) {
            // Toward its parent: all of the part of its own edge that touches.
            const Portal& map = phone.beside;
            neighbours.push_back({opposite(phone.config.side),
                                  {static_cast<int>(std::floor(map.to_android(map.segment.lo))),
                                   static_cast<int>(std::ceil(map.to_android(map.segment.hi)))},
                                  phone.parent});
        }
        for (size_t other = 0; other < phones_.size(); ++other) {
            const Phone& child = *phones_[other];
            if (child.layout_ok && child.parent == static_cast<int>(index))
                neighbours.push_back(
                    {child.config.side, child.beside.segment, static_cast<int>(other)});
        }
        phone.session->configure(phone.portal, phone.view, phone.config.mount_rotation,
                                 phone.motion, phone.accel, std::move(neighbours), phone.has_pc);
        const Touch touch = touch_of(config_, phone.config);
        TouchSetup setup;
        setup.enabled = touch.enabled && phone.link_options.touch;
        setup.natural = phone.natural;
        setup.rotation = phone.rotation;
        setup.button = touch.button;
        setup.swipe = touch.swipe;
        setup.scroll = touch.scroll;
        setup.scroll_pan = touch.scroll_pan;
        setup.release_s = touch.release_ms / 1000.0;
        setup.start_s = touch.start_ms / 1000.0;
        setup.steps = touch.steps;
        setup.total_s = touch.total_ms / 1000.0;
        setup.restart = touch.restart;
        phone.session->set_touch(setup);
        phone.stage = Stage::ready;
        phone.status.clear();
        if (!phone.note.empty())
            phone.status = phone.note;
    }
    apply_barriers();
    publish_status();
}

// "fill" read this device through adb: what could be read is written into its
// file. Nothing is made up; what the phone did not tell is said in the
// answer, and left as it was.
void Daemon::finish_fill(const std::string& serial, const uint64_t generation, const bool read,
                         const PhoneFacts& facts, const std::string& problem) {
    fill_running_.store(false);
    publish_status();
    const size_t index = find_device(config_, serial);
    if (generation != generation_ || index >= phones_.size() || phones_[index]->link < 0 ||
        phones_[index]->serial != serial || index >= config_.devices.size())
        return;
    Phone& phone = *phones_[index];
    const auto report = [&](const std::string& text, const bool ok) {
        phone.fill_note = text;
        phone.fill_ok = ok;
        log_line(one_line(label_of(phone.config)) + ": " + one_line(text));
        publish_status();
    };
    if (!read) {
        report("could not be read through adb: " + problem, false);
        return;
    }

    DeviceConfig device = config_.devices[index];
    const DeviceConfig before = device;
    const std::vector<std::string> missing = fill_device(facts, device);
    if (device.name.empty()) {
        // Its own name for itself, made different from the others'.
        // The device said this itself: only what a name may hold is kept.
        std::string base = clean_name(facts.model).substr(0, 40);
        if (base.empty())
            base = phone.serial.substr(0, 40);
        std::string name = base;
        Config probe = config_;
        for (int count = 2; count < 100; ++count) {
            probe.devices[index].name = name;
            if (validate_config(probe).empty())
                break;
            name = base + " " + std::to_string(count);
        }
        device.name = name;
    }
    char day[16] = "";
    const std::time_t time = std::time(nullptr);
    if (const std::tm* local = std::localtime(&time))
        std::strftime(day, sizeof day, "%Y-%m-%d", local);
    device.read_at = day;
    // All of this came from the device. It is only kept if the file it
    // makes reads back as itself and the config as a whole still holds;
    // otherwise the device's file would stop the config from loading.
    const auto sound = [&] {
        DeviceConfig back;
        Config probe = config_;
        probe.devices[index] = device;
        return parse_device(format_device(device), back).empty() &&
               format_device(back) == format_device(device) && validate_config(probe).empty();
    };
    if (!sound())
        device.name = before.name;
    if (!sound()) {
        report("what the phone reported is out of range, so nothing was saved", false);
        return;
    }
    config_.devices[index] = device;
    phone.config = device;
    // Whoever shows the files is told they changed (status: files_version).
    ++files_version_;
    if (!save_store(config_)) {
        report("what was read could not be written to " + device_file_path(phone.serial), false);
        return;
    }
    std::string text = "read " + std::to_string(device.width) + " x " +
                       std::to_string(device.height) + " and saved it";
    if (device.accel == AccelSetting::off && !facts.accel_gain_known)
        text += "; the gain is calculated, and has not been measured for this phone's switch";
    if (!missing.empty()) {
        text += "; not given by the phone: ";
        for (size_t at = 0; at < missing.size(); ++at)
            text += (at != 0 ? ", " : "") + missing[at];
    }
    relayout();
    report(text, missing.empty());
}

std::string Daemon::start_fill(const size_t index) {
    if (index >= phones_.size() || index >= config_.devices.size())
        return "error=no such device";
    Phone& phone = *phones_[index];
    if (phone.link < 0)
        return "error=the device is not connected";
    if (!phone.proxy)
        return "error=turn the ADB proxy on for this device first, then apply";
    if (fill_running_.load())
        return "error=a device is being read already";
    if (fill_thread_.joinable())
        fill_thread_.join();
    phone.fill_note.clear();
    fill_running_.store(true);
    fill_cancel_.store(false);
    fill_serial_ = phone.serial;
    publish_status();
    // adb takes seconds: not on the capture thread.
    fill_thread_ = std::thread([this, path = config_.adb_path, port = phone.port,
                                serial = phone.serial, generation = generation_] {
        const Adb adb(path);
        std::string problem;
        PhoneFacts facts;
        bool read = adb.available();
        bool created = false;
        if (!read)
            problem = "adb could not be run: install it, or set [adb] path";
        else
            read = adb.connect(port, problem, &fill_cancel_, &created);
        if (read) {
            facts = adb.inspect(port);
            read = facts.reachable;
            if (!read)
                problem = facts.note;
        }
        // The connection this reading made ends with it: adb's server would
        // otherwise keep one open to the proxy for as long as it runs, and
        // the next reading would meet it stale. A connection the user made
        // themselves is left alone.
        if (created)
            adb.disconnect(port);
        capture_->post([this, serial, generation, read, facts = std::move(facts),
                        problem = std::move(problem)] {
            finish_fill(serial, generation, read, facts, problem);
        });
    });
    return "ok";
}

void Daemon::apply_barriers() {
    if (capture_ == nullptr)
        return;
    std::vector<Barrier> barriers;
    for (size_t index = 0; index < phones_.size(); ++index) {
        const Phone& phone = *phones_[index];
        if (!phone.layout_ok || !phone.has_pc)
            continue;
        barriers.push_back({static_cast<uint32_t>(index + 1), phone.portal.monitor,
                            phone.portal.side, phone.portal.segment});
    }
    capture_error_ = capture_->set_barriers(barriers);
}

void Daemon::monitors_changed() {
    if (active_ >= 0)
        leave();
    monitors_ = capture_->monitors();
    relayout();
}

bool Daemon::edge_hit(const uint32_t barrier, const int t, const double push,
                      const bool button_down) {
    if (paused_ || active_ >= 0 || barrier == 0 || barrier > phones_.size())
        return false;
    Phone& phone = *phones_[barrier - 1];
    const double time = now();
    if (!phone.layout_ok || !usb_.alive(phone.link) || time < phone.ready_at)
        return false;
    if (!phone.session->edge_hit(t, push, button_down, time))
        return false;
    remember(barrier - 1);
    // A backend that cannot see keys on the PC side has no idea what is held.
    if (!capture_->sees_local_keys())
        modifiers_ = 0;
    return true;
}

void Daemon::leave() {
    if (active_ < 0)
        return;
    const Phone* phone = phones_[static_cast<size_t>(active_)].get();
    active_ = -1;
    int t = phone->session->leave();
    // A phone beside another has no edge on a monitor: the cursor comes
    // back at the middle of the edge its chain starts from.
    while (!phone->has_pc && phone->parent >= 0) {
        phone = phones_[static_cast<size_t>(phone->parent)].get();
        t = (phone->portal.segment.lo + phone->portal.segment.hi) / 2;
    }
    if (!phone->has_pc || phone->portal.monitor >= monitors_.size()) {
        if (!monitors_.empty())
            capture_->release(monitors_[0].rect.x + monitors_[0].rect.w / 2,
                              monitors_[0].rect.y + monitors_[0].rect.h / 2);
        return;
    }
    // One pixel in from the edge, so the cursor does not cross again at once.
    const Rect& rect = monitors_[phone->portal.monitor].rect;
    int x = t;
    int y = t;
    switch (phone->portal.side) {
    case Side::left:
        x = rect.x + 1;
        break;
    case Side::right:
        x = rect.x + rect.w - 2;
        break;
    case Side::top:
        y = rect.y + 1;
        break;
    case Side::bottom:
        y = rect.y + rect.h - 2;
        break;
    }
    capture_->release(x, y);
}

void Daemon::remember(const size_t phone) {
    active_ = static_cast<int>(phone);
    const std::string& serial = phones_[phone]->serial;
    if (serial == last_used_)
        return;
    last_used_ = serial;
    save_state("last_used", serial);
    publish_status();
}

void Daemon::hop(const size_t from, const size_t to) {
    if (to >= phones_.size())
        return;
    Phone& here = *phones_[from];
    Phone& there = *phones_[to];
    // Not ready: its edge stays a wall.
    if (!there.layout_ok || !usb_.alive(there.link))
        return;
    Side edge = Side::left;
    double along = 0.0;
    if (there.parent == static_cast<int>(from)) {
        // Outward, to a phone that sits beside this one.
        const double at = here.session->along(there.config.side);
        along = there.beside.to_android(static_cast<int>(std::lround(at)));
        edge = opposite(there.config.side);
    } else if (here.parent == static_cast<int>(to)) {
        // Back, to the phone this one sits beside.
        const double at = here.session->along(opposite(here.config.side));
        along = here.beside.to_pc(at);
        edge = here.config.side;
    } else {
        return;
    }
    static_cast<void>(here.session->leave());
    remember(to);
    there.session->enter_at(edge, along, now());
}

void Daemon::motion(const double dx, const double dy) {
    if (active_ < 0)
        return;
    Session& session = *phones_[static_cast<size_t>(active_)]->session;
    if (!session.motion(dx, dy, now()))
        return;
    if (session.exit() == Session::to_pc)
        leave();
    else
        hop(static_cast<size_t>(active_), static_cast<size_t>(session.exit()));
}

void Daemon::scroll(const double wheel, const double pan) {
    if (active_ >= 0)
        phones_[static_cast<size_t>(active_)]->session->scroll(wheel, pan);
}

void Daemon::button(unsigned button, const bool down) {
    if (active_ < 0)
        return;
    if (button >= 1 && button <= config_.button_map.size())
        button = config_.button_map[button - 1];
    phones_[static_cast<size_t>(active_)]->session->button(button, down);
}

bool Daemon::hotkey(const HidKey key, const bool down, const bool grabbed) {
    if (key.media)
        return false;
    const uint8_t modifier = modifier_bit(key.usage);
    if (modifier != 0) {
        modifiers_ = static_cast<uint8_t>(down ? modifiers_ | modifier : modifiers_ & ~modifier);
        return false;
    }
    const auto pressed = [&](const Hotkey& hotkey) {
        return hotkey.set() && hotkey.usage == key.usage && hotkey.modifiers == modifiers_;
    };
    // A media hotkey works wherever the input is. Its release is matched by
    // the key alone: the modifiers are often let go first.
    if (!down && held_media_usage_ != 0 && key.usage == held_media_key_) {
        static_cast<void>(send_media(held_media_usage_, false));
        held_media_usage_ = held_media_key_ = 0;
        return true;
    }
    for (size_t index = 0; index < media_key_count; ++index) {
        if (!pressed(media_hotkeys_[index]))
            continue;
        if (down && held_media_usage_ == 0 && send_media(media_keys[index].usage, true)) {
            held_media_key_ = key.usage;
            held_media_usage_ = media_keys[index].usage;
        }
        return true;
    }
    // A device's own hotkey takes the input straight to it, from the PC or
    // from another device.
    for (size_t index = 0; index < device_hotkeys_.size() && index < phones_.size(); ++index) {
        if (!pressed(device_hotkeys_[index]))
            continue;
        if (down)
            static_cast<void>(enter_by_command(index));
        return true;
    }
    if (!pressed(toggle_) && !pressed(panic_) && !pressed(pause_) && !pressed(resync_))
        return false;
    if (!down)
        return true;
    if (pressed(panic_)) {
        leave();
    } else if (pressed(toggle_)) {
        if (grabbed)
            leave();
        else
            static_cast<void>(enter_by_command(usual_phone()));
    } else if (pressed(pause_)) {
        paused_ = !paused_;
        if (paused_)
            leave();
    } else {
        for (const std::unique_ptr<Phone>& phone : phones_) {
            if (phone->session != nullptr)
                phone->session->resync();
        }
    }
    return true;
}

size_t Daemon::phone_named(const std::string& name) const { return find_device(config_, name); }

size_t Daemon::usual_phone() const {
    const auto ready = [&](const size_t index) {
        return index < phones_.size() && phones_[index]->layout_ok;
    };
    const size_t last = last_used_.empty() ? phones_.size() : find_device(config_, last_used_);
    if (ready(last))
        return last;
    for (size_t index = 0; index < phones_.size(); ++index) {
        if (ready(index))
            return index;
    }
    return phones_.size();
}

bool Daemon::send_media(const uint16_t usage, const bool down) {
    // The device the config names; else the one with the input; else the
    // one that had it last, or any that is connected.
    size_t target =
        config_.media_target.empty() ? phones_.size() : find_device(config_, config_.media_target);
    if (target >= phones_.size() && active_ >= 0)
        target = static_cast<size_t>(active_);
    if (target >= phones_.size())
        target = usual_phone();
    for (size_t index = 0; target >= phones_.size() && index < phones_.size(); ++index) {
        if (phones_[index]->link >= 0)
            target = index;
    }
    if (target >= phones_.size() || phones_[target]->link < 0)
        return false;
    usb_.sink(phones_[target]->link).media(usage, down);
    return true;
}

void Daemon::key(const HidKey key, const bool down, const bool grabbed) {
    // A hotkey's own key never reaches the phone.
    if (hotkey(key, down, grabbed) || !grabbed || active_ < 0)
        return;
    Session& session = *phones_[static_cast<size_t>(active_)]->session;
    if (key.media)
        session.media(key.usage, down);
    else if (keyboard_of(config_, phones_[static_cast<size_t>(active_)]->config))
        session.key(key.usage, down);
}

void Daemon::lost() {
    if (active_ < 0)
        return;
    // The system already took the input back; only the phone needs tidying.
    static_cast<void>(phones_[static_cast<size_t>(active_)]->session->leave());
    active_ = -1;
}

std::string Daemon::enter_by_command(const size_t index) {
    if (index >= phones_.size())
        return "error=no such device";
    Phone& phone = *phones_[index];
    if (active_ == static_cast<int>(index))
        return "ok";
    if (!phone.layout_ok || !usb_.alive(phone.link))
        return "error=the device is not ready";
    if (active_ >= 0) {
        // From another device: the input is held already, and that
        // device's cursor is put away.
        static_cast<void>(phones_[static_cast<size_t>(active_)]->session->leave());
    } else if (!capture_->grab()) {
        return "error=this backend only crosses at the screen edge";
    }
    if (phone.has_pc) {
        phone.session->enter((phone.portal.segment.lo + phone.portal.segment.hi) / 2, now());
    } else {
        // Through the edge it shares with the phone it sits beside.
        const Portal& map = phone.beside;
        phone.session->enter_at(opposite(phone.config.side),
                                map.to_android((map.segment.lo + map.segment.hi) / 2), now());
    }
    remember(index);
    // The modifiers of the hotkey that led here are still held, so their
    // state is kept; the same hotkey then brings the input back.
    return "ok";
}

void Daemon::publish_status() {
    if (capture_ == nullptr)
        return;
    auto view = std::make_shared<StatusView>();
    std::ostringstream out;
    out << "backend=" << capture_->name() << "\n";
    if (!last_used_.empty())
        out << "last_used=" << one_line(last_used_) << "\n";
    if (!config_.media_target.empty())
        out << "media_target=" << one_line(config_.media_target) << "\n";
    if (!config_error_.empty())
        out << "config_error=" << one_line(config_error_) << "\n";
    if (!capture_error_.empty())
        out << "capture_error=" << one_line(capture_error_) << "\n";
    out << "local_hotkeys=" << (capture_->sees_local_keys() ? "yes" : "no") << "\n";
    for (size_t index = 0; index < monitors_.size(); ++index) {
        const Monitor& monitor = monitors_[index];
        out << "monitor." << index + 1 << "=" << monitor.name << " " << monitor.rect.x << " "
            << monitor.rect.y << " " << monitor.rect.w << " " << monitor.rect.h << " "
            << monitor.width_mm << " " << monitor.height_mm << "\n";
    }
    // What is plugged in and has no file yet.
    for (const PhoneInfo& info : new_)
        out << "new." << one_line(info.serial) << "=" << one_line(info.product) << "\n";
    view->head = out.str();
    view->fill_serial = fill_serial_;

    for (const std::unique_ptr<Phone>& phone : phones_) {
        PhoneView entry;
        entry.serial = phone->config.serial;
        entry.link = phone->link;
        entry.placed = phone->layout_ok && phone->link >= 0;
        entry.cell = phone->cell;
        entry.touch_on = touch_of(config_, phone->config).enabled;
        // A profile no device has taken yet has no serial to be listed by.
        if (!entry.serial.empty()) {
            std::ostringstream lines;
            const std::string prefix = "device." + one_line(entry.serial) + ".";
            const char* stage = phone->stage == Stage::ready    ? "ready"
                                : phone->stage == Stage::absent ? "absent"
                                                                : "error";
            lines << prefix << "name=" << one_line(label_of(phone->config)) << "\n";
            lines << prefix << "state=" << stage << "\n";
            lines << prefix << "wanted=" << (wanted_.count(entry.serial) != 0 ? "yes" : "no")
                  << "\n";
            lines << prefix << "plugged=" << (plugged_.count(entry.serial) != 0 ? "yes" : "no")
                  << "\n";
            lines << prefix << "touch=" << touch_state(config_, phone->config) << "\n";
            if (!phone->fill_note.empty())
                lines << prefix << "fill=" << (phone->fill_ok ? "ok " : "error ")
                      << one_line(phone->fill_note) << "\n";
            if (!phone->status.empty())
                lines << prefix << "status=" << one_line(phone->status) << "\n";
            if (phone->link >= 0) {
                lines << prefix << "serial=" << one_line(phone->serial) << "\n";
                lines << prefix << "proxy=" << (phone->proxy ? std::to_string(phone->port) : "off")
                      << "\n";
                if (phone->natural.w > 0)
                    lines << prefix << "size=" << phone->natural.w << "x" << phone->natural.h
                          << "\n";
                if (phone->rotation >= 0)
                    lines << prefix << "rotation=" << phone->rotation << "\n";
                if (phone->density > 0.0)
                    lines << prefix << "density=" << phone->density << "\n";
                if (phone->layout_ok) {
                    lines << prefix << "mode=" << mode_name(phone->mode) << "\n";
                    lines << prefix << "gain=" << phone->gain << "\n";
                    if (phone->has_pc) {
                        lines << prefix << "segment=" << monitors_[phone->portal.monitor].name
                              << " " << side_text(phone->portal.side) << " "
                              << phone->portal.segment.lo << " " << phone->portal.segment.hi << " "
                              << phone->portal.anchor.lo << " " << phone->portal.anchor.hi << "\n";
                    } else {
                        lines << prefix << "beside=" << one_line(phone->config.beside) << " "
                              << side_text(phone->config.side) << " " << phone->beside.segment.lo
                              << " " << phone->beside.segment.hi << " " << phone->beside.anchor.lo
                              << " " << phone->beside.anchor.hi << "\n";
                    }
                }
            }
            entry.lines = lines.str();
        }
        view->phones.push_back(std::move(entry));
    }
    const std::lock_guard<std::mutex> lock(view_mutex_);
    view_ = std::move(view);
}

// The status, from the last published view and what the links count since.
// Called from any thread.
std::string Daemon::render_status() {
    std::shared_ptr<const StatusView> view;
    {
        const std::lock_guard<std::mutex> lock(view_mutex_);
        view = view_;
    }
    if (view == nullptr)
        return "state=starting\n";
    std::ostringstream out;
    out << "protocol=2\n";
    const int active = active_.load();
    out << "state=" << (active >= 0 ? "android" : paused_.load() ? "paused" : "pc") << "\n";
    if (active >= 0 && static_cast<size_t>(active) < view->phones.size())
        out << "active=" << one_line(view->phones[static_cast<size_t>(active)].serial) << "\n";
    out << "files_version=" << files_version_.load() << "\n";
    out << view->head;
    const bool reading = fill_running_.load();
    for (const PhoneView& phone : view->phones) {
        if (phone.serial.empty())
            continue;
        const std::string prefix = "device." + one_line(phone.serial) + ".";
        if (reading && view->fill_serial == phone.serial)
            out << prefix << "reading=1\n";
        out << phone.lines;
        if (phone.link < 0)
            continue;
        const LinkStats stats = usb_.stats(phone.link);
        out << prefix << "reports=" << stats.reports << "\n";
        out << prefix << "merged=" << stats.merged << "\n";
        out << prefix << "depth=" << stats.depth << "\n";
        out << prefix << "realtime=" << (stats.realtime ? "yes" : "no") << "\n";
        out << prefix << "samples=" << stats.samples << "\n";
        out << prefix << "queue_us=" << stats.queue_p50 << " " << stats.queue_p99 << " "
            << stats.queue_max << "\n";
        out << prefix << "total_us=" << stats.total_p50 << " " << stats.total_p99 << " "
            << stats.total_max << "\n";
        if (phone.placed && phone.cell != nullptr) {
            const PositionCell::Value at = phone.cell->load();
            out << prefix << "position=" << at.x_lo << " " << at.x_hi << " " << at.y_lo << " "
                << at.y_hi << "\n";
            if (phone.touch_on)
                out << prefix << "touch_error=" << (at.x_hi - at.x_lo) << " " << (at.y_hi - at.y_lo)
                    << "\n";
        }
    }
    return out.str();
}

std::string Daemon::command(const std::string& line) {
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    std::string answer = "error=unknown command";

    // The control endpoint opens before the capture backend is up (asking
    // for permission can take a while); nothing below may touch it yet.
    if (!ready_.load(std::memory_order_acquire) && verb != "quit") {
        if (verb != "status")
            return "error=the daemon is still starting";
        const std::lock_guard<std::mutex> lock(state_mutex_);
        return start_error_.empty()
                   ? "state=starting\n"
                   : "state=starting\ncapture_error=" + one_line(start_error_) + "\n";
    }

    if (verb == "quit") {
        quit();
        return "ok";
    }
    if (verb == "buttons") {
        // Reads sysfs; no need to bother the capture thread.
        const unsigned buttons = count_mouse_buttons();
        return buttons != 0 ? "buttons=" + std::to_string(buttons)
                            : std::string("error=no mouse could be examined");
    }

    // The rest of a command line, without the spaces around it.
    const auto rest_of = [](std::istringstream& from) {
        std::string text;
        std::getline(from, text);
        const size_t first = text.find_first_not_of(" \t");
        const size_t last = text.find_last_not_of(" \t\r");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    };
    // The device thread scans (it also opens by what it found); a scan here
    // could replace the list it is about to open from.
    const auto rescan_now = [this]() { request_scan(); };
    const auto reload = [this, &rescan_now]() -> std::string {
        // The new files have to read well before anything is changed. A phone
        // that is open stays open unless how it was opened has changed.
        Config loaded;
        config_error_ = load_store(loaded);
        if (!config_error_.empty()) {
            publish_status();
            return "error=" + config_error_;
        }
        adopt_config(std::move(loaded));
        rebuild_phones();
        rescan_now();
        return "ok";
    };

    // Answered from what the capture thread last published: asking it would
    // hold the input up while the answer is written.
    if (verb == "status")
        return render_status();

    on_capture([&] {
        if (verb == "release") {
            leave();
            answer = "ok";
        } else if (verb == "enter") {
            // A name may hold spaces: it is the rest of the line. None means
            // the device that had the input last.
            const std::string name = rest_of(words);
            answer = enter_by_command(name.empty() ? usual_phone() : phone_named(name));
        } else if (verb == "pause" || verb == "resume") {
            paused_ = verb == "pause";
            if (paused_)
                leave();
            answer = "ok";
        } else if (verb == "resync") {
            for (const std::unique_ptr<Phone>& phone : phones_) {
                if (phone->session != nullptr)
                    phone->session->resync();
            }
            answer = "ok";
        } else if (verb == "media") {
            // One press of a media key, for a desktop shortcut to run where
            // the backend cannot see the keyboard itself.
            std::string name;
            words >> name;
            // "media KEY NAME" sends it to that device this once.
            const std::string device = rest_of(words);
            const std::string usual = config_.media_target;
            if (!device.empty())
                config_.media_target = device;
            answer = "error=media previous|play_pause|next|brightness_up|brightness_down";
            if (!device.empty() && phone_named(device) >= phones_.size())
                answer = "error=no such device";
            for (const MediaKey& each : media_keys) {
                if (name != each.name || (!device.empty() && phone_named(device) >= phones_.size()))
                    continue;
                if (send_media(each.usage, true)) {
                    static_cast<void>(send_media(each.usage, false));
                    answer = "ok";
                } else {
                    answer = "error=no device is connected";
                }
            }
            config_.media_target = usual;
        } else if (verb == "probe") {
            // Moves a connected device's cursor by raw counts, for measuring
            // its display by eye when adb cannot be asked (docs/MATH.md).
            // "probe NAME corner" or "probe NAME move DX DY"; a name may
            // hold spaces, so it is what is left of the line.
            std::string name = rest_of(words);
            std::string what;
            Delta counts{-probe_reach, -probe_reach};
            bool known = false;
            if (name.size() > 7 && name.compare(name.size() - 7, 7, " corner") == 0) {
                what = "corner";
                name.erase(name.size() - 7);
                known = true;
            } else {
                const size_t move = name.rfind(" move ");
                if (move != std::string::npos) {
                    std::istringstream numbers(name.substr(move + 6));
                    what = "move";
                    known = static_cast<bool>(numbers >> counts.x >> counts.y) &&
                            (numbers >> std::ws).eof();
                    name.erase(move);
                }
            }
            const size_t index = phone_named(name);
            Phone* phone = index < phones_.size() && phones_[index]->link >= 0
                               ? phones_[index].get()
                               : nullptr;
            if (phone == nullptr) {
                answer = "error=that device is not connected";
            } else if (active_ >= 0) {
                answer = "error=bring the input back first";
            } else if (!known || std::abs(counts.x) > probe_reach ||
                       std::abs(counts.y) > probe_reach) {
                answer = "error=probe DEVICE corner | probe DEVICE move DX DY";
            } else {
                usb_.sink(phone->link).jump(counts);
                // The cursor is no longer where the session left it.
                if (phone->session != nullptr)
                    phone->session->resync();
                answer = "ok";
            }
        } else if (verb == "reports") {
            // What the last reports to a connected device carried, one line
            // each. Asking switches the recording on for a few seconds.
            const size_t index = phone_named(rest_of(words));
            if (index >= phones_.size() || phones_[index]->link < 0) {
                answer = "error=that device is not connected";
            } else {
                answer = "ok\n";
                for (const std::string& line : usb_.trace(phones_[index]->link))
                    answer += line + "\n";
            }
        } else if (verb == "connect") {
            // Opens a device. One that has a file is opened as it is; a
            // plugged-in device that has none is given a file first (nothing
            // is read from it). Nothing else is touched, and nothing is
            // opened until this is asked for.
            const std::string name = rest_of(words);
            const size_t known = phone_named(name);
            if (known < config_.devices.size()) {
                const DeviceConfig& device = config_.devices[known];
                if (device.serial.empty()) {
                    answer = "error=it waits for a device: connect a plugged-in one to give it "
                             "this profile";
                } else if (!device.enabled) {
                    answer = "error=the device is turned off";
                } else {
                    wanted_.insert(device.serial);
                    request_scan();
                    answer = "ok";
                }
            } else {
                const auto plugged =
                    std::find_if(new_.begin(), new_.end(),
                                 [&](const PhoneInfo& info) { return info.serial == name; });
                if (!valid_serial(name)) {
                    // It names a file: nothing but a plain serial gets that far.
                    answer = "error=connect SERIAL|NAME";
                } else if (plugged == new_.end()) {
                    answer = "error=no such device, and none with that serial is plugged in";
                } else {
                    if (active_ >= 0)
                        leave();
                    Config next = config_;
                    // A profile that names no device is this
                    // one's now; otherwise it starts from nothing.
                    size_t index = next.devices.size();
                    for (size_t i = 0; i < next.devices.size() && index == next.devices.size();
                         ++i) {
                        if (next.devices[i].serial.empty())
                            index = i;
                    }
                    if (index == next.devices.size())
                        next.devices.emplace_back();
                    DeviceConfig& device = next.devices[index];
                    device.serial = name;
                    device.enabled = true;
                    const std::string problem = validate_config(next);
                    if (!problem.empty()) {
                        answer = "error=" + problem;
                    } else if (!save_store(next)) {
                        answer = "error=the device's file could not be written";
                    } else {
                        wanted_.insert(name);
                        answer = reload();
                    }
                }
            }
        } else if (verb == "disconnect") {
            // Closes a device and leaves it closed, also when it is plugged
            // in again.
            const size_t index = phone_named(rest_of(words));
            if (index >= phones_.size()) {
                answer = "error=no such device";
            } else {
                Phone& phone = *phones_[index];
                wanted_.erase(phone.config.serial);
                if (phone.link >= 0) {
                    if (active_ == static_cast<int>(index))
                        leave();
                    close_phone(phone);
                    relayout();
                }
                publish_status();
                answer = "ok";
            }
        } else if (verb == "fill") {
            // Reads one connected device through its ADB proxy and writes
            // what it says into its file.
            answer = start_fill(phone_named(rest_of(words)));
        } else if (verb == "forget") {
            const size_t index = phone_named(rest_of(words));
            if (index >= config_.devices.size() || index >= phones_.size()) {
                answer = "error=no such device";
            } else {
                if (active_ >= 0)
                    leave();
                Config next = config_;
                const std::string forgotten_serial = config_.devices[index].serial;
                wanted_.erase(forgotten_serial);
                next.devices.erase(next.devices.begin() + static_cast<std::ptrdiff_t>(index));
                // Whatever sat beside it goes back to a monitor's edge.
                for (DeviceConfig& other : next.devices) {
                    if (other.beside == forgotten_serial)
                        other.beside.clear();
                }
                answer = save_store(next) ? reload() : "error=the config could not be written";
            }
        } else if (verb == "rescan") {
            rescan_now();
            answer = "ok";
        } else if (verb == "reload") {
            if (active_ >= 0)
                leave();
            answer = reload();
        }
    });
    // What these commands change is seen by the next status: the phone they
    // free or take shows as new or as known once it has been scanned for.
    if (answer == "ok" &&
        (verb == "rescan" || verb == "connect" || verb == "forget" || verb == "reload"))
        wait_scanned();
    return answer;
}

} // namespace aoas
