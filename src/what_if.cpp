#include "what_if.hpp"

#include "atom.hpp"
#include "use_changes.hpp"
#include "use_stack.hpp"
#include "version.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <system_error>

namespace egraph {

namespace {

std::vector<std::string> words_of(std::string_view text) {
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto start = text.find_first_not_of(" \t", at);
        if (start == std::string_view::npos) {
            break;
        }
        const auto end = std::min(text.find_first_of(" \t", start), text.size());
        words.emplace_back(text.substr(start, end - start));
        at = end;
    }
    return words;
}

// A package.use line's flags with "VAR:" prefixes put on, as UseManager reads them.
std::vector<std::string> expanded(const std::vector<std::string>& tokens) {
    std::vector<std::string> out;
    std::string prefix;
    for (const auto& token : tokens) {
        if (token.ends_with(':')) {
            prefix = token.substr(0, token.size() - 1);
            std::ranges::transform(prefix, prefix.begin(), [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            });
            prefix += '_';
        } else if (token.starts_with('-')) {
            out.push_back("-" + prefix + token.substr(1));
        } else {
            out.push_back(prefix + token);
        }
    }
    return out;
}

// The number of lines in the file at path, 0 when it cannot be read.
std::uint32_t lines_in(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::uint32_t count = 0;
    for (std::string line; std::getline(in, line);) {
        ++count;
    }
    return count;
}

bool under(const std::filesystem::path& file, const std::filesystem::path& directory) {
    const auto relative = file.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
}

// Calls f with every range of Evaluated::ledger_entries the ledger holds.
template <class F> void each_entry_range(Ledger& ledger, F f) {
    for (auto& node : ledger.profiles) {
        std::ranges::for_each(node.sources, f);
    }
    for (auto& repo : ledger.repositories) {
        std::ranges::for_each(repo.sources, f);
    }
    f(ledger.conf);
    f(ledger.package_use);
    f(ledger.package_env);
    for (auto& file : ledger.env_files) {
        f(file.entries);
    }
    f(ledger.env);
    f(ledger.env_d);
}

// entries into target at offset, the ranges after it moved along.
void insert_entries(Evaluated& evaluated, Range& target, std::uint32_t offset,
                    const std::vector<LedgerEntry>& entries) {
    const auto at = target.first + offset;
    const auto count = static_cast<std::uint32_t>(entries.size());
    evaluated.ledger_entries.insert(
        std::next(evaluated.ledger_entries.begin(), static_cast<std::ptrdiff_t>(at)),
        entries.begin(), entries.end());
    each_entry_range(evaluated.ledger, [&](Range& range) {
        if (&range != &target && range.first >= at) {
            range.first += count;
        }
    });
    target.count += count;
}

// Where in source a line of the file at path goes: after every entry of a file read before it
// (portage reads a directory depth first, its entries sorted), or of the file itself; with stop,
// before the first entry stop holds for.
template <class Stop>
std::uint32_t position_in(const Evaluated& evaluated, Range source,
                          const std::filesystem::path& path, Stop stop) {
    std::uint32_t position = 0;
    for (std::uint32_t i = 0; i < source.count; ++i) {
        const std::filesystem::path file{
            evaluated.string(evaluated.ledger_entries.at(source.first + i).file)};
        if (stop(file)) {
            break;
        }
        if (file.compare(path) <= 0) {
            position = i + 1;
        }
    }
    return position;
}

// Whether a flag starting with prefix may be in the candidate's IUSE, explicit or implicit.
bool may_have_prefix(const Store& installed, const Evaluated& evaluated, const Candidate& candidate,
                     std::string_view prefix) {
    const auto starts = [prefix](std::string_view flag) { return flag.starts_with(prefix); };
    if (std::ranges::any_of(evaluated.ids_in(candidate.iuse),
                            [&](std::uint32_t id) { return starts(evaluated.string(id)); })) {
        return true;
    }
    const auto& implicit = installed.implicit;
    if (candidate.iuse_effective) {
        return std::ranges::any_of(implicit.effective, starts);
    }
    return std::ranges::any_of(implicit.literals, starts) ||
           std::ranges::any_of(implicit.prefixes, [prefix](std::string_view other) {
               return other.starts_with(prefix) || prefix.starts_with(other);
           });
}

// Whether line may change the candidate's USE: an atom that could match it, and a flag in its
// IUSE (a flag token changes no other flag; "-*", "prefix_*" and env files may change any).
bool may_change(const Store& installed, const Evaluated& evaluated, const Candidate& candidate,
                const WhatIfLine& line) {
    if (const auto atom = parse_config_atom(line.atom);
        atom && !atom->extended && atom->cp != evaluated.string(candidate.cp)) {
        return false;
    }
    if (line.file == WhatIfLine::File::env) {
        return true;
    }
    return std::ranges::any_of(expanded(line.tokens), [&](std::string_view token) {
        if (token.starts_with('-')) {
            token.remove_prefix(1);
        }
        if (token == "*") {
            return true;
        }
        if (token.ends_with("_*")) {
            return may_have_prefix(installed, evaluated, candidate,
                                   token.substr(0, token.size() - 1));
        }
        return in_iuse(installed, evaluated, candidate, token);
    });
}

std::set<std::string, std::less<>> use_set(const StackedUse& stacked) {
    return {stacked.use.begin(), stacked.use.end()};
}

std::set<std::string, std::less<>> names(const Tables& tables, Range range) {
    std::set<std::string, std::less<>> found;
    for (const auto id : tables.ids_in(range)) {
        auto flag = tables.string(id);
        if (flag.starts_with('+') || flag.starts_with('-')) {
            flag.remove_prefix(1);
        }
        found.emplace(flag);
    }
    return found;
}

// The flag a --newuse entry ("flag*", "-flag%", "(-flag%*)") names.
std::string_view rebuild_flag(std::string_view entry) {
    while (!entry.empty() && (entry.front() == '(' || entry.front() == '-')) {
        entry.remove_prefix(1);
    }
    while (!entry.empty() && (entry.back() == ')' || entry.back() == '*' || entry.back() == '%')) {
        entry.remove_suffix(1);
    }
    return entry;
}

// --newuse's flags for an installed package against a candidate, as the builder's rebuild_flags
// gives them: those whose state changed, and those its IUSE gained or lost but the profile
// fixes, which no line tried changes and so are kept from previous.
std::vector<std::string> rebuild_entries(const Store& installed, const Package& pkg,
                                         const Evaluated& evaluated, const Candidate& candidate,
                                         Range previous) {
    const auto old_iuse = names(installed, pkg.iuse);
    const auto iuse = names(evaluated, candidate.iuse);
    const auto old_use = names(installed, pkg.use);
    const auto use = names(evaluated, candidate.use);
    std::set<std::string, std::less<>> was_on;
    std::set<std::string, std::less<>> now_on;
    std::ranges::set_intersection(old_iuse, old_use, std::inserter(was_on, was_on.end()));
    std::ranges::set_intersection(iuse, use, std::inserter(now_on, now_on.end()));
    std::set<std::string, std::less<>> state;
    std::ranges::set_symmetric_difference(was_on, now_on, std::inserter(state, state.end()));
    auto flags = state;
    for (const auto entry : evaluated.ids_in(previous)) {
        if (const auto text = evaluated.string(entry); text.contains('%')) {
            flags.emplace(rebuild_flag(text));
        }
    }
    std::vector<std::string> entries;
    for (const auto& flag : flags) {
        const auto star = state.contains(flag) ? "*" : "";
        if (iuse.contains(flag)) {
            entries.push_back(std::format("{}{}{}{}", use.contains(flag) ? "" : "-", flag,
                                          old_iuse.contains(flag) ? "" : "%", star));
        } else {
            entries.push_back(std::format("(-{}%{})", flag, star));
        }
    }
    return entries;
}

// The installed packages' --newuse flags against candidates whose USE changed, anew: from their
// own version's ebuild, and from the best visible version in their slot where that is their own
// version, which becomes their target where any flags are left and stops being one where none
// are.
void rebuild_installed(Evaluated& evaluated, const Store& installed,
                       const std::set<std::uint32_t>& changed) {
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp;
    for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
        by_cp[std::string{evaluated.string(evaluated.candidates.at(i).cp)}].push_back(i);
    }
    Interner intern(evaluated);
    const auto ids_of = [&](const std::vector<std::string>& entries) {
        const Range range{.first = static_cast<std::uint32_t>(evaluated.ids.size()),
                          .count = static_cast<std::uint32_t>(entries.size())};
        for (const auto& entry : entries) {
            const auto interned = intern(entry);
            evaluated.ids.push_back(interned);
        }
        return range;
    };
    for (std::uint32_t id = 0; id < installed.packages.size(); ++id) {
        const auto& pkg = installed.packages.at(id);
        if (const auto own = evaluated.packages.at(id).own; own && changed.contains(*own)) {
            const auto entries =
                rebuild_entries(installed, pkg, evaluated, evaluated.candidates.at(*own),
                                evaluated.packages.at(id).own_rebuild);
            evaluated.packages.at(id).own_rebuild = ids_of(entries);
        }
        const auto cp = installed.string(pkg.cp);
        const auto found = by_cp.find(cp);
        if (found == by_cp.end()) {
            continue;
        }
        const auto slot = installed.string(pkg.slot);
        std::optional<std::uint32_t> best;
        std::optional<Version> best_version;
        for (const auto index : found->second) {
            const auto& candidate = evaluated.candidates.at(index);
            if (!candidate.visible() || evaluated.string(candidate.slot) != slot) {
                continue;
            }
            const auto cpv = evaluated.string(candidate.cpv);
            auto version = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
            if (version && (!best_version || vercmp(*version, *best_version) > 0)) {
                best = index;
                best_version = std::move(version);
            }
        }
        const auto cpv = installed.string(pkg.cpv);
        const auto version = parse_version(cpv.substr(std::min(cpv.size(), cp.size() + 1)));
        if (!best || !best_version || !changed.contains(*best) || !version ||
            vercmp(*version, *best_version) != 0) {
            continue;
        }
        auto& dependencies = evaluated.packages.at(id);
        const auto entries =
            rebuild_entries(installed, pkg, evaluated, evaluated.candidates.at(*best),
                            dependencies.target == best ? dependencies.rebuild : Range{});
        dependencies.rebuild = ids_of(entries);
        if (entries.empty()) {
            dependencies.target.reset();
        } else {
            dependencies.target = best;
        }
    }
}

} // namespace

