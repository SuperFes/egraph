#include "complete.hpp"

#include <array>
#include <set>

namespace egraph {

namespace {

constexpr std::array<std::string_view, 5> sets{"@installed", "@profile", "@selected", "@system",
                                               "@world"};

// One version a word may name: installed, or in a repository.
struct Entry {
    std::string_view cp;
    std::string_view cpv;
    std::string_view slot;
    std::string_view repo;
};

std::string_view without_category(std::string_view text) {
    return text.substr(text.find('/') + 1);
}

std::string_view category(std::string_view cp) {
    return cp.substr(0, cp.find('/'));
}

class Words {
  public:
    Words(const Store& store, const Evaluated& evaluated,
          std::optional<std::reference_wrapper<const RepositoryIndex>> index, Completing what)
        : atoms_{what != Completing::installed} {
        for (const auto& pkg : store.packages) {
            entries_.push_back({.cp = store.string(pkg.cp),
                                .cpv = store.string(pkg.cpv),
                                .slot = store.string(pkg.slot),
                                .repo = store.string(pkg.repo)});
            cps_.insert(store.string(pkg.cp));
        }
        if (!atoms_) {
            return;
        }
        if (index) {
            const RepositoryIndex& repository = *index;
            for (const auto& version : repository.versions) {
                entries_.push_back({.cp = repository.string(version.cp),
                                    .cpv = repository.string(version.cpv),
                                    .slot = repository.string(version.slot),
                                    .repo = repository.string(
                                        repository.repositories.at(version.repository).name)});
            }
            for (const auto& each : repository.repositories) {
                repositories_.insert(repository.string(each.name));
            }
        }
        for (const auto& candidate : evaluated.candidates) {
            entries_.push_back({.cp = evaluated.string(candidate.cp),
                                .cpv = evaluated.string(candidate.cpv),
                                .slot = evaluated.string(candidate.slot),
                                .repo = evaluated.string(candidate.repo)});
        }
        for (const auto id : evaluated.ids_in(evaluated.repository_cps)) {
            cps_.insert(evaluated.string(id));
        }
    }

    [[nodiscard]] std::vector<std::string> complete(Completing what, std::string_view word) {
        if (what == Completing::repositories) {
            for (const auto& entry : entries_) {
                offer("", entry.repo, word);
            }
            for (const auto repo : repositories_) {
                offer("", repo, word);
            }
        } else if (word.starts_with('@')) {
            if (atoms_) {
                for (const auto set : sets) {
                    offer("", set, word);
                }
            }
        } else {
            const auto op = word.substr(0, std::min(word.find_first_not_of("<>=~"), word.size()));
            atom(op, word.substr(op.size()));
        }
        return {found_.begin(), found_.end()};
    }

  private:
    void offer(std::string_view prefix, std::string_view text, std::string_view word) {
        if (auto whole = std::string{prefix} + std::string{text}; whole.starts_with(word)) {
            found_.insert(std::move(whole));
        }
    }

    void atom(std::string_view op, std::string_view rest) {
        const auto word = std::string{op} + std::string{rest};
        if (const auto repo = rest.find("::"); repo != std::string_view::npos) {
            const auto head = std::string{op} + std::string{rest.substr(0, repo + 2)};
            for (const auto& entry : entries_) {
                offer(head, entry.repo, word);
            }
            for (const auto name : repositories_) {
                offer(head, name, word);
            }
            return;
        }
        if (const auto slot = rest.find(':'); slot != std::string_view::npos) {
            const auto head = rest.substr(0, slot);
            const auto prefix = std::string{op} + std::string{head} + ":";
            for (const auto& entry : entries_) {
                const auto named = op.empty() ? entry.cp : entry.cpv;
                if (head == named || (atoms_ && head == without_category(named))) {
                    offer(prefix, entry.slot, word);
                }
            }
            return;
        }
        if (!rest.contains('/')) {
            for (const auto cp : cps_) {
                offer(op, std::string{category(cp)} + "/", word);
            }
            // Every name at once for an empty word would bury the categories.
            if (atoms_ && !rest.empty()) {
                if (op.empty()) {
                    for (const auto cp : cps_) {
                        offer("", without_category(cp), word);
                    }
                } else {
                    for (const auto& entry : entries_) {
                        offer(op, without_category(entry.cpv), word);
                    }
                }
            }
            return;
        }
        if (!op.empty()) {
            for (const auto& entry : entries_) {
                offer(op, entry.cpv, word);
            }
            return;
        }
        for (const auto cp : cps_) {
            offer("", cp, word);
        }
        if (!atoms_) {
            for (const auto& entry : entries_) {
                if (rest.size() > entry.cp.size()) {
                    offer("", entry.cpv, word);
                }
            }
        }
    }

    bool atoms_;
    std::vector<Entry> entries_;
    std::set<std::string_view> repositories_;
    std::set<std::string_view> cps_;
    std::set<std::string> found_;
};

} // namespace

std::vector<std::string>
complete_word(const Store& store, const Evaluated& evaluated,
              std::optional<std::reference_wrapper<const RepositoryIndex>> index, Completing what,
              std::string_view word) {
    return Words{store, evaluated, index, what}.complete(what, word);
}

bool needs_index(Completing what, std::string_view word) {
    if (what == Completing::installed) {
        return false;
    }
    return what == Completing::repositories || word.starts_with('=') || word.starts_with('<') ||
           word.starts_with('>') || word.starts_with('~') || word.contains(':');
}

} // namespace egraph
