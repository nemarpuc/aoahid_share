// SPDX-License-Identifier: MIT
//
// The xdg-desktop-portal InputCapture backend (GNOME, KDE Plasma on Wayland).
// The compositor watches the barriers and grabs the input itself; the events
// arrive through libei. See docs/PLATFORMS.md.
#include "capture.hpp"
#include "task_queue.hpp"
#include "wayland_outputs.hpp"

#include <dbus/dbus.h>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

namespace aoas {
namespace {

constexpr const char* portal_name = "org.freedesktop.portal.Desktop";
constexpr const char* portal_path = "/org/freedesktop/portal/desktop";
constexpr const char* capture_interface = "org.freedesktop.portal.InputCapture";
constexpr const char* request_interface = "org.freedesktop.portal.Request";
constexpr const char* session_interface = "org.freedesktop.portal.Session";

constexpr uint32_t capability_keyboard = 1U;
constexpr uint32_t capability_pointer = 2U;
// Permission survives until the user revokes it.
constexpr uint32_t persist_until_revoked = 2U;

constexpr int call_timeout_ms = 5000;
// Start shows the permission dialog; the user may take a while.
constexpr int dialog_timeout_ms = 120000;
// Smooth-scroll pixels a wheel notch corresponds to, for devices that report
// no discrete steps.
constexpr double pixels_per_notch = 15.0;

void dict_open(DBusMessageIter& parent, DBusMessageIter& array) {
    dbus_message_iter_open_container(&parent, DBUS_TYPE_ARRAY, "{sv}", &array);
}

void dict_basic(DBusMessageIter& array, const char* key, const int type, const char* signature,
                const void* value) {
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&array, &entry);
}

void dict_string(DBusMessageIter& array, const char* key, const std::string& value) {
    const char* text = value.c_str();
    dict_basic(array, key, DBUS_TYPE_STRING, "s", &text);
}

void dict_uint(DBusMessageIter& array, const char* key, const uint32_t value) {
    const dbus_uint32_t number = value;
    dict_basic(array, key, DBUS_TYPE_UINT32, "u", &number);
}

// Finds a key of an a{sv} and leaves value on the variant's content.
bool dict_find(DBusMessageIter dict, const char* key, DBusMessageIter& value) {
    if (dbus_message_iter_get_arg_type(&dict) != DBUS_TYPE_ARRAY)
        return false;
    DBusMessageIter entries;
    dbus_message_iter_recurse(&dict, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&entries, &entry);
        const char* name = nullptr;
        dbus_message_iter_get_basic(&entry, &name);
        if (name != nullptr && std::strcmp(name, key) == 0 && dbus_message_iter_next(&entry) &&
            dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &value);
            return true;
        }
        dbus_message_iter_next(&entries);
    }
    return false;
}

bool dict_uint_value(DBusMessageIter dict, const char* key, uint32_t& out) {
    DBusMessageIter value;
    if (!dict_find(dict, key, value) || dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_UINT32)
        return false;
    dbus_uint32_t number = 0;
    dbus_message_iter_get_basic(&value, &number);
    out = number;
    return true;
}

bool dict_text_value(DBusMessageIter dict, const char* key, std::string& out) {
    DBusMessageIter value;
    if (!dict_find(dict, key, value))
        return false;
    const int type = dbus_message_iter_get_arg_type(&value);
    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return false;
    const char* text = nullptr;
    dbus_message_iter_get_basic(&value, &text);
    out = text != nullptr ? text : "";
    return true;
}

class PortalCapture final : public Capture {
  public:
    ~PortalCapture() override {
        if (ei_ != nullptr)
            ei_unref(ei_);
        if (connection_ != nullptr) {
            if (!session_.empty()) {
                DBusMessage* call = dbus_message_new_method_call(portal_name, session_.c_str(),
                                                                 session_interface, "Close");
                if (call != nullptr) {
                    dbus_connection_send(connection_, call, nullptr);
                    dbus_connection_flush(connection_);
                    dbus_message_unref(call);
                }
            }
            dbus_connection_remove_filter(connection_, &PortalCapture::filter, this);
            dbus_connection_close(connection_);
            dbus_connection_unref(connection_);
        }
    }

    const char* name() const noexcept override { return "portal"; }
    bool sees_local_keys() const noexcept override { return false; }