std::expected<WhatIfLine, std::string> parse_what_if(WhatIfLine::File file, std::string_view text) {
    auto words = words_of(text);
    if (words.empty()) {
        return std::unexpected("an empty line");
    }
    WhatIfLine line{.file = file, .atom = "*/*", .tokens = {}};
    if (words.front().contains('/')) {
        if (const auto atom = parse_config_atom(words.front()); !atom) {
            return std::unexpected(std::format("{}: not an atom", words.front()));
        }
        line.atom = words.front();
        words.erase(words.begin());
    } else if (file == WhatIfLine::File::env) {
        return std::unexpected(
            std::format("{}: env files are for an atom, */* for every package", text));
    }
    if (words.empty()) {
        return std::unexpected(std::format("{}: no {}", line.atom,
                                           file == WhatIfLine::File::use ? "flags" : "env files"));
    }
    line.tokens = std::move(words);
    return line;
}

std::filesystem::path what_if_path(const std::filesystem::path& user_config,
                                   WhatIfLine::File file) {
    auto path = user_config / (file == WhatIfLine::File::use ? "package.use" : "package.env");
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        return path;
    }
    return path / "egraph";
}

std::expected<Evaluated, std::string> with_what_if(Evaluated evaluated, const Store& installed,
                                                   std::span<const WhatIfLine> lines,
                                                   const std::filesystem::path& user_config) {
    if (lines.empty()) {
        return evaluated;
    }
    std::vector<std::uint32_t> candidates;
    for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
        if (std::ranges::any_of(lines, [&](const WhatIfLine& line) {
                return may_change(installed, evaluated, evaluated.candidates.at(i), line);
            })) {
            candidates.push_back(i);
        }
    }
    std::vector<std::set<std::string, std::less<>>> before;
    {
        const UseStacker stacker(installed, evaluated);
        for (const auto index : candidates) {
            before.push_back(use_set(stacker.stack(evaluated.candidates.at(index))));
        }
    }

    const auto use_path = what_if_path(user_config, WhatIfLine::File::use);
    const auto env_path = what_if_path(user_config, WhatIfLine::File::env);
    const auto use_directory = user_config / "package.use";
    const auto env_directory = user_config / "env";
    // Each file's next line: on from its last on disk, or in the ledger.
    const auto first_line = [&](const std::filesystem::path& path) {
        auto last = lines_in(path);
        for (const auto& entry : evaluated.ledger_entries) {
            if (evaluated.string(entry.file) == path.native()) {
                last = std::max(last, entry.line);
            }
        }
        return last + 1;
    };
    auto next_use = first_line(use_path);
    auto next_env = first_line(env_path);

    Interner intern(evaluated);
    const auto ids_of = [&](const std::vector<std::string>& tokens) {
        const Range range{.first = static_cast<std::uint32_t>(evaluated.ids.size()),
                          .count = static_cast<std::uint32_t>(tokens.size())};
        for (const auto& token : tokens) {
            const auto id = intern(token);
            evaluated.ids.push_back(id);
        }
        return range;
    };
    const auto entry_of = [&](const std::filesystem::path& path, std::uint32_t line,
                              const WhatIfLine& what_if, const std::vector<std::string>& tokens) {
        return LedgerEntry{.file = intern(path.native()),
                           .line = line,
                           .atom = intern(what_if.atom),
                           .var = intern("USE"),
                           .tokens = ids_of(tokens)};
    };
    const auto never = [](const std::filesystem::path&) { return false; };

    for (const auto& line : lines) {
        auto& ledger = evaluated.ledger;
        const bool everywhere = line.atom == "*/*";
        if (line.file == WhatIfLine::File::use) {
            const auto entry = entry_of(use_path, next_use++, line, expanded(line.tokens));
            if (everywhere) {
                // make.conf, then package.use's */* lines, then those of package.env's env files.
                const auto offset =
                    position_in(evaluated, ledger.conf, use_path, [&](const auto& file) {
                        return under(file, env_directory) ||
                               (under(file, use_directory) && file.compare(use_path) > 0);
                    });
                insert_entries(evaluated, ledger.conf, offset, {entry});
            } else {
                insert_entries(evaluated, ledger.package_use,
                               position_in(evaluated, ledger.package_use, use_path, never),
                               {entry});
            }
            continue;
        }
        std::vector<Range> files;
        for (const auto& name : line.tokens) {
            const auto found = std::ranges::find_if(ledger.env_files, [&](const auto& file) {
                return evaluated.string(file.name) == name;
            });
            if (found == ledger.env_files.end()) {
                return std::unexpected(std::format("env/{}: no such env file", name));
            }
            files.push_back(found->entries);
        }
        if (everywhere) {
            std::vector<LedgerEntry> entries;
            for (const auto range : files) {
                const auto in = evaluated.entries_in(range);
                entries.insert(entries.end(), in.begin(), in.end());
            }
            insert_entries(evaluated, ledger.conf, ledger.conf.count, entries);
            ++next_env;
        } else {
            const auto entry = entry_of(env_path, next_env++, line, line.tokens);
            insert_entries(evaluated, ledger.package_env,
                           position_in(evaluated, ledger.package_env, env_path, never), {entry});
        }
    }

    std::vector<UseChange> changes;
    {
        const UseStacker stacker(installed, evaluated);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto index = candidates.at(i);
            const auto after = use_set(stacker.stack(evaluated.candidates.at(index)));
            UseChange change{.candidate = index, .flags = {}};
            for (const auto& flag : before.at(i)) {
                if (!after.contains(flag)) {
                    change.flags.emplace(flag, false);
                }
            }
            for (const auto& flag : after) {
                if (!before.at(i).contains(flag)) {
                    change.flags.emplace(flag, true);
                }
            }
            if (!change.flags.empty()) {
                changes.push_back(std::move(change));
            }
        }
    }
    std::set<std::uint32_t> changed;
    for (const auto& change : changes) {
        changed.insert(change.candidate);
    }
    auto tried = with_use_changes(std::move(evaluated), installed, changes);
    rebuild_installed(tried, installed, changed);
    return tried;
}

