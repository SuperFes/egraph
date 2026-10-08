#pragma once

#include "atom.hpp"
#include "evaluated.hpp"
#include "store.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// Where a step of a flag's state came from: a layer of USE_ORDER, or what portage applies over
// them all.
enum class UseLayer : std::uint8_t {
    env_d,
    repo,
    features,
    pkginternal,
    defaults,
    conf,
    pkg,
    env,
    force,
    arch,
    mask,
};

[[nodiscard]] std::string_view layer_name(UseLayer layer);

// One token that set a flag, or would have: a token naming the flag counts even when the flag
// already had that state (the entry then does nothing for it); "-*", "-prefix_*", "prefix_*"
// and a USE_EXPAND variable's replacement count only where they changed it.
struct UseStep {
    UseLayer layer = UseLayer::defaults;
    // Index into Evaluated::ledger_entries; none for the per-package layers and ARCH.
    std::optional<std::uint32_t> entry;
    // As the entry gives it ("-*", "flag", "video_cards_*"), or a USE_EXPAND variable's name
    // where its value replaced the flags of its prefix.
    std::string token;
    // The flag's state after the step.
    bool enabled = false;
    bool changed = false;
};

// A package's USE as portage stacks it (config.setcpv, then regenerate), from the USE ledger.
struct StackedUse {
    // PORTAGE_USE: the enabled flags within its IUSE, explicit or implicit, sorted.
    std::vector<std::string> use;
    // Of its explicit IUSE, the flags masked or forced, sorted.
    std::vector<std::string> forced;
    // Every flag a step touched, its steps in order.
    std::map<std::string, std::vector<UseStep>> steps;
};

// Whether flag is in the candidate's IUSE, explicit or implicit, as setcpv filters USE into
// PORTAGE_USE.
[[nodiscard]] bool in_iuse(const Store& installed, const Evaluated& evaluated,
                           const Candidate& candidate, std::string_view flag);

// The ledger's atoms parsed once, for stacking many packages.
class UseStacker {
  public:
    UseStacker(const Store& installed EGRAPH_LIFETIMEBOUND,
               const Evaluated& evaluated EGRAPH_LIFETIMEBOUND);

    // A token of a ledger entry: its position among the entry's tokens.
    struct Omitted {
        std::uint32_t entry = 0;
        std::uint32_t position = 0;
    };

    // Without, as if that token were not on its line.
    [[nodiscard]] StackedUse stack(const Candidate& candidate,
                                   std::optional<Omitted> without = std::nullopt) const;

    // The env files package.env applies to the candidate, in order: its lines matching it, as
    // portage applies them (*/* lines aside, which fold into make.conf).
    [[nodiscard]] std::vector<std::string> env_files(const Candidate& candidate) const;

  private:
    // The entries of a package.* source matching the candidate, as ordered_by_atom_specificity
    // applies them: its keys (one per atom, its lines in order) as portage's dictionaries hold
    // them for the cp, plain atoms first, then the extended ones by their cp; least specific
    // first.
    [[nodiscard]] std::vector<std::uint32_t> matching(const Candidate& candidate,
                                                      Range source) const;

    // Never null; the stores outlive the stacker.
    const Store* installed_;
    const Evaluated* evaluated_;
    // Per ledger entry, its atom read as configuration files read it; none for a global entry
    // or one portage would have refused.
    std::vector<std::optional<Atom>> atoms_;
    // The entries with a plain atom, by its cp, and those with an extended one; each ascending.
    std::map<std::string, std::vector<std::uint32_t>, std::less<>> by_cp_;
    std::vector<std::uint32_t> extended_;
};

} // namespace egraph