    std::string start(CaptureHandler& handler, const CaptureEnv& env) override {
        handler_ = &handler;
        DBusError error;
        dbus_error_init(&error);
        // A private connection, so closing it never disturbs another user of
        // the shared one in this process.
        connection_ = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (connection_ == nullptr) {
            std::string text = "no session D-Bus: ";
            text += error.message != nullptr ? error.message : "unknown error";
            dbus_error_free(&error);
            return text;
        }
        dbus_connection_set_exit_on_disconnect(connection_, FALSE);
        // ":1.42" becomes "1_42" in request and session object paths.
        sender_ = dbus_bus_get_unique_name(connection_) + 1;
        std::replace(sender_.begin(), sender_.end(), '.', '_');
        if (!dbus_connection_add_filter(connection_, &PortalCapture::filter, this, nullptr))
            return "out of memory";
        dbus_bus_add_match(
            connection_, "type='signal',interface='org.freedesktop.portal.InputCapture'", nullptr);
        dbus_bus_add_match(
            connection_, "type='signal',interface='org.freedesktop.portal.Session',member='Closed'",
            nullptr);

        uint32_t version = 0;
        if (!read_version(version))
            return "this desktop has no InputCapture portal";

        std::string failure = version >= 2 ? create_and_start(env) : create_legacy();
        if (!failure.empty())
            return failure;
        failure = connect_eis();
        if (!failure.empty())
            return failure;
        return read_zones();
    }

    std::vector<Monitor> monitors() override {
        const std::vector<Monitor> outputs = query_wayland_outputs();
        std::vector<Monitor> result;
        for (size_t index = 0; index < zones_.size(); ++index) {
            Monitor monitor;
            monitor.rect = zones_[index];
            monitor.name = "zone-" + std::to_string(index + 1);
            // A zone and an output with the same logical rectangle are the
            // same screen; that gives the zone its name and physical size.
            for (const Monitor& output : outputs) {
                if (output.rect.x == monitor.rect.x && output.rect.y == monitor.rect.y &&
                    output.rect.w == monitor.rect.w && output.rect.h == monitor.rect.h) {
                    if (!output.name.empty())
                        monitor.name = output.name;
                    monitor.width_mm = output.width_mm;
                    monitor.height_mm = output.height_mm;
                    break;
                }
            }
            result.push_back(std::move(monitor));
        }
        return result;
    }

    std::string set_barriers(const std::vector<Barrier>& barriers) override {
        barriers_ = barriers;
        if (session_.empty())
            return "the portal session is not running";
        if (active_)
            return "the input is captured";

        // Barriers are replaced on a session that is not armed, the order the
        // portal's own clients use: disable, set, enable.
        if (enabled_) {
            static_cast<void>(simple_call("Disable"));
            enabled_ = false;
        }
        std::vector<uint32_t> refused;
        std::string failure = send_barriers(barriers_, refused);
        if (failure.empty() && !refused.empty()) {
            // KDE's portal only takes a barrier that covers a whole screen
            // edge. Widen the refused ones to their edge; the handler still
            // decides by position, so a hit outside the wanted span is
            // handed straight back.
            for (Barrier& barrier : barriers_) {
                if (std::find(refused.begin(), refused.end(), barrier.id) == refused.end() ||
                    barrier.monitor >= zones_.size())
                    continue;
                const Rect& zone = zones_[barrier.monitor];
                barrier.span = vertical(barrier.side) ? Span{zone.y, zone.y + zone.h - 1}
                                                      : Span{zone.x, zone.x + zone.w - 1};
            }
            failure = send_barriers(barriers_, refused);
        }
        if (!failure.empty())
            return "the barriers could not be set: " + failure;
        if (!refused.empty()) {
            std::string ids;
            for (const uint32_t id : refused)
                ids += (ids.empty() ? "" : ", ") + std::to_string(id);
            return "the compositor refused barrier " + ids +
                   " (an edge shared with another screen cannot be used)";
        }
        if (barriers_.empty())
            return {};
        enabled_ = simple_call("Enable");
        return enabled_ ? std::string() : std::string("the capture could not be enabled");
    }

    // The compositor decides when a capture starts; it cannot be asked to.
    bool grab() override { return false; }

