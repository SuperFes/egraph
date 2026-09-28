#pragma once

// Human layouts. Each takes the tab-separated records the lines layout prints and arranges them
// for reading: grouped, aligned, coloured and decorated with glyphs. Both layouts carry the same
// information, so tests of the records cover both.

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace egraph {

enum class Tone : std::uint8_t {
    heading,
    category,
    name,
    version,
    op,
    slot,
    use,
    repo,
    // Dependency kinds, by their shorthand letter.
    runtime,
    install,
    post,
    build,
    host,
    choice,
    root,
    bad,
    good,
    note,
    count,
};

enum class ColorDepth : std::uint8_t { none, palette, truecolor };

class Painter {
  public:
    explicit Painter(ColorDepth depth) : depth_(depth) {}
    // text wrapped in the ANSI style for tone, or as is without colour.
    [[nodiscard]] std::string operator()(std::string_view text, Tone tone) const;

  private:
    ColorDepth depth_;
};

enum class GlyphSet : std::uint8_t { nerd, unicode, ascii };

// Icons and tree parts, each one column wide (Nerd Font icons are private-use code points).
struct Glyphs {
    std::string_view package;
    std::string_view selected;
    std::string_view system;
    std::string_view profile;
    std::string_view set;
    std::string_view orphan;
    std::string_view broken;
    std::string_view soname;
    std::string_view search;
    std::string_view good;
    std::string_view choice;
    std::string_view branch;
    std::string_view absent;
};

[[nodiscard]] const Glyphs& glyphs(GlyphSet set);

struct Theme {
    Painter paint;
    GlyphSet glyph_set = GlyphSet::nerd;

    [[nodiscard]] const Glyphs& glyph() const { return glyphs(glyph_set); }
};

// deps (reverse false) or rdeps records, one block per subject cpv in the given order: a line
// per dependency (or dependent) and atom, with the kinds it appears under as a letter matrix.
void human_edges(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> subjects, bool reverse, const Theme& theme);

// One why path: the root record, then an edge record per link.
void human_path(std::ostream& out, std::span<const std::string> records, const Theme& theme);

void human_orphans(std::ostream& out, std::span<const std::string> records, const Theme& theme);

void human_broken(std::ostream& out, std::span<const std::string> records, const Theme& theme);

void human_soname(std::ostream& out, std::span<const std::string> records, std::string_view soname,
                  bool providers, const Theme& theme);

// match records, one block per atom in the given order.
void human_match(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> atoms, const Theme& theme);

// The legend for the kind letters and the any-of marker.
void human_legend(std::ostream& out, const Theme& theme);

// A cpv, or a dependency (an atom, or a || group as portage renders it), coloured part by part.
[[nodiscard]] std::string paint_cpv(std::string_view cpv, const Painter& paint);
[[nodiscard]] std::string paint_dependency(std::string_view text, const Painter& paint);

} // namespace egraph
