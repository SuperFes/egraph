#include "notices.hpp"

#include "status.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <tuple>
#include <utility>

namespace egraph {

using Json = nlohmann::json;

namespace {

// Whether each of an array's entries is an object with the string fields given.
bool all_have(const Json& list, std::initializer_list<std::string_view> fields) {
    return list.is_array() && std::ranges::all_of(list, [&](const Json& entry) {
               return entry.is_object() && std::ranges::all_of(fields, [&](std::string_view field) {
                          const auto found = entry.find(field);
                          return found != entry.end() && found->is_string();
                      });
           });
}

bool strings(const Json& list) {
    return list.is_array() && std::ranges::all_of(list, &Json::is_string);
}

} // namespace

std::expected<Notices, std::string> parse_notices(std::string_view text) {
    const auto json = Json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        return std::unexpected("not a JSON object");
    }
    const auto config = json.find("config");
    if (config == json.end() || !all_have(*config, {"file", "update"})) {
        return std::unexpected("config: not a list of files and their updates");
    }
    const auto news = json.find("news");
    if (news == json.end() || !all_have(*news, {"repo", "item", "title"})) {
        return std::unexpected("news: not a list of news items");
    }
    const auto preserved = json.find("preserved");
    if (preserved == json.end() ||
        !(preserved->is_null() || (all_have(*preserved, {"path", "package"}) &&
                                   std::ranges::all_of(*preserved, [](const Json& entry) {
                                       return strings(entry.value("consumers", Json{}));
                                   })))) {
        return std::unexpected("preserved: not a list of libraries and what uses them");
    }
    const auto rebuild = json.find("rebuild");
    if (rebuild == json.end() || !(rebuild->is_null() || strings(*rebuild))) {
        return std::unexpected("rebuild: not a list of atoms");
    }
    Notices notices;
    if (!preserved->is_null()) {
        auto& found = notices.preserved.emplace();
        for (const auto& entry : *preserved) {
            found.push_back({.path = entry.at("path").get<std::string>(),
                             .package = entry.at("package").get<std::string>(),
                             .consumers = entry.at("consumers").get<std::vector<std::string>>()});
        }
    }
    if (!rebuild->is_null()) {
        notices.rebuild = rebuild->get<std::vector<std::string>>();
    }
    for (const auto& entry : *config) {
        notices.config.push_back({.file = entry.at("file").get<std::string>(),
                                  .update = entry.at("update").get<std::string>()});
    }
    for (const auto& entry : *news) {
        notices.news.push_back({.repo = entry.at("repo").get<std::string>(),
                                .item = entry.at("item").get<std::string>(),
                                .title = entry.at("title").get<std::string>()});
    }
    return notices;
}

std::vector<std::string> notice_lines(const Notices& notices) {
    std::vector<std::string> lines;
    lines.reserve(notices.config.size() + notices.news.size());
    for (const auto& [file, update] : notices.config) {
        lines.push_back(std::format("{}\tconfig\t{}", file, update));
    }
    for (const auto& [repo, item, title] : notices.news) {
        lines.push_back(std::format("{}\tnews\t{}\t{}", item, repo, title));
    }
    for (const auto& [path, package, consumers] :
         notices.preserved.value_or(std::vector<Notices::Preserved>{})) {
        std::string joined;
        for (const auto& consumer : consumers) {
            joined += std::format("{}{}", joined.empty() ? "" : " ", consumer);
        }
        lines.push_back(std::format("{}\tpreserved\t{}\t{}", path, package, joined));
    }
    for (const auto& atom : notices.rebuild.value_or(std::vector<std::string>{})) {
        lines.push_back(std::format("{}\trebuild", atom));
    }
    for (const auto& advisory : notices.advisories) {
        for (const auto& [cpv, fixed] : advisory.packages) {
            std::string joined;
            for (const auto& atom : fixed) {
                joined += std::format("{}{}", joined.empty() ? "" : " ", atom);
            }
            lines.push_back(
                std::format("{}\tglsa\t{}\t{}\t{}", advisory.id, advisory.title, cpv, joined));
        }
    }
    for (const auto& [name, synced] : notices.stale) {
        lines.push_back(std::format("{}\tstale\t{}", name, synced.time_since_epoch().count()));
    }
    for (const auto& [cpv, reasons] : notices.masked) {
        std::string joined;
        for (const auto& reason : reasons) {
            joined += std::format("{}{}", joined.empty() ? "" : ", ", reason);
        }
        lines.push_back(std::format("{}\tmasked\t{}", cpv, joined));
    }
    for (const auto& [cpv, category, soname] : notices.missing) {
        lines.push_back(std::format("{}\tmissing\t{}\t{}", cpv, category, soname));
    }
    return lines;
}

