#pragma once

// Which versions of the repository index emerge can install, evaluated from what the index
// holds: portdbapi's _visible (EAPI, SLOT, package.mask and package.unmask, keywords, licenses,
// properties, restrictions) less what depgraph finds invalid, and why a version is masked as
// depgraph's get_masking_status words it.

#include "repository.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace egraph {

class VersionMasks {
  public:
    explicit VersionMasks(const RepositoryIndex& index EGRAPH_LIFETIMEBOUND);
    VersionMasks(const VersionMasks&) = delete;
    VersionMasks& operator=(const VersionMasks&) = delete;
    VersionMasks(VersionMasks&&) noexcept;
    VersionMasks& operator=(VersionMasks&&) noexcept;
    ~VersionMasks();

    // Of index.versions[version].
    [[nodiscard]] bool visible(std::uint32_t version) const;
    // As portdbapi's match-visible has it, invalid metadata aside.
    [[nodiscard]] bool portdb_visible(std::uint32_t version) const;
    // Why it is masked: getmaskingstatus's reasons ("package.mask", "~amd64 keyword", "EULA
    // license(s)", "EAPI 9") where portdb masks it, then "invalid: " and each thing depgraph
    // finds invalid, and "SLOT: undefined" for an empty SLOT; empty for a visible version.
    // getmaskingstatus reads a PROPERTIES conditional under no USE, so a version masked by one
    // alone has none.
    [[nodiscard]] std::vector<std::string> reasons(std::uint32_t version) const;

  private:
    [[nodiscard]] std::vector<std::string> masking_status(std::uint32_t version) const;

    struct Rules;
    std::unique_ptr<const Rules> rules_;
};

// Each version of the packages arguments name (atoms, or names without a category for every
// category's), every version without any: "cpv::repo<TAB>slot<TAB>visible", or
// "cpv::repo<TAB>slot<TAB>masked" and a field per reason; a slot with its sub-slot when that
// differs. By cp, then by version and repository. An argument that names nothing, or does not
// parse, is the error.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
version_lines(const RepositoryIndex& index, const VersionMasks& masking,
              std::span<const std::string> arguments);

} // namespace egraph
