#include "resume.hpp"

#include <nlohmann/json.hpp>

namespace egraph {

using Json = nlohmann::json;

std::string resume_entry(const Evaluated& original, const Plan& plan, std::string_view eroot,
                         const EmergeRequest& request, bool oneshot) {
    const auto& evaluated = plan.evaluated_or(original);
    auto mergelist = Json::array();
    for (const auto index : plan.order) {
        const auto& candidate = evaluated.candidates.at(plan.merges.at(index).candidate);
        mergelist.push_back({"ebuild", eroot, evaluated.string(candidate.cpv), "merge"});
    }
    // As emerge's option parser leaves them.
    auto options = Json::object();
    if (request.update) {
        options.emplace("--update", true);
    }
    if (request.deep) {
        options.emplace("--deep", true);
    }
    if (request.noreplace) {
        options.emplace("--noreplace", true);
    }
    if (request.rebuilds == UseRebuilds::all) {
        options.emplace("--newuse", true);
    } else if (request.rebuilds == UseRebuilds::changed) {
        options.emplace("--reinstall", "changed-use");
    }
    if (!request.dynamic_deps) {
        options.emplace("--dynamic-deps", "n");
    }
    if (oneshot) {
        options.emplace("--oneshot", true);
    }
    return Json{{"favorites", oneshot ? Json::array() : Json(request.targets)},
                {"mergelist", std::move(mergelist)},
                {"myopts", std::move(options)}}
        .dump(1);
}

} // namespace egraph
