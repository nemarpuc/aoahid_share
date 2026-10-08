// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/session.hpp"

#include <aoahid.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct aoahid_adb_proxy_context;

namespace aoas {

struct PhoneInfo {
    std::string serial;
    std::string product;
    uint16_t vendor_id{};
    uint16_t product_id{};
};

// `buttons` is fixed when the phone is opened; the three Nodes can be added
// and removed on an open phone (Usb::set_nodes).
struct LinkOptions {
    unsigned buttons{5};
    bool mouse{true};
    bool keyboard{true};
    bool media{true};
    // The touchscreen Node, built for the device's natural size.
    bool touch{};
    Size touch_size{};
};

// What a link measured about its own reports, in microseconds. "Queue" runs
// from the moment the input reached the sink to the start of the USB call,
// "total" to the report's completion. Percentiles are over the last
// `samples` reports; `*_max` is the worst since the phone was opened.
struct LinkStats {
    uint64_t reports{};
    // Inputs that were summed into a report that was already waiting.
    uint64_t merged{};
    uint32_t samples{};
    uint32_t queue_p50{};
    uint32_t queue_p99{};
    uint32_t queue_max{};
    uint32_t total_p50{};
    uint32_t total_p99{};
    uint32_t total_max{};
    // Reports waiting right now.
    uint32_t depth{};
    // The sender runs with a real-time priority.
    bool realtime{};
};

// Owns the open phones ("links"). Each link has a libaoahid Context of its
// own, a lock for the calls on it and a thread that sends to it, so a phone
// that is slow or being opened never holds up another. Discovery has a
// Context and a lock of its own too: scanning never waits for a report and no
// report waits for a scan. Sink calls only queue; the link's thread coalesces
// motion and delivers keys and buttons in order.
class Usb {
  public:
    static constexpr int max_links = 8;

    Usb();
    ~Usb();
    Usb(const Usb&) = delete;
    Usb& operator=(const Usb&) = delete;

    // Empty on success.
    [[nodiscard]] std::string start();
    void stop();
    // How a link is paced. Reports per second while only motion is pending; 0
    // sends as fast as the previous report completes. With `every`, nothing
    // is summed or paced: each movement and scroll step is a report of its
    // own, in order. Each phone has its own.
    void set_rate(int link, unsigned hz, bool every) noexcept {
        if (link < 0 || link >= max_links)
            return;
        Link& entry = links_[static_cast<size_t>(link)];
        entry.rate_hz.store(hz, std::memory_order_relaxed);
        entry.every.store(every, std::memory_order_relaxed);
    }

    // Lists AOA-capable phones. Probes every USB device, so it is not called
    // while a phone has the input; it does not touch any link.
    [[nodiscard]] std::vector<PhoneInfo> scan(std::string& error);
    // Opens the phone at that index of the last scan(). Returns the link, or
    // -1 with the reason in error.
    [[nodiscard]] int open(size_t index, const LinkOptions& options, std::string& error);
    void close(int link);
    // Registers the Nodes `options` names and unregisters the others, on a
    // phone that stays open. `buttons` is not applied. A Node added now takes
    // no input until Android has had time to register it. On failure the link
    // is marked failed.
    void set_nodes(int link, const LinkOptions& options);
    // Open and not failed.
    [[nodiscard]] bool alive(int link) const noexcept;
    [[nodiscard]] std::string failure(int link) const;
    [[nodiscard]] uint64_t reports(int link) const noexcept;
    [[nodiscard]] LinkStats stats(int link) const;
    // What the last reports to a phone carried, one line per call, oldest
    // first: "N MS REPORT TEXT" (the line's number, the steady clock in
    // milliseconds, the report it went out in). Asking also turns the
    // recording on for the next few seconds; it is off otherwise.
    [[nodiscard]] std::vector<std::string> trace(int link);
    [[nodiscard]] Sink& sink(int link) noexcept;

    // Serves the phone's ADB interface on 127.0.0.1:port. Returns the proxy's
    // code (0 on success); held is set when another program holds the
    // interface, and absent when the phone exposes none.
    int start_proxy(int link, uint16_t port, bool& held, bool& absent);
    void stop_proxy(int link);

  private:
    static constexpr size_t ring_size = 256;

    struct Event {
        uint8_t kind;
        uint8_t link;
        uint16_t code;
        bool down;
        // When it reached the sink, in steady-clock nanoseconds.
        int64_t at;
        // A touchscreen contact's position, in the device's raw coordinates.
        int32_t x{};
        int32_t y{};
        // A touch event that puts a contact down, as against moving it.
        bool placing{};
        // After a placing event: how long the next report waits, in
        // nanoseconds; -1 is one report period (1 ms at least).
        int64_t gap_ns{-1};
    };
    // One call into libaoahid, as the sender made it.
    struct Trace {
        enum Kind : uint8_t { mouse_move, mouse_scroll, button, key, media, touch };
        int64_t at{};
        uint64_t report{};
        Kind kind{};
        // Buttons and keys: pressed. Touch: 0 lift, 1 down, 2 move.
        uint8_t phase{};
        int32_t a{};
        int32_t b{};
        int32_t c{};
    };
    struct Motion {
        int64_t dx{};
        int64_t dy{};
        int64_t wheel{};
        int64_t pan{};
        // When the first of what it holds reached the sink; 0 when empty.
        int64_t at{};
        [[nodiscard]] bool any() const noexcept {
            return dx != 0 || dy != 0 || wheel != 0 || pan != 0;
        }
    };
    class LinkSink final : public Sink {
      public:
        void bind(Usb* usb, int link) noexcept {
            usb_ = usb;
            link_ = static_cast<uint8_t>(link);
        }
        void move(Delta counts) override;
        void jump(Delta counts) override;
        void scroll(int wheel, int pan) override;
        void button(unsigned button, bool down) override;
        void key(uint16_t usage, bool down) override;
        void media(uint16_t usage, bool down) override;
        void touch(unsigned contact, int x, int y, bool down) override;
        void touch_hold(unsigned contact, int x, int y, double release_s) override;
        void touch_place(unsigned contact, int x, int y, double gap_s) override;