std::vector<std::string> newly_reached(const Evaluated& before, const Evaluated& tried) {
    std::set<std::string_view> evaluated;
    for (const auto& candidate : tried.candidates) {
        evaluated.insert(tried.string(candidate.cp));
    }
    std::set<std::string_view> repository;
    for (const auto id : tried.ids_in(tried.repository_cps)) {
        repository.insert(tried.string(id));
    }
    std::set<std::string> found;
    for (std::size_t i = 0; i < tried.candidates.size() && i < before.candidates.size(); ++i) {
        const auto& candidate = tried.candidates.at(i);
        const auto& was = before.candidates.at(i);
        if (candidate.use.first == was.use.first && candidate.use.count == was.use.count) {
            continue;
        }
        for (const auto range : candidate.deps) {
            for (const auto& node : tried.nodes_in(range)) {
                if (node.type != NodeType::atom) {
                    continue;
                }
                const auto atom = parse_atom(tried.string(node.atom));
                if (atom && repository.contains(atom->cp) && !evaluated.contains(atom->cp)) {
                    found.insert(atom->cp);
                }
            }
        }
    }
    return {found.begin(), found.end()};
}

std::string saved_text(std::string_view existing, std::span<const WhatIfLine> lines, bool own) {
    const auto written = [](const std::string& atom, const std::vector<std::string>& tokens) {
        std::string text = atom;
        for (const auto& token : tokens) {
            text += " " + token;
        }
        return text + "\n";
    };
    if (!own) {
        std::string text{existing};
        if (!text.empty() && !text.ends_with('\n')) {
            text += '\n';
        }
        for (const auto& line : lines) {
            text += written(line.atom, line.tokens);
        }
        return text;
    }
    // The file's lines: a comment or blank one as it is, another by its atom and tokens, those
    // of a line saving touched rewritten.
    struct Held {
        std::string text;
        std::optional<std::string> atom;
        std::vector<std::string> tokens;
        bool touched = false;
    };
    std::vector<Held> held;
    for (const auto part : std::views::split(existing, '\n')) {
        const std::string_view text{part};
        std::vector<std::string> words;
        for (const auto word : std::views::split(text, ' ')) {
            for (const auto each : std::views::split(std::string_view{word}, '\t')) {
                if (!std::string_view{each}.empty()) {
                    words.emplace_back(std::string_view{each});
                }
            }
        }
        if (words.empty() || words.front().starts_with('#')) {
            held.push_back({.text = std::string{text}, .atom = std::nullopt, .tokens = {}});
        } else {
            held.push_back({.text = std::string{text},
                            .atom = words.front(),
                            .tokens = {words.begin() + 1, words.end()}});
        }
    }
    // What split leaves after the last newline.
    if (!held.empty() && held.back().text.empty() && !held.back().atom) {
        held.pop_back();
    }
    if (held.empty()) {
        held.push_back(
            {.text = "# Lines egraph tried and saved; egraph keeps one line per atom here.",
             .atom = std::nullopt,
             .tokens = {}});
    }
    const auto name = [](std::string_view token) {
        return token.starts_with('-') ? token.substr(1) : token;
    };
    for (const auto& line : lines) {
        const bool use = line.file == WhatIfLine::File::use;
        const auto tokens = use ? expanded(line.tokens) : line.tokens;
        const auto named = [&](const std::string& token) {
            return std::ranges::any_of(
                tokens, [&](const std::string& other) { return name(other) == name(token); });
        };
        std::optional<std::size_t> last;
        for (std::size_t i = 0; i < held.size(); ++i) {
            auto& each = held.at(i);
            if (each.atom != line.atom) {
                continue;
            }
            if (!each.touched && use) {
                each.tokens = expanded(each.tokens);
            }
            each.touched = true;
            std::erase_if(each.tokens, named);
            last = i;
        }
        if (!last) {
            held.push_back({.text = {}, .atom = line.atom, .tokens = {}, .touched = true});
            last = held.size() - 1;
        }
        auto& into = held.at(*last).tokens;
        for (const auto& token : tokens) {
            std::erase_if(into, [&](const std::string& old) { return name(old) == name(token); });
            into.push_back(token);
        }
    }
    std::string text;
    for (const auto& each : held) {
        if (!each.atom || !each.touched) {
            text += each.text + "\n";
        } else if (!each.tokens.empty()) {
            text += written(*each.atom, each.tokens);
        }
    }
    return text;
}

