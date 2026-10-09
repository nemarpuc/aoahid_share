// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/config.hpp"
#include "aoahid_share/position_cell.hpp"
#include "aoahid_share/session.hpp"

#include "adb.hpp"
#include "capture.hpp"
#include "usb.hpp"
#include "usb_watch.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace aoas {

// Ties the capture backend, the sessions and the USB output together. All of
// its state is touched only on the capture thread; other threads hand work to
// it with on_capture().
class Daemon final : public CaptureHandler {
  public:
    Daemon();
    ~Daemon() override;

    // Runs until quit() or a signal. Returns the process exit code. A test
    // passes its own capture backend; otherwise the config's is used.
    int run(std::unique_ptr<Capture> capture = nullptr);
    void quit();

    // Executes one control command (docs/CONFIG.md) and returns its answer.
    // Called from the control thread.
    [[nodiscard]] std::string command(const std::string& line);

    void monitors_changed() override;
    bool edge_hit(uint32_t barrier, int t, double push, bool button_down) override;
    void motion(double dx, double dy) override;
    void scroll(double wheel, double pan) override;
    void button(unsigned button, bool down) override;
    void key(HidKey key, bool down, bool grabbed) override;
    void lost() override;

  private:
    enum class Stage { absent, ready, error };

    struct Phone {
        DeviceConfig config;
        // The port its ADB interface is served on.
        uint16_t port{};
        Stage stage{Stage::absent};
        std::string status{"not connected"};
        // What went wrong while connecting (the ADB proxy, adb itself); it
        // explains a value that is then missing.
        std::string note;
        std::string serial;
        int link{-1};
        bool proxy{};
        // How it was opened (see rebuild_phones()).
        LinkOptions link_options;
        bool wants_proxy{};
        // What the last "fill" said, and whether all went well.
        std::string fill_note;
        bool fill_ok{};
        Size natural{};
        int rotation{-1};
        // Android pixels per millimetre.
        double density{};
        AccelMode mode{AccelMode::unknown};
        // Pixels one count moves the phone's cursor at low speed.
        double gain{1.0};
        bool layout_ok{};
        // Its size and gain are known; where it sits is worked out next.
        bool measured{};
        // The display as it is held.
        Size view{};
        AccelModel accel;
        MotionConfig motion;
        // It touches a monitor: `portal` is its edge there. Otherwise it
        // sits beside phones_[parent], and `beside` maps that device's edge
        // (the "PC" side of the portal) onto this one's.
        bool has_pc{};
        int parent{-1};
        Portal beside;
        Portal portal;
        // Where its session publishes its position for the status; declared
        // before the session, which points at it.
        std::shared_ptr<PositionCell> cell{std::make_shared<PositionCell>()};
        // Present while the phone is open.
        std::unique_ptr<Session> session;
        double ready_at{};
    };

    // What the device thread found out about a phone it opened.
    struct Opened {
        size_t phone{};
        int link{-1};
        std::string serial;
        bool proxy{};
        LinkOptions options;
        bool wants_proxy{};
        uint16_t port{};
        // Which list of phones it was opened for (see generation_).
        uint64_t generation{};
        std::string note;
    };

    // What `status` says, as of the last publish_status(): the text that
    // changes only with the layout, and what is needed to add what changes
    // with every report. It is built on the capture thread and read on any
    // other, so a status request never waits for the capture thread.
    struct PhoneView {
        std::string serial;
        int link{-1};
        bool placed{};
        // Touch is switched on for it: the status then shows how far a tap
        // can be from the cursor.
        bool touch_on{};
        std::string lines;
        std::shared_ptr<PositionCell> cell;
    };
    struct StatusView {
        std::string head;
        std::string fill_serial;
        std::vector<std::string> serials;
        std::vector<PhoneView> phones;
    };

    // Runs a function on the capture thread and waits for it.
    void on_capture(const std::function<void()>& task);

