#pragma once

// The user's session bus for desktop notifications, as org.freedesktop.Notifications takes them,
// and the only place egraph calls sd-bus (in bus.cpp, built only with the notify feature). Like
// os.hpp, everything here is values.

#include "os.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct sd_bus;

namespace egraph::bus {

struct Notification {
    // Replaces the notification of this id where the server still shows it; 0 posts a new one.
    std::uint32_t replaces = 0;
    std::string summary;
    std::string body{};
    // Each action's key and label, in order; "default" is a click on the notification itself.
    std::vector<std::pair<std::string, std::string>> actions{};
    // Milliseconds shown, as the spec takes them: -1 as the server sees fit, 0 never expiring.
    std::int32_t expire = -1;
};

// An action invoked on a notification this session posted, or its closing.
struct Event {
    enum class Kind : std::uint8_t { action, closed };
    Kind kind = Kind::action;
    std::uint32_t id = 0;
    // The action's key.
    std::string action{};
    bool operator==(const Event&) const = default;
};

class Session {
  public:
    // Connects to the user's session bus (DBUS_SESSION_BUS_ADDRESS, else the runtime directory's)
    // and listens for what happens to the notifications posted.
    static std::expected<Session, std::string> open();

    // Takes the well-known name for this connection; false where another holds it.
    std::expected<bool, std::string> claim(const std::string& name);
    // Gives it back, for a process about to run another image to claim it at once.
    std::expected<void, std::string> release(const std::string& name);

    // The id the server gave the notification.
    std::expected<std::uint32_t, std::string> notify(const Notification& notification);
    std::expected<void, std::string> close(std::uint32_t id);
    // What happened since, without waiting.
    std::expected<std::vector<Event>, std::string> events();
    // What to wait on until there are more.
    [[nodiscard]] os::Pollable pollable() const;

    // Where the signal callbacks put what they find: on the heap, for a Session that moves.
    struct Inbox {
        std::vector<std::uint32_t> posted;
        std::vector<Event> events;
    };

  private:
    struct Unref {
        void operator()(sd_bus* bus) const;
    };
    Session(std::unique_ptr<sd_bus, Unref> bus, std::unique_ptr<Inbox> inbox)
        : bus_(std::move(bus)), inbox_(std::move(inbox)) {}

    std::unique_ptr<sd_bus, Unref> bus_;
    std::unique_ptr<Inbox> inbox_;
};

} // namespace egraph::bus
