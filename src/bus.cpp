#include "bus.hpp"

#include <systemd/sd-bus.h>

#include <algorithm>
#include <cerrno>
#include <format>
#include <system_error>

namespace egraph::bus {

namespace {

constexpr const char* service = "org.freedesktop.Notifications";
constexpr const char* object = "/org/freedesktop/Notifications";

struct MessageUnref {
    void operator()(sd_bus_message* message) const { sd_bus_message_unref(message); }
};
using Message = std::unique_ptr<sd_bus_message, MessageUnref>;

// An sd_bus_error freed when it goes.
class Error {
  public:
    Error() = default;
    Error(const Error&) = delete;
    Error& operator=(const Error&) = delete;
    Error(Error&&) = delete;
    Error& operator=(Error&&) = delete;
    ~Error() { sd_bus_error_free(&error_); }
    sd_bus_error* get() { return &error_; }
    // The server's message where it sent one, else the errno's.
    [[nodiscard]] std::string text(int result) const {
        if (error_.message != nullptr) {
            return error_.message;
        }
        return std::error_code(-result, std::generic_category()).message();
    }

  private:
    sd_bus_error error_{};
};

std::string errno_text(int result) {
    return std::error_code(-result, std::generic_category()).message();
}

std::expected<Message, std::string> method_call(sd_bus* bus, const char* member) {
    sd_bus_message* raw = nullptr;
    if (const int r = sd_bus_message_new_method_call(bus, &raw, service, object, service, member);
        r < 0) {
        return std::unexpected(errno_text(r));
    }
    return Message{raw};
}

// The call's reply; its error, the server's or the bus's, as text.
std::expected<Message, std::string> call(sd_bus* bus, const Message& message) {
    Error error;
    sd_bus_message* raw = nullptr;
    if (const int r = sd_bus_call(bus, message.get(), 0, error.get(), &raw); r < 0) {
        return std::unexpected(error.text(r));
    }
    return Message{raw};
}

int on_action(sd_bus_message* message, void* userdata, sd_bus_error*) {
    std::uint32_t id = 0;
    const char* key = nullptr;
    if (sd_bus_message_read_basic(message, 'u', &id) <= 0 ||
        sd_bus_message_read_basic(message, 's', static_cast<void*>(&key)) <= 0 || key == nullptr) {
        return 0;
    }
    auto& inbox = *static_cast<Session::Inbox*>(userdata);
    if (std::ranges::contains(inbox.posted, id)) {
        inbox.events.push_back({.kind = Event::Kind::action, .id = id, .action = key});
    }
    return 0;
}

int on_closed(sd_bus_message* message, void* userdata, sd_bus_error*) {
    std::uint32_t id = 0;
    if (sd_bus_message_read_basic(message, 'u', &id) <= 0) {
        return 0;
    }
    auto& inbox = *static_cast<Session::Inbox*>(userdata);
    if (std::erase(inbox.posted, id) > 0) {
        inbox.events.push_back({.kind = Event::Kind::closed, .id = id, .action = {}});
    }
    return 0;
}

} // namespace

void Session::Unref::operator()(sd_bus* bus) const {
    sd_bus_flush_close_unref(bus);
}

std::expected<Session, std::string> Session::open() {
    sd_bus* raw = nullptr;
    if (const int r = sd_bus_open_user(&raw); r < 0) {
        return std::unexpected("cannot connect to the session bus: " + errno_text(r));
    }
    std::unique_ptr<sd_bus, Unref> bus{raw};
    auto inbox = std::make_unique<Inbox>();
    // Floating matches: they last as long as the connection.
    for (const auto& [member, handler] :
         {std::pair{"ActionInvoked", &on_action}, std::pair{"NotificationClosed", &on_closed}}) {
        if (const int r = sd_bus_match_signal(bus.get(), nullptr, nullptr, object, service, member,
                                              handler, inbox.get());
            r < 0) {
            return std::unexpected("cannot connect to the session bus: " + errno_text(r));
        }
    }
    return Session{std::move(bus), std::move(inbox)};
}

std::expected<bool, std::string> Session::claim(const std::string& name) {
    const int r = sd_bus_request_name(bus_.get(), name.c_str(), 0);
    if (r == -EEXIST) {
        return false;
    }
    if (r == -EALREADY) {
        return true;
    }
    if (r < 0) {
        return std::unexpected(std::format("cannot take the name {}: {}", name, errno_text(r)));
    }
    return true;
}

std::expected<void, std::string> Session::release(const std::string& name) {
    if (const int r = sd_bus_release_name(bus_.get(), name.c_str()); r < 0) {
        return std::unexpected(std::format("cannot give up the name {}: {}", name, errno_text(r)));
    }
    return {};
}

std::expected<std::uint32_t, std::string> Session::notify(const Notification& notification) {
    auto message = method_call(bus_.get(), "Notify");
    const auto failed = [](const std::string& why) {
        return std::unexpected("cannot post the notification: " + why);
    };
    if (!message) {
        return failed(message.error());
    }
    sd_bus_message* m = message->get();
    const auto string = [m](const std::string& text) {
        return sd_bus_message_append_basic(m, 's', text.c_str());
    };
    int r = string("egraph");
    r = r < 0 ? r : sd_bus_message_append_basic(m, 'u', &notification.replaces);
    r = r < 0 ? r : string("");
    r = r < 0 ? r : string(notification.summary);
    r = r < 0 ? r : string(notification.body);
    r = r < 0 ? r : sd_bus_message_open_container(m, 'a', "s");
    for (const auto& [key, label] : notification.actions) {
        r = r < 0 ? r : string(key);
        r = r < 0 ? r : string(label);
    }
    r = r < 0 ? r : sd_bus_message_close_container(m);
    r = r < 0 ? r : sd_bus_message_open_container(m, 'a', "{sv}");
    r = r < 0 ? r : sd_bus_message_close_container(m);
    r = r < 0 ? r : sd_bus_message_append_basic(m, 'i', &notification.expire);
    if (r < 0) {
        return failed(errno_text(r));
    }
    const auto reply = call(bus_.get(), *message);
    if (!reply) {
        return failed(reply.error());
    }
    std::uint32_t id = 0;
    if (const int read = sd_bus_message_read_basic(reply->get(), 'u', &id); read <= 0) {
        return failed(read < 0 ? errno_text(read) : "the server answered without an id");
    }
    if (!std::ranges::contains(inbox_->posted, id)) {
        inbox_->posted.push_back(id);
    }
    return id;
}

std::expected<void, std::string> Session::close(std::uint32_t id) {
    auto message = method_call(bus_.get(), "CloseNotification");
    if (!message) {
        return std::unexpected("cannot close the notification: " + message.error());
    }
    if (const int r = sd_bus_message_append_basic(message->get(), 'u', &id); r < 0) {
        return std::unexpected("cannot close the notification: " + errno_text(r));
    }
    return call(bus_.get(), *message)
        .transform([](const Message&) {})
        .transform_error(
            [](const std::string& why) { return "cannot close the notification: " + why; });
}

std::expected<std::vector<Event>, std::string> Session::events() {
    int r = 0;
    while ((r = sd_bus_process(bus_.get(), nullptr)) > 0) {
    }
    if (r < 0) {
        return std::unexpected("the session bus failed: " + errno_text(r));
    }
    return std::exchange(inbox_->events, {});
}

os::Pollable Session::pollable() const {
    return {.fd = sd_bus_get_fd(bus_.get()), .events = sd_bus_get_events(bus_.get())};
}

} // namespace egraph::bus
