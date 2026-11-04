#include "run_display.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace egraph {

namespace {

constexpr std::string_view erase_below = "\x1b[J";

bool continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

} // namespace

std::string run_duration(double seconds) {
    const auto whole = static_cast<long long>(std::llround(std::max(seconds, 0.0)));
    if (whole >= 3600) {
        return std::format("{} h {:02} min", whole / 3600, whole % 3600 / 60);
    }
    if (whole >= 60) {
        return std::format("{} min {:02} s", whole / 60, whole % 60);
    }
    return std::format("{} s", whole);
}

std::string RunDisplay::line_of(std::size_t step, std::string_view name,
                                std::string_view what) const {
    if (options_.human) {
        return std::format("({} of {}) {}: {}", step + 1, options_.total, name, what);
    }
    return std::format("{}\t{}\t{}", step + 1, name, what);
}

std::string RunDisplay::totals() const {
    auto text = std::format("{} of {} done", done_, options_.total);
    if (failures_ != 0) {
        text += std::format(", {} failed", failures_);
    }
    text += std::format(", {} elapsed", run_duration(elapsed_));
    if (remaining_) {
        text += std::format(", about {} left", run_duration(*remaining_));
    }
    return text;
}

std::string RunDisplay::cut(std::string_view text) const {
    // One column short of the width, so that a line never wraps.
    const auto room = options_.width > 1 ? options_.width - 1 : 1;
    if (text.size() <= room) {
        return std::string{text};
    }
    auto end = room;
    while (end > 0 && continuation(text.at(end))) {
        --end;
    }
    return std::string{text.substr(0, end)};
}

void RunDisplay::redraw(std::string_view above) {
    std::string frame;
    if (drawn_ != 0) {
        frame += std::format("\r\x1b[{}A", drawn_);
    }
    frame += erase_below;
    frame += above;
    drawn_ = 0;
    for (const auto& [step, line] : active_) {
        frame += cut(line) + '\n';
        ++drawn_;
    }
    frame += cut(totals()) + '\n';
    ++drawn_;
    out_ << frame << std::flush;
}

void RunDisplay::status(std::size_t step, std::string_view name, std::string_view what) {
    const auto line = line_of(step, name, what);
    if (!options_.live) {
        out_ << line << '\n' << std::flush;
        return;
    }
    active_.insert_or_assign(step, line);
    redraw({});
}

void RunDisplay::finish(std::size_t step, std::string_view name, std::string_view what,
                        bool failed) {
    ++done_;
    if (failed) {
        ++failures_;
    }
    const auto line = line_of(step, name, what);
    if (!options_.live) {
        out_ << line << '\n' << std::flush;
        return;
    }
    active_.erase(step);
    redraw(cut(line) + '\n');
}

void RunDisplay::note(std::string_view line) {
    if (!options_.live) {
        out_ << line << '\n' << std::flush;
        return;
    }
    redraw(cut(line) + '\n');
}

void RunDisplay::tick(double elapsed, std::optional<double> remaining) {
    elapsed_ = elapsed;
    remaining_ = remaining;
    if (options_.live) {
        redraw({});
    }
}

void RunDisplay::close() {
    if (options_.live && drawn_ != 0) {
        active_.clear();
        redraw({});
        drawn_ = 0;
    }
}

} // namespace egraph
