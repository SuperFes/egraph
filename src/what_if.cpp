#include "what_if.hpp"

#include "atom.hpp"
#include "use_changes.hpp"
#include "use_stack.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
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

// The candidates lines may change: those of the cps they name, or every one.
std::vector<std::uint32_t> affected(const Evaluated& evaluated, std::span<const WhatIfLine> lines) {
    std::set<std::string, std::less<>> cps;
    bool all = false;
    for (const auto& line : lines) {
        const auto atom = parse_config_atom(line.atom);
        if (!atom || atom->extended) {
            all = true;
        } else {
            cps.insert(atom->cp);
        }
    }
    std::vector<std::uint32_t> found;
    for (std::uint32_t i = 0; i < evaluated.candidates.size(); ++i) {
        if (all || cps.contains(evaluated.string(evaluated.candidates.at(i).cp))) {
            found.push_back(i);
        }
    }
    return found;
}

std::set<std::string, std::less<>> use_set(const StackedUse& stacked) {
    return {stacked.use.begin(), stacked.use.end()};
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
    const auto candidates = affected(evaluated, lines);
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
    return with_use_changes(std::move(evaluated), installed, changes);
}

} // namespace egraph
