#pragma once

// The GLSAs that affect the installed packages, as glsa-check -t affected finds them.

#include "repository.hpp"
#include "store.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace egraph {

struct AffectedAdvisory {
    struct Package {
        std::string cpv;
        // The GLSA's unaffected atoms for it: the versions that fix it.
        std::vector<std::string> fixed;
    };
    std::string id;
    std::string title;
    std::uint64_t revision = 0;
    // Sorted by cpv.
    std::vector<Package> packages;
};

// Whether an installed package is in a GLSA's range: a plain atom as vardb.match decides it, a
// revision range (>=~cat/pkg-1.2-r1, >~, <=~, <~) as the glsa module's revisionMatch does: the
// same version, any revision, and the revisions compared.
[[nodiscard]] bool in_advisory_range(const Store& store, const Package& pkg,
                                     std::string_view range);

// The advisories not applied whose ranges an installed package is in for the index's ARCH, as
// Glsa.isVulnerable: a package entry affects the system when an installed version is in one of
// its vulnerable ranges and none of its unaffected ones. Sorted by id.
[[nodiscard]] std::vector<AffectedAdvisory>
affected_advisories(const RepositoryIndex& index, const Store& installed,
                    std::span<const std::string> applied);

// The GLSAs glsa-check --inject marked applied, as the glsa module reads them with grabfile.
[[nodiscard]] std::vector<std::string> applied_advisories(const std::filesystem::path& eroot);

} // namespace egraph
