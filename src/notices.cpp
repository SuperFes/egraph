#include "notices.hpp"

#include "status.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iterator>
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

std::string joined(std::span<const std::string> words, std::string_view separator) {
    std::string text;
    for (const auto& word : words) {
        text += std::format("{}{}", text.empty() ? "" : separator, word);
    }
    return text;
}

// A file's findings, or all of them, by severity.
struct FindingCounts {
    std::string file{};
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t notes = 0;

    void add(FindingKind kind) {
        const auto severity = severity_name(kind);
        (severity == "error" ? errors : severity == "warning" ? warnings : notes) += 1;
    }

    // "1 error, 2 warnings", leaving out those with none.
    [[nodiscard]] std::string text() const {
        std::vector<std::string> parts;
        for (const auto& [n, one] :
             {std::pair{errors, "error"}, {warnings, "warning"}, {notes, "note"}}) {
            if (n != 0) {
                parts.push_back(std::format("{} {}{}", n, one, n == 1 ? "" : "s"));
            }
        }
        return joined(parts, ", ");
    }
};

// Each file with findings, in order.
std::vector<FindingCounts> counts_by_file(std::span<const Finding> findings) {
    std::vector<FindingCounts> files;
    for (const auto& finding : findings) {
        auto found = std::ranges::find(files, finding.file, &FindingCounts::file);
        if (found == files.end()) {
            files.push_back({.file = finding.file});
            found = std::prev(files.end());
        }
        found->add(finding.kind);
    }
    std::ranges::sort(files, {}, &FindingCounts::file);
    return files;
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
    if (news == json.end() || !all_have(*news, {"repo", "item", "title", "path"})) {
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
                                .title = entry.at("title").get<std::string>(),
                                .path = entry.at("path").get<std::string>()});
    }
    return notices;
}

