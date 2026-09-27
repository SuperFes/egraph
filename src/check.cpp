#include "check.hpp"

#include "json.hpp"

#include <map>
#include <string_view>

namespace egraph {

namespace {

std::map<std::string_view, std::string> by_cpv(const Store& store) {
    std::map<std::string_view, std::string> packages;
    for (const auto& pkg : store.packages) {
        packages.emplace(store.string(pkg.cpv), package_json(store, pkg));
    }
    return packages;
}

} // namespace

std::vector<std::string> drift(const Store& stored, const Store& fresh) {
    const auto old = by_cpv(stored);
    const auto now = by_cpv(fresh);
    std::vector<std::string> lines;
    for (const auto& [cpv, json] : now) {
        const auto found = old.find(cpv);
        if (found == old.end()) {
            lines.push_back("+" + std::string{cpv});
        } else if (found->second != json) {
            lines.push_back("~" + std::string{cpv});
        }
    }
    for (const auto& [cpv, json] : old) {
        if (!now.contains(cpv)) {
            lines.push_back("-" + std::string{cpv});
        }
    }
    return lines;
}

} // namespace egraph
