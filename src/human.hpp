#pragma once

// Human layouts. Each takes the tab-separated records the lines layout prints and arranges them
// for reading: grouped, aligned, coloured and decorated with glyphs. Both layouts carry the same
// information, so tests of the records cover both.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

// A tone's colour: 24-bit, and the nearest xterm-256 entry.
struct ToneStyle {
    bool bold = false;
    bool italic = false;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t xterm = 0;
};

[[nodiscard]] ToneStyle tone_style(Tone tone);

// Dependency kinds in the order depclean reads them, with their shorthand letters.
struct KindShorthand {
    std::string_view name;
    std::string_view letter;
    std::string_view meaning;
    Tone tone;
};

inline constexpr std::array<KindShorthand, 5> kind_shorthands{{
    {.name = "RDEPEND", .letter = "R", .meaning = "runtime", .tone = Tone::runtime},
    {.name = "IDEPEND", .letter = "I", .meaning = "install", .tone = Tone::install},
    {.name = "PDEPEND", .letter = "P", .meaning = "post", .tone = Tone::post},
    {.name = "DEPEND", .letter = "D", .meaning = "build", .tone = Tone::build},
    {.name = "BDEPEND", .letter = "B", .meaning = "build host", .tone = Tone::host},
}};

// A cpv's parts; the version is empty for a bare cp, the category for a bare name.
struct CpvParts {
    std::string_view category;
    std::string_view name;
    std::string_view version;
};

[[nodiscard]] CpvParts split_cpv(std::string_view cpv);

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
    // Tree parts beside branch (the last child): a child with siblings after it, an ancestor's
    // line passing by, a node that can unfold, one unfolded, and one already on its own path.
    std::string_view tee;
    std::string_view rail;
    std::string_view folded;
    std::string_view unfolded;
    std::string_view cycle;
    // Before what is installed in place of a dependency.
    std::string_view instead;
    // A pending update: a newer version, an older one, the same one rebuilt, and a package new in
    // its slot; and one that dependents hold back.
    std::string_view upgrade;
    std::string_view downgrade;
    std::string_view rebuild;
    std::string_view added;
    std::string_view held;
    // A box's corners and sides.
    struct Frame {
        std::string_view top_left;
        std::string_view top_right;
        std::string_view bottom_left;
        std::string_view bottom_right;
        std::string_view across;
        std::string_view down;
    } frame;
    // A running emerge's tasks: building, installing a binary package, merging, and built but
    // waiting to merge.
    std::string_view build;
    std::string_view binary;
    std::string_view merge;
    std::string_view waiting;
    // On the merge list, not started.
    std::string_view queued;
    // A spinner's frames, one code point each.
    std::string_view spinner;
    // A progress bar's full and empty cells, and cells filled by eighths (none in ascii).
    std::string_view bar_full;
    std::string_view bar_empty;
    std::string_view bar_eighths;
    // A sparkline's eight levels, lowest first.
    std::string_view spark;
    // Key hints.
    std::string_view move;
    std::string_view enter;
    // The terminal interface's trail separator and selection marker.
    std::string_view trail;
    std::string_view cursor;
};

[[nodiscard]] const Glyphs& glyphs(GlyphSet set);

// The icon for a root set, named as "@selected".
[[nodiscard]] std::string_view set_glyph(std::string_view set, const Glyphs& glyph);

struct Theme {
    Painter paint;
    GlyphSet glyph_set = GlyphSet::nerd;

    [[nodiscard]] const Glyphs& glyph() const { return glyphs(glyph_set); }
};

// deps (reverse false) or rdeps records, one block per subject cpv in the given order: a line
// per dependency (or dependent) and atom, with the kinds it appears under as a letter matrix, and
// the toggles that would add it for a possible one (--possible).
void human_edges(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> subjects, bool reverse, const Theme& theme);

// One why path: the root record, then an edge record per link.
void human_path(std::ostream& out, std::span<const std::string> records, const Theme& theme);

void human_orphans(std::ostream& out, std::span<const std::string> records, const Theme& theme);

// broken records, then the build-time dependencies since replaced; either may carry a fourth
// field of what is installed in the dependency's place, cpvs separated by spaces.
void human_broken(std::ostream& out, std::span<const std::string> broken,
                  std::span<const std::string> replaced, const Theme& theme);

// blockers records, one block per holder: each blocker with its kind and what it blocks. named
// says packages were given, so a blocker that blocks nothing may be among them.
void human_blockers(std::ostream& out, std::span<const std::string> records, bool named,
                    const Theme& theme);

void human_soname(std::ostream& out, std::span<const std::string> records, std::string_view soname,
                  bool providers, const Theme& theme);

// A held update's holder as a sentence: the installed packages depending on it, and the root
// sets ("@selected", "@system") whose atoms select it.
[[nodiscard]] std::string holder_note(std::span<const std::string_view> dependents,
                                      std::span<const std::string_view> sets);

// One line of the commands past a held update, the label on the first line of each remedy.
struct RemedyLine {
    std::string label;
    std::string text;
    Tone tone = Tone::note;
};

// The commands past a held update to target (a cpv): with frees, removing the holders (cpvs),
// deselecting their world atoms first, then the update, naming the other held packages it
// frees; with nodeps, the update without its dependencies.
[[nodiscard]] std::vector<RemedyLine>
remedy_lines(std::span<const std::string_view> holders, std::span<const std::string_view> deselect,
             std::string_view target, const std::optional<std::vector<std::string_view>>& frees,
             bool nodeps);

// updates records: one line per package, its versions or the flags it would be rebuilt for. With
// table, records from update_lines' table: in merge order, each led by its place and followed by
// the places it waits for, new packages among them.
void human_updates(std::ostream& out, std::span<const std::string> records, const Theme& theme,
                   bool table = false);

// merge_differences' records after a plan: that emerge --pretend agrees, or where it does not;
// verb says what it does ("merges", "removes").
void human_verification(std::ostream& out, std::span<const std::string> records, const Theme& theme,
                        std::string_view verb = "merges");

// deselect_lines' records: the atoms leaving @selected, then what depclean would remove after.
void human_deselect(std::ostream& out, std::span<const std::string> records, const Theme& theme);

// selection_changes' records, after an action: what joined @selected, then what left it.
void human_selection(std::ostream& out, std::span<const std::string> records, const Theme& theme);

// notice_lines records under headings, a blank line between: the configuration files with
// updates waiting, how many when more than one, then the unread news by item; nothing for none.
void human_notices(std::ostream& out, std::span<const std::string> records, const Theme& theme);

// removal_lines' records: what goes, then what is kept with what keeps it, and a count.
void human_removal(std::ostream& out, std::span<const std::string> records, const Theme& theme);

// update_tree_lines' records as a tree under each root set, merges drawn from table (update_lines'
// table records) with their place and waits, the packages between them plain.
void human_update_tree(std::ostream& out, std::span<const std::string> table,
                       std::span<const std::string> tree, const Theme& theme);

// match records, one block per atom in the given order.
void human_match(std::ostream& out, std::span<const std::string> records,
                 std::span<const std::string> atoms, const Theme& theme);

// The legend for the kind letters and the any-of marker, and with possible the toggles.
void human_legend(std::ostream& out, const Theme& theme, bool possible = false);

// A cpv, or a dependency (an atom, or a || group as portage renders it), coloured part by part.
[[nodiscard]] std::string paint_cpv(std::string_view cpv, const Painter& paint);
[[nodiscard]] std::string paint_dependency(std::string_view text, const Painter& paint);

} // namespace egraph