std::vector<std::string> notice_lines(const Notices& notices) {
    std::vector<std::string> lines;
    lines.reserve(notices.config.size() + notices.news.size());
    for (const auto& [file, update] : notices.config) {
        lines.push_back(std::format("{}\tconfig\t{}", file, update));
    }
    for (const auto& [repo, item, title, path] : notices.news) {
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
    if (const auto& plan = notices.plan) {
        lines.push_back(std::format("title\tplan\t{}", plan->title));
        for (const auto& line : plan->detail) {
            lines.push_back(std::format("detail\tplan\t{}", line));
        }
    }
    for (const auto& counts : counts_by_file(notices.findings)) {
        lines.push_back(std::format("{}\tcheck\t{}\t{}\t{}", counts.file, counts.errors,
                                    counts.warnings, counts.notes));
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
    std::pair{NoticeKind::plan, std::string_view{"plan"}},
    std::pair{NoticeKind::check, std::string_view{"check"}},
};

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

std::string config_title(std::size_t files) {
    return std::format("{} configuration {} updates waiting", files,
                       files == 1 ? "file has" : "files have");
}

bool config_update_waiting(const std::filesystem::path& file) {
    // As find_updated_config_files: ._cfg????_<name>, less backups ending in ~ or .bak.
    const std::string tail = file.filename().string();
    std::string lower = tail;
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (tail.empty() || tail.ends_with('~') || lower.ends_with(".bak")) {
        return false;
    }
    constexpr std::string_view prefix = "._cfg0000_";
    const std::string suffix = "_" + tail;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{file.parent_path(), error}) {
        const std::string name = entry.path().filename().string();
        if (name.size() == prefix.size() + tail.size() && name.starts_with("._cfg") &&
            name.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

std::vector<Notice> notice_list(const Notices& notices, Seconds now) {
    std::vector<Notice> list;
    const auto add = [&](NoticeKind kind, std::string key, std::string title,
                         std::vector<std::string> detail, std::string fingerprint,
                         std::vector<std::string> packages = {}) {
        list.push_back({.kind = kind,
                        .key = std::move(key),
                        .title = std::move(title),
                        .detail = std::move(detail),
                        .packages = std::move(packages),
                        .fingerprint = std::move(fingerprint),
                        .since = now});
    };
    for (const auto& advisory : notices.advisories) {
        std::vector<std::string> detail;
        std::vector<std::string> packages;
        auto fingerprint = std::to_string(advisory.revision);
        for (const auto& [cpv, fixed] : advisory.packages) {
            detail.push_back(
                fixed.empty() ? cpv : std::format("{}, fixed in {}", cpv, joined(fixed, " or ")));
            packages.push_back(cpv);
            fingerprint += " " + cpv;
        }
        add(NoticeKind::glsa, "glsa:" + advisory.id,
            std::format("GLSA {}: {}", advisory.id, advisory.title), std::move(detail),
            std::move(fingerprint), std::move(packages));
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
            std::move(fingerprint), {cpv});
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
            joined(reasons, ", "), {cpv});
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
        auto title = config_title(files.size());
        add(NoticeKind::config, "config", std::move(title), std::move(files),
            std::move(fingerprint));
    }
    if (notices.plan) {
        list.push_back(*notices.plan);
    }
    if (!notices.findings.empty()) {
        FindingCounts all;
        // FNV-1a over the records: stable from one egraph to the next, as std::hash is not.
        std::uint64_t digest = 0xcbf29ce484222325U;
        for (const auto& finding : notices.findings) {
            all.add(finding.kind);
            for (const char c : finding_record(finding) + '\n') {
                digest = (digest ^ static_cast<unsigned char>(c)) * 0x100000001b3U;
            }
        }
        std::vector<std::string> detail;
        for (const auto& counts : counts_by_file(notices.findings)) {
            detail.push_back(std::format("{}: {}", counts.file, counts.text()));
        }
        add(NoticeKind::check, "check", std::format("Configuration check: {}", all.text()),
            std::move(detail), std::format("{:016x}", digest));
    }
    for (const auto& [repo, item, title, path] : notices.news) {
        add(NoticeKind::news, std::format("news:{}/{}", repo, item), title.empty() ? item : title,
            {}, item);
        list.back().file = path;
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

std::string set_aside_json(std::span<const SetAside> set_aside) {
    auto entries = Json::array();
    for (const auto& entry : set_aside) {
        Json object{{"key", entry.key}, {"fingerprint", entry.fingerprint}};
        if (entry.until) {
            object.emplace("until", entry.until->time_since_epoch().count());
        }
        entries.push_back(std::move(object));
    }
    const Json document{{"format", 1}, {"set_aside", std::move(entries)}};
    return document.dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
}

std::expected<std::vector<SetAside>, std::string> parse_set_aside(std::string_view text) {
    const auto document = Json::parse(text, nullptr, false);
    const auto invalid = std::unexpected(std::string{"not a set-aside file"});
    if (!document.is_object()) {
        return invalid;
    }
    const auto format = document.find("format");
    const auto entries = document.find("set_aside");
    if (format == document.end() || !format->is_number_unsigned() || entries == document.end() ||
        !entries->is_array()) {
        return invalid;
    }
    if (const auto version = format->get<std::uint64_t>(); version != 1) {
        return std::unexpected(std::format("format {}, from another egraph version", version));
    }
    if (!all_have(*entries, {"key", "fingerprint"})) {
        return invalid;
    }
    std::vector<SetAside> read;
    for (const auto& entry : *entries) {
        SetAside aside{.key = entry.at("key").get<std::string>(),
                       .fingerprint = entry.at("fingerprint").get<std::string>()};
        if (const auto until = entry.find("until"); until != entry.end()) {
            if (!until->is_number_integer()) {
                return invalid;
            }
            aside.until = Seconds{std::chrono::seconds{until->get<std::int64_t>()}};
        }
        read.push_back(std::move(aside));
    }
    return read;
}

std::optional<std::filesystem::path> set_aside_path(const std::optional<std::string>& state_home,
                                                    const std::optional<std::string>& home) {
    std::filesystem::path base;
    if (state_home && std::filesystem::path{*state_home}.is_absolute()) {
        base = *state_home;
    } else if (home && !home->empty()) {
        base = std::filesystem::path{*home} / ".local/state";
    } else {
        return std::nullopt;
    }
    return base / "egraph/set-aside.json";
}

bool is_set_aside(const Notice& notice, std::span<const SetAside> set_aside, Seconds now) {
    const auto found = std::ranges::find(set_aside, notice.key, &SetAside::key);
    return found != set_aside.end() && found->fingerprint == notice.fingerprint &&
           (!found->until || now < *found->until);
}

std::vector<Notice> shown_notices(std::span<const Notice> notices,
                                  std::span<const SetAside> set_aside, Seconds now) {
    std::vector<Notice> shown;
    std::ranges::copy_if(notices, std::back_inserter(shown), [&](const Notice& notice) {
        return !is_set_aside(notice, set_aside, now);
    });
    return shown;
}

void set_notice_aside(std::vector<SetAside>& set_aside, const Notice& notice,
                      std::optional<Seconds> until, std::span<const Notice> notices) {
    std::erase_if(set_aside, [&](const SetAside& entry) {
        return entry.key == notice.key || !std::ranges::contains(notices, entry.key, &Notice::key);
    });
    set_aside.push_back({.key = notice.key, .fingerprint = notice.fingerprint, .until = until});
}

std::expected<std::size_t, std::string> named_notice(std::span<const Notice> notices,
                                                     std::string_view name) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < notices.size(); ++i) {
        const std::string_view key = notices[i].key;
        if (key == name) {
            return i;
        }
        if (const auto colon = key.find(':');
            colon != std::string_view::npos && key.substr(colon + 1) == name) {
            found.push_back(i);
        }
    }
    if (found.empty()) {
        return std::unexpected(std::format("no notice is named {}", name));
    }
    if (found.size() > 1) {
        std::string keys;
        for (const auto i : found) {
            keys += std::format("{}{}", keys.empty() ? "" : ", ", notices[i].key);
        }
        return std::unexpected(std::format("{} names {} notices: {}", name, found.size(), keys));
    }
    return found.front();
}

void drop_notices(Notices& notices, std::span<const std::string> keys) {
    const auto dropped = [&keys](const std::string& key) {
        return std::ranges::contains(keys, key);
    };
    std::erase_if(notices.advisories,
                  [&](const AffectedAdvisory& advisory) { return dropped("glsa:" + advisory.id); });
    std::erase_if(notices.missing, [&](const Notices::Missing& missing) {
        return dropped("missing:" + missing.cpv);
    });
    if (dropped("preserved")) {
        if (notices.preserved) {
            notices.preserved->clear();
        }
        if (notices.rebuild) {
            notices.rebuild->clear();
        }
    }
    std::erase_if(notices.masked,
                  [&](const Notices::Masked& masked) { return dropped("masked:" + masked.cpv); });
    std::erase_if(notices.stale,
                  [&](const Notices::Stale& stale) { return dropped("stale:" + stale.name); });
    if (dropped("config")) {
        notices.config.clear();
    }
    if (dropped("plan")) {
        notices.plan.reset();
    }
    if (dropped("check")) {
        notices.findings.clear();
    }
    std::erase_if(notices.news, [&](const Notices::News& news) {
        return dropped(std::format("news:{}/{}", news.repo, news.item));
    });
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