    [[nodiscard]] std::string load_config();
    // Takes a config that has read and validated well into use.
    void adopt_config(Config loaded);
    void rebuild_phones();
    void device_loop();
    [[nodiscard]] Opened open_phone(size_t phone, size_t scan_index, const Config& config,
                                    const DeviceConfig& device, uint16_t port,
                                    const std::string& serial);
    void install(Opened opened);
    [[nodiscard]] LinkOptions link_options_of(const DeviceConfig& device) const;
    void drop(size_t phone, const std::string& why);
    // Lays every open phone out again: those on a monitor first, then those
    // beside another phone, and tells each session its neighbours.
    void relayout();
    [[nodiscard]] bool measure(Phone& phone);
    [[nodiscard]] bool place_on_monitor(Phone& phone);
    [[nodiscard]] bool place_beside(Phone& phone, const Phone& parent);
    void set_motion(Phone& phone);
    // "fill": starts reading a connected device through its proxy, off the
    // capture thread, and writes what it said into its file when done.
    [[nodiscard]] std::string start_fill(size_t index);
    void finish_fill(const std::string& serial, uint64_t generation, bool read,
                     const PhoneFacts& facts, const std::string& problem);
    void apply_barriers();
    void leave();
    [[nodiscard]] std::string enter_by_command(size_t phone);
    // The cursor left phones_[from] toward phones_[to], which is beside it.
    void hop(size_t from, size_t to);
    void remember(size_t phone);
    // The phone a name or serial means, or the count of phones.
    [[nodiscard]] size_t phone_named(const std::string& name) const;
    // Where the input goes when nothing says which device: the one that had
    // it last if it is ready, else the first that is.
    [[nodiscard]] size_t usual_phone() const;
    void close_phone(Phone& phone);
    [[nodiscard]] bool hotkey(HidKey key, bool down);
    // Sends a media key to that phone. False when it is not open.
    bool send_media(size_t phone, uint16_t usage, bool down);
    // Where a media key goes when nothing names a device: the one that has
    // the input, else the one that had it last, or any that is connected.
    [[nodiscard]] size_t media_phone() const;
    void save_state(const std::string& key, const std::string& value);
    void publish_status();
    [[nodiscard]] std::string render_status();
    // Asks the device thread for a scan now.
    void request_scan();
    void wait_scanned();
    [[nodiscard]] static double now() noexcept;

    Config config_;
    std::string config_error_;

    Usb usb_;
    std::unique_ptr<Capture> capture_;
    std::string capture_error_;
    std::vector<Monitor> monitors_;
    std::vector<std::unique_ptr<Phone>> phones_;
    std::atomic<int> active_{-1};
    std::atomic<bool> paused_{};
    uint8_t modifiers_{};
    // Each phone's shortcuts, parsed; one per phone, in their order.
    struct PhoneKeys {
        Hotkey switch_key;
        Hotkey lock;
        Hotkey resync;
        std::array<Hotkey, media_key_count> media{};
    };
    std::vector<PhoneKeys> keys_;
    // The device that has the input is locked: no edge takes the input off it.
    std::atomic<bool> locked_{};
    // Devices the user asked to connect, by serial: only these are opened, and
    // one that drops out is opened again when it comes back. Starting the
    // daemon opens nothing. Capture thread only.
    std::set<std::string> wanted_;
    // The serials the last scan found plugged in. Capture thread only.
    std::set<std::string> plugged_;
    // The serial of the device that had the input last.
    std::string last_used_;
    // Devices that are connected but have no file yet.
    std::vector<PhoneInfo> new_;
    // The reading "fill" runs: its thread, whose phone, and whether it is
    // to end early (the daemon is quitting).
    std::thread fill_thread_;
    std::atomic<bool> fill_running_{};
    std::atomic<bool> fill_cancel_{};
    std::string fill_serial_;
    // The media hotkey being held: its key, the usage it sent and to which phone.
    uint16_t held_media_key_{};
    uint16_t held_media_usage_{};
    size_t held_media_phone_{};
    std::string state_;
    // Why no capture backend has started yet; guarded by state_mutex_.
    std::string start_error_;
    std::mutex state_mutex_;

    std::mutex view_mutex_;
    std::shared_ptr<const StatusView> view_;

    UsbWatch watch_;
    // Devices coming and going are told by watch_; otherwise it is polled.
    bool watching_{};
    std::thread device_thread_;
    std::mutex device_mutex_;
    std::condition_variable device_wake_;
    std::condition_variable scan_done_;
    // Scans asked for and scans made, under device_mutex_.
    uint64_t scans_requested_{};
    uint64_t scans_finished_{};
    std::atomic<bool> stopping_{};
    // A reload asks the device thread for a scan now; under device_mutex_.
    bool wake_{};
    // Counts the rebuilds of phones_; capture thread only.
    uint64_t generation_{};
    // Counts the times the daemon wrote a device's file on its own ("fill").
    std::atomic<uint64_t> files_version_{};
    // Set once the capture backend runs; commands arriving earlier are
    // answered without touching it.
    std::atomic<bool> ready_{};
    std::atomic<bool> quit_early_{};
    // Set once the capture loop has ended; on_capture() stops waiting then.
    std::atomic<bool> finished_{};
    int exit_code_{};
};

} // namespace aoas
