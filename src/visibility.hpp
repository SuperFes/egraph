#pragma once

// Which versions of the repository index portage shows, evaluated from what the index holds:
// portdbapi's _visible (EAPI, SLOT, package.mask and package.unmask, keywords, licenses,
// properties, restrictions), and why a version is masked as getmaskingstatus words it.

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
    // Why it is masked, as getmaskingstatus words it ("package.mask", "~amd64 keyword",
    // "EULA license(s)", "EAPI 9"); empty for a visible version. getmaskingstatus reads a
    // PROPERTIES conditional under no USE, so a version masked by one alone has none.
    [[nodiscard]] std::vector<std::string> reasons(std::uint32_t version) const;

  private:
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