std::expected<std::vector<SavedFile>, std::string>
saved_files(const std::filesystem::path& user_config, std::span<const WhatIfLine> lines) {
    std::vector<SavedFile> files;
    for (const auto kind : {WhatIfLine::File::use, WhatIfLine::File::env}) {
        std::vector<WhatIfLine> of_kind;
        std::ranges::copy_if(lines, std::back_inserter(of_kind),
                             [kind](const WhatIfLine& line) { return line.file == kind; });
        if (of_kind.empty()) {
            continue;
        }
        auto path = what_if_path(user_config, kind);
        std::string existing;
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            std::ifstream in(path, std::ios::binary);
            std::stringstream read;
            read << in.rdbuf();
            if (!in) {
                return std::unexpected(std::format("cannot read {}", path.string()));
            }
            existing = read.str();
        }
        const bool own = path.filename() == "egraph";
        files.push_back({.path = std::move(path), .text = saved_text(existing, of_kind, own)});
    }
    return files;
}

std::expected<void, std::string> write_saved(std::span<const SavedFile> files) {
    namespace fs = std::filesystem;
    for (const auto& file : files) {
        const auto cannot = [&file] {
            return std::unexpected(std::format("cannot write {}", file.path.string()));
        };
        std::error_code error;
        fs::create_directories(file.path.parent_path(), error);
        if (error) {
            return cannot();
        }
        // Hidden, so that portage passes over it while it is there.
        const auto beside =
            file.path.parent_path() / std::format(".{}.new", file.path.filename().string());
        {
            std::ofstream out(beside, std::ios::binary | std::ios::trunc);
            out << file.text;
            if (!out.flush()) {
                fs::remove(beside, error);
                return cannot();
            }
        }
        if (const auto status = fs::status(file.path, error); !error && fs::exists(status)) {
            fs::permissions(beside, status.permissions(), error);
        }
        fs::rename(beside, file.path, error);
        if (error) {
            fs::remove(beside, error);
            return cannot();
        }
    }
    return {};
}

