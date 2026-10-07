#include "screen.hpp"

#include <notcurses/notcurses.h>

#include <array>
#include <chrono>
#include <ctime>
#include <string>

namespace egraph::tui {

namespace {

ncplane* plane(notcurses* terminal) {
    return notcurses_stdplane(terminal);
}

void use_pen(ncplane* target, const Pen& pen) {
    unsigned styles = NCSTYLE_NONE;
    styles |= pen.bold ? NCSTYLE_BOLD : 0U;
    styles |= pen.italic ? NCSTYLE_ITALIC : 0U;
    ncplane_set_styles(target, styles);
    if (pen.fg) {
        ncplane_set_fg_rgb8(target, pen.fg->red, pen.fg->green, pen.fg->blue);
    } else {
        ncplane_set_fg_default(target);
    }
    if (pen.bg) {
        ncplane_set_bg_rgb8(target, pen.bg->red, pen.bg->green, pen.bg->blue);
    } else {
        ncplane_set_bg_default(target);
    }
}

Key translate(std::uint32_t id) {
    if (id == static_cast<std::uint32_t>(-1)) {
        return {.kind = KeyKind::closed};
    }
    struct Named {
        std::uint32_t id;
        KeyKind kind;
    };
    const std::array<Named, 14> named{{
        {NCKEY_RESIZE, KeyKind::resize},
        {NCKEY_EOF, KeyKind::closed},
        {NCKEY_UP, KeyKind::up},
        {NCKEY_DOWN, KeyKind::down},
        {NCKEY_LEFT, KeyKind::left},
        {NCKEY_RIGHT, KeyKind::right},
        {NCKEY_PGUP, KeyKind::page_up},
        {NCKEY_PGDOWN, KeyKind::page_down},
        {NCKEY_HOME, KeyKind::home},
        {NCKEY_END, KeyKind::end},
        {NCKEY_ENTER, KeyKind::enter},
        {NCKEY_TAB, KeyKind::tab},
        {NCKEY_BACKSPACE, KeyKind::backspace},
        {NCKEY_ESC, KeyKind::escape},
    }};
    for (const auto& entry : named) {
        if (entry.id == id) {
            return {.kind = entry.kind};
        }
    }
    if (id == '\n' || id == '\r') {
        return {.kind = KeyKind::enter};
    }
    if (id < 0x110000U && !nckey_synthesized_p(id)) {
        return {.kind = KeyKind::character, .code = static_cast<char32_t>(id)};
    }
    return {};
}

} // namespace

void Screen::Stop::operator()(notcurses* terminal) const {
    notcurses_stop(terminal);
}

std::expected<std::unique_ptr<notcurses, Screen::Stop>, std::string> Screen::start() {
    notcurses_options options{};
    options.loglevel = NCLOGLEVEL_SILENT;
    options.flags = NCOPTION_SUPPRESS_BANNERS;
    notcurses* terminal = notcurses_core_init(&options, nullptr);
    if (terminal == nullptr) {
        return std::unexpected(std::string{"cannot start the terminal interface"});
    }
    return std::unique_ptr<notcurses, Stop>{terminal};
}

std::expected<Screen, std::string> Screen::open() {
    return start().transform(
        [](std::unique_ptr<notcurses, Stop> terminal) { return Screen{std::move(terminal)}; });
}

void Screen::suspend() {
    terminal_.reset();
}

std::expected<void, std::string> Screen::resume() {
    return start().transform(
        [this](std::unique_ptr<notcurses, Stop> terminal) { terminal_ = std::move(terminal); });
}

Size Screen::size() const {
    unsigned rows = 0;
    unsigned cols = 0;
    notcurses_stddim_yx(terminal_.get(), &rows, &cols);
    return {.rows = rows, .cols = cols};
}

void Screen::clear() {
    ncplane* target = plane(terminal_.get());
    use_pen(target, {});
    ncplane_erase(target);
}

void Screen::put(unsigned row, unsigned col, std::string_view text, const Pen& pen) {
    ncplane* target = plane(terminal_.get());
    use_pen(target, pen);
    // Stops at the right edge rather than wrapping; the error for the cut is expected.
    const std::string terminated{text};
    ncplane_putstr_yx(target, static_cast<int>(row), static_cast<int>(col), terminated.c_str());
}

void Screen::fill_row(unsigned row, const Pen& pen) {
    put(row, 0, std::string(size().cols, ' '), pen);
}

void Screen::render() {
    notcurses_render(terminal_.get());
}

Key Screen::read(std::optional<std::chrono::milliseconds> timeout) {
    // notcurses_get takes an absolute CLOCK_MONOTONIC deadline, which steady_clock is on Linux.
    std::optional<timespec> deadline;
    if (timeout) {
        const auto at = std::chrono::steady_clock::now().time_since_epoch() + *timeout;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(at);
        deadline = timespec{
            .tv_sec = static_cast<time_t>(seconds.count()),
            .tv_nsec = static_cast<long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(at - seconds).count())};
    }
    while (true) {
        ncinput input{};
        const auto id = notcurses_get(terminal_.get(), deadline ? &*deadline : nullptr, &input);
        if (id == 0) {
            return {.kind = KeyKind::tick};
        }
        // Terminals with the kitty protocol also report releases.
        if (input.evtype != NCTYPE_RELEASE) {
            return translate(id);
        }
    }
}

} // namespace egraph::tui