    // One SetPointerBarriers call. Empty on success, with the ids the
    // compositor turned down in refused.
    std::string send_barriers(const std::vector<Barrier>& barriers,
                              std::vector<uint32_t>& refused) {
        refused.clear();
        const std::string token = next_token();
        DBusMessage* call = method("SetPointerBarriers");
        DBusMessageIter iter;
        DBusMessageIter options;
        DBusMessageIter list;
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dict_open(iter, options);
        dict_string(options, "handle_token", token);
        dbus_message_iter_close_container(&iter, &options);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "a{sv}", &list);
        for (const Barrier& barrier : barriers) {
            if (barrier.monitor >= zones_.size())
                continue;
            const Rect& zone = zones_[barrier.monitor];
            dbus_int32_t x1 = 0;
            dbus_int32_t y1 = 0;
            dbus_int32_t x2 = 0;
            dbus_int32_t y2 = 0;
            // A barrier lies on the left or top edge of its pixels, so the
            // right and bottom ones sit one past the zone.
            if (vertical(barrier.side)) {
                x1 = x2 = barrier.side == Side::left ? zone.x : zone.x + zone.w;
                y1 = barrier.span.lo;
                y2 = barrier.span.hi;
            } else {
                y1 = y2 = barrier.side == Side::top ? zone.y : zone.y + zone.h;
                x1 = barrier.span.lo;
                x2 = barrier.span.hi;
            }
            DBusMessageIter dict;
            DBusMessageIter entry;
            DBusMessageIter variant;
            DBusMessageIter position;
            dict_open(list, dict);
            dict_uint(dict, "barrier_id", barrier.id);
            const char* key = "position";
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "(iiii)", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &position);
            dbus_message_iter_append_basic(&position, DBUS_TYPE_INT32, &x1);
            dbus_message_iter_append_basic(&position, DBUS_TYPE_INT32, &y1);
            dbus_message_iter_append_basic(&position, DBUS_TYPE_INT32, &x2);
            dbus_message_iter_append_basic(&position, DBUS_TYPE_INT32, &y2);
            dbus_message_iter_close_container(&variant, &position);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(&dict, &entry);
            dbus_message_iter_close_container(&list, &dict);
        }
        dbus_message_iter_close_container(&iter, &list);
        const dbus_uint32_t zone_set = zone_set_;
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &zone_set);

        DBusMessage* response = nullptr;
        const std::string failure = request(call, token, call_timeout_ms, response);
        if (!failure.empty())
            return failure;

        DBusMessageIter results;
        DBusMessageIter value;
        if (response_results(response, results) && dict_find(results, "failed_barriers", value) &&
            dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY) {
            DBusMessageIter ids;
            dbus_message_iter_recurse(&value, &ids);
            while (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_UINT32) {
                dbus_uint32_t id = 0;
                dbus_message_iter_get_basic(&ids, &id);
                refused.push_back(id);
                dbus_message_iter_next(&ids);
            }
        }
        dbus_message_unref(response);
        return {};
    }

    void release(const int x, const int y) override {
        if (!active_)
            return;
        active_ = false;
        DBusMessage* call = method("Release");
        DBusMessageIter iter;
        DBusMessageIter options;
        DBusMessageIter entry;
        DBusMessageIter variant;
        DBusMessageIter position;
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dict_open(iter, options);
        dict_uint(options, "activation_id", activation_);
        const char* key = "cursor_position";
        const double px = x;
        const double py = y;
        dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "(dd)", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_STRUCT, nullptr, &position);
        dbus_message_iter_append_basic(&position, DBUS_TYPE_DOUBLE, &px);
        dbus_message_iter_append_basic(&position, DBUS_TYPE_DOUBLE, &py);
        dbus_message_iter_close_container(&variant, &position);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&options, &entry);
        dbus_message_iter_close_container(&iter, &options);
        dbus_connection_send(connection_, call, nullptr);
        dbus_connection_flush(connection_);
        dbus_message_unref(call);
    }

    void run() override {
        int bus_fd = -1;
        if (!dbus_connection_get_unix_fd(connection_, &bus_fd))
            return;
        while (!stopping_.load(std::memory_order_acquire)) {
            // What libdbus already read (the reply that came with a call) is
            // dispatched now: poll() only wakes for what is still unread.
            while (dbus_connection_get_dispatch_status(connection_) == DBUS_DISPATCH_DATA_REMAINS)
                dbus_connection_dispatch(connection_);
            pollfd fds[3] = {{bus_fd, POLLIN, 0},
                             {ei_ != nullptr ? ei_get_fd(ei_) : -1, POLLIN, 0},
                             {tasks_.fd(), POLLIN, 0}};
            if (poll(fds, 3, -1) < 0)
                continue;
            if ((fds[2].revents & POLLIN) != 0)
                tasks_.drain();
            if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                read_ei();
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                if (!dbus_connection_read_write(connection_, 0))
                    break;
                while (dbus_connection_dispatch(connection_) == DBUS_DISPATCH_DATA_REMAINS) {
                }
            }
        }
    }

    void stop() override {
        stopping_.store(true, std::memory_order_release);
        tasks_.post([] {});
    }

    void post(std::function<void()> task) override { tasks_.post(std::move(task)); }

  private:
    std::string next_token() { return "aoahid_share_" + std::to_string(++token_counter_); }

    DBusMessage* method(const char* name) const {
        return dbus_message_new_method_call(portal_name, portal_path, capture_interface, name);
    }

    bool read_version(uint32_t& version) {
        DBusMessage* call = dbus_message_new_method_call(portal_name, portal_path,
                                                         "org.freedesktop.DBus.Properties", "Get");
        const char* interface = capture_interface;
        const char* property = "version";
        dbus_message_append_args(call, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
                                 DBUS_TYPE_INVALID);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(connection_, call, call_timeout_ms, nullptr);
        dbus_message_unref(call);
        if (reply == nullptr)
            return false;
        DBusMessageIter iter;
        DBusMessageIter value;
        bool ok = false;
        if (dbus_message_iter_init(reply, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&iter, &value);
            if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32) {
                dbus_uint32_t number = 0;
                dbus_message_iter_get_basic(&value, &number);
                version = number;
                ok = true;
            }
        }
        dbus_message_unref(reply);
        return ok;
    }

    // Sends a call that answers through a Request object and waits for its
    // Response signal. Consumes call. Empty on success, with response set.
    std::string request(DBusMessage* call, const std::string& token, const int timeout_ms,
                        DBusMessage*& response) {
        // Subscribe before calling, or a fast Response could be missed.
        wait_path_ = std::string(portal_path) + "/request/" + sender_ + "/" + token;
        const std::string rule =
            "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" +
            wait_path_ + "'";
        dbus_bus_add_match(connection_, rule.c_str(), nullptr);
        wait_reply_ = nullptr;

        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(connection_, call, call_timeout_ms, &error);
        dbus_message_unref(call);
        std::string failure;
        if (reply == nullptr) {
            failure = error.message != nullptr ? error.message : "the portal did not answer";
            dbus_error_free(&error);
        } else {
            dbus_message_unref(reply);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (wait_reply_ == nullptr && std::chrono::steady_clock::now() < deadline) {
                if (!dbus_connection_read_write_dispatch(connection_, 100)) {
                    failure = "the D-Bus connection closed";
                    break;
                }
            }
            if (wait_reply_ == nullptr && failure.empty())
                failure = "no answer in time";
        }
        dbus_bus_remove_match(connection_, rule.c_str(), nullptr);
        wait_path_.clear();
        if (!failure.empty())
            return failure;

        DBusMessageIter iter;
        dbus_uint32_t code = 2;
        if (dbus_message_iter_init(wait_reply_, &iter) &&
            dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
            dbus_message_iter_get_basic(&iter, &code);
        if (code != 0) {
            dbus_message_unref(wait_reply_);
            wait_reply_ = nullptr;
            return code == 1 ? "the request was declined" : "the portal reported an error";
        }
        response = wait_reply_;
        wait_reply_ = nullptr;
        return {};
    }

    // The a{sv} that follows the response code.
    static bool response_results(DBusMessage* response, DBusMessageIter& results) {
        return dbus_message_iter_init(response, &results) && dbus_message_iter_next(&results) &&
               dbus_message_iter_get_arg_type(&results) == DBUS_TYPE_ARRAY;
    }

    // A session method that takes only (o session, a{sv} options) and
    // returns nothing.
    bool simple_call(const char* name) {
        DBusMessage* call = method(name);
        DBusMessageIter iter;
        DBusMessageIter options;
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dict_open(iter, options);
        dbus_message_iter_close_container(&iter, &options);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(connection_, call, call_timeout_ms, nullptr);
        dbus_message_unref(call);
        if (reply == nullptr)
            return false;
        dbus_message_unref(reply);
        return true;
    }

    // Interface version 2: create, then Start with a restore token so the
    // permission dialog appears only the first time.
    std::string create_and_start(const CaptureEnv& env) {
        DBusMessage* call = method("CreateSession2");
        DBusMessageIter iter;
        DBusMessageIter options;
        dbus_message_iter_init_append(call, &iter);
        dict_open(iter, options);
        dict_string(options, "session_handle_token", next_token());
        dbus_message_iter_close_container(&iter, &options);
        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(connection_, call, call_timeout_ms, &error);
        dbus_message_unref(call);
        if (reply == nullptr) {
            std::string text = "the capture session could not be created: ";
            text += error.message != nullptr ? error.message : "no answer";
            dbus_error_free(&error);
            return text;
        }
        DBusMessageIter results;
        const bool found = dbus_message_iter_init(reply, &results) &&
                           dict_text_value(results, "session_handle", session_);
        dbus_message_unref(reply);
        if (!found)
            return "the portal returned no session";

        const std::string token = next_token();
        call = method("Start");
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        const char* parent = "";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent);
        dict_open(iter, options);
        dict_string(options, "handle_token", token);
        dict_uint(options, "capabilities", capability_keyboard | capability_pointer);
        dict_uint(options, "persist_mode", persist_until_revoked);
        if (!env.restore_token.empty())
            dict_string(options, "restore_token", env.restore_token);
        dbus_message_iter_close_container(&iter, &options);

        DBusMessage* response = nullptr;
        const std::string failure = request(call, token, dialog_timeout_ms, response);
        if (!failure.empty())
            return "permission to capture input was not given: " + failure;
        std::string restore;
        if (response_results(response, results) &&
            dict_text_value(results, "restore_token", restore) && env.save_restore_token)
            env.save_restore_token(restore);
        dbus_message_unref(response);
        return {};
    }

    // Interface version 1: one call that both creates and starts, and asks
    // for permission on every run.
    std::string create_legacy() {
        const std::string token = next_token();
        DBusMessage* call = method("CreateSession");
        DBusMessageIter iter;
        DBusMessageIter options;
        dbus_message_iter_init_append(call, &iter);
        const char* parent = "";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent);
        dict_open(iter, options);
        dict_string(options, "handle_token", token);
        dict_string(options, "session_handle_token", next_token());
        dict_uint(options, "capabilities", capability_keyboard | capability_pointer);
        dbus_message_iter_close_container(&iter, &options);

        DBusMessage* response = nullptr;
        const std::string failure = request(call, token, dialog_timeout_ms, response);
        if (!failure.empty())
            return "permission to capture input was not given: " + failure;
        DBusMessageIter results;
        const bool found = response_results(response, results) &&
                           dict_text_value(results, "session_handle", session_);
        dbus_message_unref(response);
        return found ? std::string() : std::string("the portal returned no session");
    }

    std::string connect_eis() {
        DBusMessage* call = method("ConnectToEIS");
        DBusMessageIter iter;
        DBusMessageIter options;
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dict_open(iter, options);
        dbus_message_iter_close_container(&iter, &options);
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(connection_, call, call_timeout_ms, nullptr);
        dbus_message_unref(call);
        if (reply == nullptr)
            return "the input event connection could not be opened";
        int fd = -1;
        const bool ok =
            dbus_message_get_args(reply, nullptr, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID);
        dbus_message_unref(reply);
        if (!ok || fd < 0)
            return "the portal returned no input event connection";

        ei_ = ei_new_receiver(nullptr);
        ei_configure_name(ei_, "aoahid_share");
        // libei owns the descriptor from here on.
        if (ei_setup_backend_fd(ei_, fd) != 0)
            return "libei could not use the input event connection";
        return {};
    }

    std::string read_zones() {
        const std::string token = next_token();
        DBusMessage* call = method("GetZones");
        DBusMessageIter iter;
        DBusMessageIter options;
        dbus_message_iter_init_append(call, &iter);
        const char* session = session_.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &session);
        dict_open(iter, options);
        dict_string(options, "handle_token", token);
        dbus_message_iter_close_container(&iter, &options);

        DBusMessage* response = nullptr;
        const std::string failure = request(call, token, call_timeout_ms, response);
        if (!failure.empty())
            return "the screen layout could not be read: " + failure;

        zones_.clear();
        DBusMessageIter results;
        DBusMessageIter value;
        if (response_results(response, results)) {
            static_cast<void>(dict_uint_value(results, "zone_set", zone_set_));
            if (dict_find(results, "zones", value) &&
                dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY) {
                DBusMessageIter zones;
                dbus_message_iter_recurse(&value, &zones);
                while (dbus_message_iter_get_arg_type(&zones) == DBUS_TYPE_STRUCT) {
                    // (uuii): width, height, x offset, y offset.
                    DBusMessageIter zone;
                    dbus_message_iter_recurse(&zones, &zone);
                    dbus_uint32_t width = 0;
                    dbus_uint32_t height = 0;
                    dbus_int32_t x = 0;
                    dbus_int32_t y = 0;
                    dbus_message_iter_get_basic(&zone, &width);
                    dbus_message_iter_next(&zone);
                    dbus_message_iter_get_basic(&zone, &height);
                    dbus_message_iter_next(&zone);
                    dbus_message_iter_get_basic(&zone, &x);
                    dbus_message_iter_next(&zone);
                    dbus_message_iter_get_basic(&zone, &y);
                    zones_.push_back({x, y, static_cast<int>(width), static_cast<int>(height)});
                    dbus_message_iter_next(&zones);
                }
            }
        }
        dbus_message_unref(response);
        return zones_.empty() ? std::string("the compositor reported no screens") : std::string();
    }

    static DBusHandlerResult filter(DBusConnection*, DBusMessage* message, void* user) {
        return static_cast<PortalCapture*>(user)->on_message(message);
    }

    DBusHandlerResult on_message(DBusMessage* message) {
        if (dbus_message_is_signal(message, request_interface, "Response")) {
            const char* path = dbus_message_get_path(message);
            if (path != nullptr && !wait_path_.empty() && wait_path_ == path &&
                wait_reply_ == nullptr)
                wait_reply_ = dbus_message_ref(message);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_is_signal(message, capture_interface, "Activated")) {
            activated(message);
        } else if (dbus_message_is_signal(message, capture_interface, "Deactivated") ||
                   dbus_message_is_signal(message, capture_interface, "Disabled")) {
            if (active_) {
                active_ = false;
                handler_->lost();
            }
            // A Disabled session stays valid; arm it again.
            if (dbus_message_is_signal(message, capture_interface, "Disabled") &&
                !barriers_.empty())
                static_cast<void>(simple_call("Enable"));
        } else if (dbus_message_is_signal(message, capture_interface, "ZonesChanged")) {
            // Not read here: asking the portal dispatches messages, which
            // cannot be done from inside a dispatch.
            tasks_.post([this] {
                if (read_zones().empty())
                    handler_->monitors_changed();
            });
        } else if (dbus_message_is_signal(message, session_interface, "Closed")) {
            const char* path = dbus_message_get_path(message);
            if (path != nullptr && session_ == path) {
                session_.clear();
                if (active_) {
                    active_ = false;
                    handler_->lost();
                }
            }
        } else {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    void activated(DBusMessage* message) {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(message, &iter) || !dbus_message_iter_next(&iter))
            return;
        uint32_t barrier_id = 0;
        static_cast<void>(dict_uint_value(iter, "activation_id", activation_));
        static_cast<void>(dict_uint_value(iter, "barrier_id", barrier_id));
        double x = 0.0;
        double y = 0.0;
        DBusMessageIter value;
        if (dict_find(iter, "cursor_position", value) &&
            dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRUCT) {
            DBusMessageIter position;
            dbus_message_iter_recurse(&value, &position);
            dbus_message_iter_get_basic(&position, &x);
            dbus_message_iter_next(&position);
            dbus_message_iter_get_basic(&position, &y);
        }
        active_ = true;
        scroll_wheel_ = scroll_pan_ = 0.0;
        scroll_discrete_ = false;

        const Barrier* hit = nullptr;
        for (const Barrier& barrier : barriers_) {
            if (barrier.id == barrier_id)
                hit = &barrier;
        }
        // The compositor already holds the input, so the push that led here
        // cannot be measured; report it as more than any threshold.
        constexpr double pushed_through = 1e9;
        bool taken = false;
        if (hit != nullptr) {
            const int along = static_cast<int>(std::lround(vertical(hit->side) ? y : x));
            taken = handler_->edge_hit(hit->id, std::clamp(along, hit->span.lo, hit->span.hi),
                                       pushed_through, false);
        }
        if (!taken) {
            // Not ours to take: hand the cursor straight back, just inside.
            int back_x = static_cast<int>(std::lround(x));
            int back_y = static_cast<int>(std::lround(y));
            if (hit != nullptr && hit->monitor < zones_.size()) {
                const Rect& zone = zones_[hit->monitor];
                back_x = std::clamp(back_x, zone.x + 1, zone.x + zone.w - 2);
                back_y = std::clamp(back_y, zone.y + 1, zone.y + zone.h - 2);
            }
            release(back_x, back_y);
        }
    }

    void read_ei() {
        bool disconnected = false;
        ei_dispatch(ei_);
        while (ei_event* event = ei_get_event(ei_)) {
            switch (ei_event_get_type(event)) {
            case EI_EVENT_SEAT_ADDED:
                ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_POINTER,
                                          EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL,
                                          EI_DEVICE_CAP_KEYBOARD, nullptr);
                break;
            case EI_EVENT_DISCONNECT:
                disconnected = true;
                if (active_) {
                    active_ = false;
                    handler_->lost();
                }
                break;
            case EI_EVENT_POINTER_MOTION:
                if (active_)
                    handler_->motion(ei_event_pointer_get_dx(event),
                                     ei_event_pointer_get_dy(event));
                break;
            case EI_EVENT_BUTTON_BUTTON: {
                unsigned button = 0;
                if (active_ && evdev_to_button(ei_event_button_get_button(event), button))
                    handler_->button(button, ei_event_button_get_is_press(event));
                break;
            }
            case EI_EVENT_SCROLL_DISCRETE:
                // 120 units per notch; down and right are positive here,
                // while a HID wheel counts up as positive.
                scroll_discrete_ = true;
                scroll_wheel_ -= ei_event_scroll_get_discrete_dy(event) / 120.0;
                scroll_pan_ += ei_event_scroll_get_discrete_dx(event) / 120.0;
                break;
            case EI_EVENT_SCROLL_DELTA:
                scroll_smooth_wheel_ -= ei_event_scroll_get_dy(event) / pixels_per_notch;
                scroll_smooth_pan_ += ei_event_scroll_get_dx(event) / pixels_per_notch;
                break;
            case EI_EVENT_KEYBOARD_KEY: {
                HidKey key;
                if (active_ && evdev_to_hid(ei_event_keyboard_get_key(event), key))
                    handler_->key(key, ei_event_keyboard_get_key_is_press(event), true);
                break;
            }
            case EI_EVENT_FRAME:
                // A wheel reports both forms in one frame; the discrete one
                // is exact, so the smooth one only counts on its own.
                if (!scroll_discrete_) {
                    scroll_wheel_ = scroll_smooth_wheel_;
                    scroll_pan_ = scroll_smooth_pan_;
                }
                if (active_ && (scroll_wheel_ != 0.0 || scroll_pan_ != 0.0))
                    handler_->scroll(scroll_wheel_, scroll_pan_);
                scroll_wheel_ = scroll_pan_ = scroll_smooth_wheel_ = scroll_smooth_pan_ = 0.0;
                scroll_discrete_ = false;
                break;
            default:
                break;
            }
            ei_event_unref(event);
        }
        // Its descriptor stays readable at end of file: left polled, run()
        // would spin.
        if (disconnected) {
            ei_unref(ei_);
            ei_ = nullptr;
        }
    }

    CaptureHandler* handler_{};
    DBusConnection* connection_{};
    ei* ei_{};
    TaskQueue tasks_;
    std::atomic<bool> stopping_{};

    std::string sender_;
    std::string session_;
    unsigned token_counter_{};
    std::string wait_path_;
    DBusMessage* wait_reply_{};

    std::vector<Rect> zones_;
    uint32_t zone_set_{};
    std::vector<Barrier> barriers_;
    bool active_{};
    // Enable has been called since the barriers were last set.
    bool enabled_{};
    uint32_t activation_{};

    double scroll_wheel_{};
    double scroll_pan_{};
    double scroll_smooth_wheel_{};
    double scroll_smooth_pan_{};
    bool scroll_discrete_{};
};

} // namespace

std::unique_ptr<Capture> make_portal_capture() { return std::make_unique<PortalCapture>(); }

} // namespace aoas