std::vector<Notices::Stale> stale_repositories(const RepositoryIndex& index, Seconds now,
                                               int days) {
    std::vector<Notices::Stale> found;
    if (days <= 0) {
        return found;
    }
    const auto oldest = now - std::chrono::days{days};
    for (const auto& repository : index.repositories) {
        const auto synced = repository_synced(std::string{index.string(repository.location)});
        if (synced && *synced < oldest) {
            found.push_back(
                {.name = std::string{index.string(repository.name)}, .synced = *synced});
        }
    }
    return found;
}

std::vector<Notices::Masked> masked_installed(const Store& store, const Evaluated& evaluated,
                                              bool dynamic_deps) {
    std::vector<Notices::Masked> found;
    for (std::size_t id = 0; id < store.packages.size(); ++id) {
        const auto& record = evaluated.packages.at(id);
        if (!(dynamic_deps ? record.masked : record.vdb_masked)) {
            continue;
        }
        Notices::Masked masked{.cpv = std::string{store.string(store.packages.at(id).cpv)},
                               .reasons = {}};
        for (const auto reason :
             evaluated.ids_in(dynamic_deps ? record.mask_reasons : record.vdb_mask_reasons)) {
            masked.reasons.emplace_back(evaluated.string(reason));
        }
        found.push_back(std::move(masked));
    }
    std::ranges::sort(found, {}, &Notices::Masked::cpv);
    return found;
}

std::vector<Notices::Missing> missing_sonames(const Store& store) {
    const auto key = [](const Notices::Missing& missing) {
        return std::tie(missing.cpv, missing.category, missing.soname);
    };
    std::vector<Notices::Missing> found;
    for (const auto& pkg : store.packages) {
        for (const auto& required : store.required_in(pkg.required)) {
            if (required.providers.count == 0) {
                found.push_back({.cpv = std::string{store.string(pkg.cpv)},
                                 .category = std::string{store.string(required.category)},
                                 .soname = std::string{store.string(required.soname)}});
            }
        }
    }
    std::ranges::sort(found, {}, key);
    const auto [first, last] = std::ranges::unique(found, {}, key);
    found.erase(first, last);
    return found;
}

namespace {

constexpr std::array notice_kinds{
    std::pair{NoticeKind::glsa, std::string_view{"glsa"}},
    std::pair{NoticeKind::news, std::string_view{"news"}},
    std::pair{NoticeKind::config, std::string_view{"config"}},
    std::pair{NoticeKind::preserved, std::string_view{"preserved"}},
    std::pair{NoticeKind::stale, std::string_view{"stale"}},
    std::pair{NoticeKind::masked, std::string_view{"masked"}},
    std::pair{NoticeKind::missing, std::string_view{"missing"}},
};

std::string joined(std::span<const std::string> words, std::string_view separator) {
    std::string text;
    for (const auto& word : words) {
        text += std::format("{}{}", text.empty() ? "" : separator, word);
    }
    return text;
}

} // namespace

std::string_view notice_kind_name(NoticeKind kind) {
    for (const auto& [known, name] : notice_kinds) {
        if (known == kind) {
            return name;
        }
    }
    return {};
}