std::vector<std::string> tried_lines(std::span<const std::string> before,
                                     std::span<const std::string> after) {
    // A plan's merges by first field: kind, target, repo and flags, in its order.
    struct Merge {
        std::string first;
        std::array<std::string, 4> fields;
    };
    const auto merges = [](std::span<const std::string> records) {
        std::vector<Merge> found;
        for (const auto& record : records) {
            std::vector<std::string> fields;
            std::size_t at = 0;
            while (at <= record.size()) {
                const auto end = std::min(record.find('\t', at), record.size());
                fields.push_back(record.substr(at, end - at));
                at = end + 1;
            }
            constexpr std::array<std::string_view, 5> kinds{"upgrade", "downgrade", "rebuild",
                                                            "new", "new-slot"};
            if (fields.size() < 4 || !std::ranges::contains(kinds, fields.at(1))) {
                continue;
            }
            found.push_back({.first = fields.at(0),
                             .fields = {fields.at(1), fields.at(2), fields.at(3),
                                        fields.size() > 4 ? fields.at(4) : ""}});
        }
        return found;
    };
    const auto row = [](const Merge& merge, std::string_view change) {
        const auto& [kind, target, repo, flags] = merge.fields;
        return std::format("{}\ttried\t{}\t{}\t{}\t{}\t{}", merge.first, change, kind, target, repo,
                           flags);
    };
    const auto was = merges(before);
    const auto now = merges(after);
    const auto find = [](const std::vector<Merge>& in, const std::string& first) {
        return std::ranges::find(in, first, &Merge::first);
    };
    std::vector<std::string> found;
    for (const auto& merge : now) {
        if (const auto old = find(was, merge.first); old == was.end()) {
            found.push_back(row(merge, "added"));
        } else if (old->fields != merge.fields) {
            found.push_back(row(merge, "changed"));
        }
    }
    for (const auto& merge : was) {
        if (find(now, merge.first) == now.end()) {
            found.push_back(row(merge, "dropped"));
        }
    }
    return found;
}

