#include "sets.hpp"

#include "atom.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace egraph {

using Json = nlohmann::json;

namespace {

bool strings(const Json& list) {
    return list.is_array() && std::ranges::all_of(list, &Json::is_string);
}

bool pairs(const Json& list) {
    return list.is_array() && std::ranges::all_of(list, [](const Json& pair) {
               return pair.is_array() && pair.size() == 2 && strings(pair);
           });
}

// The field's string, or empty where there is none.
std::expected<std::string, std::string> optional_string(const Json& entry, std::string_view field,
                                                        std::string_view name) {
    const auto found = entry.find(field);
    if (found == entry.end()) {
        return "";
    }
    if (!found->is_string()) {
        return std::unexpected(std::format("sets: {}'s {} is not a string", name, field));
    }
    return found->get<std::string>();
}

std::expected<PackageSet, std::string> parse_set(const Json& entry) {
    PackageSet set;
    set.name = entry.at("name").get<std::string>();
    auto kind = optional_string(entry, "class", set.name);
    auto description = optional_string(entry, "description", set.name);
    if (!kind || !description) {
        return std::unexpected(!kind ? kind.error() : description.error());
    }
    set.kind = std::move(*kind);
    set.description = std::move(*description);
    if (const auto found = entry.find("world_candidate"); found != entry.end()) {
        if (!found->is_boolean()) {
            return std::unexpected(
                std::format("sets: {}'s world_candidate is not true or false", set.name));
        }
        set.world_candidate = found->get<bool>();
    }
    if (const auto found = entry.find("atoms"); found != entry.end()) {
        if (!pairs(*found)) {
            return std::unexpected(
                std::format("sets: {}'s atoms are not (set, atom) pairs", set.name));
        }
        auto& atoms = set.atoms.emplace();
        for (const auto& pair : *found) {
            atoms.push_back(
                {.set = pair.at(0).get<std::string>(), .atom = pair.at(1).get<std::string>()});
        }
    }
    if (entry.contains("error")) {
        auto error = optional_string(entry, "error", set.name);
        if (!error) {
            return std::unexpected(error.error());
        }
        set.error = std::move(*error);
    }
    return set;
}

} // namespace

std::expected<SetList, std::string> parse_sets(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    const auto errors = json.find("errors");
    if (errors == json.end() || !strings(*errors)) {
        return std::unexpected("errors: not a list of strings");
    }
    const auto sets = json.find("sets");
    if (sets == json.end() || !sets->is_array() ||
        !std::ranges::all_of(*sets, [](const Json& entry) {
            return entry.is_object() && entry.contains("name") && entry.at("name").is_string();
        })) {
        return std::unexpected("sets: not a list of named sets");
    }
    SetList list{.sets = {}, .errors = errors->get<std::vector<std::string>>()};
    for (const auto& entry : *sets) {
        auto set = parse_set(entry);
        if (!set) {
            return std::unexpected(set.error());
        }
        list.sets.push_back(std::move(*set));
    }
    return list;
}

std::expected<Sets, std::string> loaded_sets(const SetList& list) {
    Sets sets;
    for (const auto& set : list.sets) {
        if (set.error) {
            return std::unexpected(std::format("@{}: {}", set.name, *set.error));
        }
        if (set.atoms) {
            sets.emplace(set.name,
                         GivenSet{.atoms = *set.atoms, .world_candidate = set.world_candidate});
        }
    }
    return sets;
}

std::vector<std::string> set_lines(const SetList& list) {
    std::vector<std::string> lines;
    lines.reserve(list.sets.size());
    for (const auto& set : list.sets) {
        lines.push_back(std::format("@{}\t{}\t{}\t{}", set.name, set.kind,
                                    set.world_candidate ? "world" : "", set.description));
    }
    return lines;
}

std::vector<std::string> set_atom_lines(const Store& store, std::span<const std::string> names,
                                        const Sets& sets) {
    std::vector<std::string> lines;
    for (const auto& name : names) {
        const auto found = sets.find(name);
        if (found == sets.end()) {
            continue;
        }
        for (const auto& [holder, text] : found->second.atoms) {
            std::string cpvs;
            if (const auto atom = parse_atom(text)) {
                for (const auto& pkg : store.packages) {
                    if (matches(store, pkg, *atom)) {
                        cpvs += std::format("{}{}", cpvs.empty() ? "" : " ", store.string(pkg.cpv));
                    }
                }
            }
            lines.push_back(std::format("@{}\t@{}\t{}\t{}", name, holder, text, cpvs));
        }
    }
    return lines;
}

} // namespace egraph