std::optional<NoticeKind> notice_kind(std::string_view name) {
    for (const auto& [kind, known] : notice_kinds) {
        if (known == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::vector<Notice> notice_list(const Notices& notices, Seconds now) {
    std::vector<Notice> list;
    const auto add = [&](NoticeKind kind, std::string key, std::string title,
                         std::vector<std::string> detail, std::string fingerprint) {
        list.push_back({.kind = kind,
                        .key = std::move(key),
                        .title = std::move(title),
                        .detail = std::move(detail),
                        .fingerprint = std::move(fingerprint),
                        .since = now});
    };
    for (const auto& advisory : notices.advisories) {
        std::vector<std::string> detail;
        auto fingerprint = std::to_string(advisory.revision);
        for (const auto& [cpv, fixed] : advisory.packages) {
            detail.push_back(
                fixed.empty() ? cpv : std::format("{}, fixed in {}", cpv, joined(fixed, " or ")));
            fingerprint += " " + cpv;
        }
        add(NoticeKind::glsa, "glsa:" + advisory.id,
            std::format("GLSA {}: {}", advisory.id, advisory.title), std::move(detail),
            std::move(fingerprint));
    }
    for (std::size_t i = 0; i < notices.missing.size();) {
        const auto cpv = notices.missing.at(i).cpv;
        std::vector<std::string> detail;
        for (; i < notices.missing.size() && notices.missing.at(i).cpv == cpv; ++i) {
            const auto& missing = notices.missing.at(i);
            detail.push_back(std::format("{} ({})", missing.soname, missing.category));
        }
        auto fingerprint = joined(detail, " ");
        add(NoticeKind::missing, "missing:" + cpv,
            std::format("{} needs libraries nothing installed provides", cpv), std::move(detail),
            std::move(fingerprint));
    }
    if (const auto& preserved = notices.preserved; preserved && !preserved->empty()) {
        std::vector<std::string> detail;
        std::string fingerprint;
        for (const auto& [path, package, consumers] : *preserved) {
            detail.push_back(consumers.empty()
                                 ? path
                                 : std::format("{}, used by {}", path, joined(consumers, ", ")));
            fingerprint += std::format("{}{}", fingerprint.empty() ? "" : " ", path);
        }
        add(NoticeKind::preserved, "preserved",
            std::format("{} preserved {}", preserved->size(),
                        preserved->size() == 1 ? "library" : "libraries"),
            std::move(detail), std::move(fingerprint));
    }
    for (const auto& [cpv, reasons] : notices.masked) {
        add(NoticeKind::masked, "masked:" + cpv, std::format("{} is masked", cpv), reasons,
            joined(reasons, ", "));
    }
    for (const auto& [name, synced] : notices.stale) {
        add(NoticeKind::stale, "stale:" + name,
            std::format("{} synced {}", name, age_text(synced, now)), {},
            std::to_string(synced.time_since_epoch().count()));
    }
    if (!notices.config.empty()) {
        std::vector<std::string> files;
        std::string fingerprint;
        for (const auto& [file, update] : notices.config) {
            if (!std::ranges::contains(files, file)) {
                files.push_back(file);
            }
            fingerprint += std::format("{}{}", fingerprint.empty() ? "" : " ", update);
        }
        auto title = std::format("{} configuration {} updates waiting", files.size(),
                                 files.size() == 1 ? "file has" : "files have");
        add(NoticeKind::config, "config", std::move(title), std::move(files),
            std::move(fingerprint));
    }
    for (const auto& [repo, item, title] : notices.news) {
        add(NoticeKind::news, std::format("news:{}/{}", repo, item), title.empty() ? item : title,
            {}, item);
    }
    return list;
}

void carry_since(std::vector<Notice>& notices, std::span<const Notice> previous) {
    for (auto& notice : notices) {
        if (const auto found = std::ranges::find(previous, notice.key, &Notice::key);
            found != previous.end()) {
            notice.since = found->since;
        }
    }
}

std::vector<Notice> new_notices(std::span<const Notice> notices, std::span<const Notice> previous) {
    std::vector<Notice> added;
    for (const auto& notice : notices) {
        if (!std::ranges::contains(previous, notice.key, &Notice::key)) {
            added.push_back(notice);
        }
    }
    return added;
}

void drop_preserved(Notices& notices) {
    if (!notices.preserved) {
        return;
    }
    std::erase_if(notices.missing, [&](const Notices::Missing& missing) {
        return std::ranges::any_of(*notices.preserved, [&](const Notices::Preserved& library) {
            return std::filesystem::path{library.path}.filename() == missing.soname;
        });
    });
}

} // namespace egraph
