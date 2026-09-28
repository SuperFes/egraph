#pragma once

// Human layouts. Each takes the tab-separated records the lines layout prints and arranges them
// for reading: grouped, aligned, and coloured when the painter is. Both layouts carry the same
// information, so tests of the records cover both.

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace egraph {

enum class Tone : std::uint8_t { heading, package, kind, atom, choice, root, bad, note };

class Painter {
  public:
    explicit Painter(bool color) : color_(color) {}
    // text wrapped in the ANSI style for tone, or as is without colour.
    [[nodiscard]] std::string operator()(std::string_view text, Tone tone) const;

  private:
    bool color_;
};

// deps (reverse false) or rdeps records, one block per subject cpv in the given order: a line
// per dependency (or dependent) and atom, with every kind it appears under, runtime kinds first.
void human_edges(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> subjects, bool reverse, const Painter& paint);

// One why path: the root record, then an edge record per link.
void human_path(std::ostream& out, std::span<const std::string> records, const Painter& paint);

void human_orphans(std::ostream& out, std::span<const std::string> records, const Painter& paint);

void human_broken(std::ostream& out, std::span<const std::string> records, const Painter& paint);

void human_soname(std::ostream& out, std::span<const std::string> records, std::string_view soname,
                  bool providers, const Painter& paint);

// match records, one block per atom in the given order.
void human_match(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> atoms, const Painter& paint);

} // namespace egraph