      private:
        Usb* usb_{};
        uint8_t link_{};
    };
    struct Link {
        // The Context every call on this phone is serialized on, and the lock
        // that serializes them. Created with the first phone the slot opens
        // and kept: a Context never reuses a HID id, so a phone that comes
        // back to the slot it had does not meet its old registrations.
        aoahid_context* context{};
        std::mutex usb;
        aoahid_device* device{};
        aoahid_node* mouse{};
        aoahid_node* keyboard{};
        aoahid_node* toggle{};
        aoahid_node* touch{};
        // The size the touch Node was built for; guarded by `usb`.
        Size touch_size{};
        // Per touch contact: whether it is down on the device; guarded by `usb`.
        std::array<bool, 2> touch_down{};
        // The time, per Node (mouse, keyboard, toggle, touch), from which it
        // takes input; guarded by `usb`.
        std::array<int64_t, 4> node_ready_ns{};
        unsigned buttons{};
        aoahid_adb_proxy_context* proxy{};
        std::string last_serial;
        // Claimed by an open phone; guarded by Usb::slots_.
        bool taken{};
        std::atomic<bool> open{};
        std::atomic<bool> failed{};
        std::atomic<unsigned> rate_hz{};
        std::atomic<bool> every{};
        std::atomic<uint64_t> reports{};
        std::atomic<uint64_t> merged{};
        mutable std::mutex note;
        std::string failure;

        // The queue, guarded by `queue`.
        mutable std::mutex queue;
        std::condition_variable wake;
        Motion motion;
        // Movements that each go out alone and in order, ahead of `motion`.
        std::deque<Motion> jumps;
        std::array<Event, 128> events{};
        // Per touch contact: when a held contact is lifted (0: not held), and
        // where.
        std::array<int64_t, 2> hold_until{};
        std::array<int32_t, 2> hold_x{};
        std::array<int32_t, 2> hold_y{};
        // Per touch contact: whether its last queued event leaves it down,
        // and a lift that did not fit the full queue and is queued when there
        // is room (with where).
        std::array<bool, 2> touch_queued_down{};
        std::array<bool, 2> lift_owed{};
        std::array<int32_t, 2> lift_x{};
        std::array<int32_t, 2> lift_y{};
        size_t head{};
        size_t count{};
        bool stopping{};
        std::thread sender;
        std::atomic<bool> realtime{};

        // Latencies of the last reports; written by the sender only.
        std::array<std::atomic<uint32_t>, ring_size> queue_us{};
        std::array<std::atomic<uint32_t>, ring_size> total_us{};
        std::atomic<uint64_t> ring_next{};
        std::atomic<uint32_t> queue_worst{};
        std::atomic<uint32_t> total_worst{};
        // The calls of the last reports, for `trace`; recorded only until
        // `trace_until` (steady-clock nanoseconds).
        std::atomic<int64_t> trace_until{};
        std::mutex trace_lock;
        std::array<Trace, 128> trace_ring{};
        uint64_t trace_next{};
        LinkSink sink;
    };

    void push(Link& link, const Event& event);
    // The caller holds link.queue. A move of a contact that is already the
    // last thing queued replaces that entry, so a fast drag cannot fill the
    // queue.
    void queue_touch_locked(Link& link, unsigned contact, int x, int y, bool down, int64_t at,
                            int64_t gap_ns = -1);
    void run(Link& link);
    void deliver(Link& link, const Motion& motion, const Event* events, size_t count);
    void fail(Link& link, aoahid_result result);
    void close_locked(Link& link);
    [[nodiscard]] static aoahid_result sync_nodes_locked(Link& link, const LinkOptions& options,
                                                         int64_t ready_ns);
    // Queues a report that goes out alone, behind whatever is pending. The
    // caller holds link.queue.
    static void queue_alone(Link& link, const Motion& report);
    [[nodiscard]] static int64_t now_ns() noexcept;

    // Discovery: its own Context, serialized by scan_lock_. The snapshot is
    // kept until the next scan so open() can use an entry of it.
    aoahid_context* scan_context_{};
    aoahid_discovery* discovery_{};
    std::mutex scan_lock_;

    // Guards which slots are taken and the serial each one last had.
    std::mutex slots_;
    std::array<Link, max_links> links_{};
};

} // namespace aoas