std::vector<EnvChange> env_changes(const Store& installed, const Evaluated& untried,
                                   const Evaluated& tried, std::span<const WhatIfLine> lines) {
    std::vector<std::string> everywhere;
    bool any = false;
    for (const auto& line : lines) {
        if (line.file == WhatIfLine::File::env) {
            any = true;
            if (line.atom == "*/*") {
                everywhere.insert(everywhere.end(), line.tokens.begin(), line.tokens.end());
            }
        }
    }
    std::vector<EnvChange> found;
    if (!any) {
        return found;
    }
    const UseStacker before(installed, untried);
    const UseStacker after(installed, tried);
    for (std::uint32_t id = 0; id < installed.packages.size() && id < untried.packages.size();
         ++id) {
        const auto own = untried.packages.at(id).own;
        if (!own) {
            continue;
        }
        EnvChange change{.package = id,
                         .before = before.env_files(untried.candidates.at(*own)),
                         .after = after.env_files(tried.candidates.at(*own))};
        change.after.insert(change.after.end(), everywhere.begin(), everywhere.end());
        if (change.before != change.after) {
            found.push_back(std::move(change));
        }
    }
    return found;
}

std::vector<std::string> env_lines(const Store& installed, std::span<const EnvChange> changes) {
    const auto joined = [](const std::vector<std::string>& files) {
        std::string text;
        for (const auto& file : files) {
            text += std::format("{}{}", text.empty() ? "" : " ", file);
        }
        return text;
    };
    std::vector<std::string> found;
    for (const auto& change : changes) {
        found.push_back(std::format("{}\tenv\t{}\t{}",
                                    installed.string(installed.packages.at(change.package).cpv),
                                    joined(change.before), joined(change.after)));
    }
    return found;
}

} // namespace egraph
