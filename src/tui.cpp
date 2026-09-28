#include "tui.hpp"

#include "build_info.hpp"

#include <cmath>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <tuple>

namespace egraph::tui {

namespace {

// Every task of every emerge, in order.
std::vector<std::reference_wrapper<const emerge::Task>> tasks(const Watched& watched) {
    std::vector<std::reference_wrapper<const emerge::Task>> all;
    for (const auto& snapshot : watched.snapshots) {
        all.insert(all.end(), snapshot.tasks.begin(), snapshot.tasks.end());
    }
    return all;
}

// count code points of text, after the first skip.
std::string code_points(std::string_view text, std::size_t skip, std::size_t count) {
    return clip(text, skip + count).substr(clip(text, skip).size());
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> found;
    for (const auto line : std::views::split(text, '\n')) {
        found.emplace_back(std::string_view{line});
    }
    return found;
}

std::string folded(std::string_view text) {
    std::string out{text};
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void append_utf8(std::string& out, char32_t code) {
    const auto byte = [](std::uint32_t value) { return static_cast<char>(value); };
    const auto c = static_cast<std::uint32_t>(code);
    if (c < 0x80U) {
        out += byte(c);
    } else if (c < 0x800U) {
        out += byte(0xC0U | (c >> 6U));
        out += byte(0x80U | (c & 0x3FU));
    } else if (c < 0x10000U) {
        out += byte(0xE0U | (c >> 12U));
        out += byte(0x80U | ((c >> 6U) & 0x3FU));
        out += byte(0x80U | (c & 0x3FU));
    } else {
        out += byte(0xF0U | (c >> 18U));
        out += byte(0x80U | ((c >> 12U) & 0x3FU));
        out += byte(0x80U | ((c >> 6U) & 0x3FU));
        out += byte(0x80U | (c & 0x3FU));
    }
}

void pop_code_point(std::string& text) {
    while (!text.empty()) {
        const auto last = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((last & 0xC0U) != 0x80U) {
            return;
        }
    }
}

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

// Index into kind_shorthands for an index into dep_kinds.
std::size_t shorthand_of(std::uint32_t kind) {
    const auto name = dep_kinds.at(kind);
    const auto found = std::ranges::find(kind_shorthands, name, &KindShorthand::name);
    return static_cast<std::size_t>(found - kind_shorthands.begin());
}

bool is(const Key& key, char32_t code) {
    return key.kind == KeyKind::character && key.code == code;
}

void keep_visible(Cursor& cursor, std::size_t height) {
    if (cursor.at < cursor.top) {
        cursor.top = cursor.at;
    } else if (cursor.at >= cursor.top + height) {
        cursor.top = cursor.at - height + 1;
    }
}

// Moves over a plain list of count entries.
void move(Cursor& cursor, std::size_t count, const Key& key, std::size_t height) {
    if (count == 0) {
        cursor = {};
        return;
    }
    auto at = cursor.at;
    if (key.kind == KeyKind::up || is(key, U'k')) {
        at = at == 0 ? 0 : at - 1;
    } else if (key.kind == KeyKind::down || is(key, U'j')) {
        at = at + 1;
    } else if (key.kind == KeyKind::page_up) {
        at = at > height ? at - height : 0;
    } else if (key.kind == KeyKind::page_down) {
        at = at + height;
    } else if (key.kind == KeyKind::home || is(key, U'g')) {
        at = 0;
    } else if (key.kind == KeyKind::end || is(key, U'G')) {
        at = count - 1;
    }
    cursor.at = std::min(at, count - 1);
    keep_visible(cursor, height);
}

bool is_move(const Key& key) {
    switch (key.kind) {
    case KeyKind::up:
    case KeyKind::down:
    case KeyKind::page_up:
    case KeyKind::page_down:
    case KeyKind::home:
    case KeyKind::end:
        return true;
    default:
        return is(key, U'j') || is(key, U'k') || is(key, U'g') || is(key, U'G');
    }
}

bool selectable(const Row& row) {
    return row.type == RowType::link || row.type == RowType::path;
}

// Moves over a page, landing only on packages; headings above the first stay in view.
void move_on_page(App::Page& page, const Key& key, std::size_t height) {
    std::vector<std::size_t> stops;
    for (std::size_t i = 0; i < page.rows.size(); ++i) {
        if (selectable(page.rows.at(i))) {
            stops.push_back(i);
        }
    }
    if (stops.empty()) {
        return;
    }
    const auto current = std::ranges::lower_bound(stops, page.cursor.at);
    Cursor among{.at = static_cast<std::size_t>(current - stops.begin()), .top = 0};
    among.at = std::min(among.at, stops.size() - 1);
    move(among, stops.size(), key, height);
    page.cursor.at = stops.at(among.at);
    keep_visible(page.cursor, height);
    if (among.at == 0) {
        page.cursor.top = 0;
    }
}

Row text_row(RowType type, std::string text) {
    Row row;
    row.type = type;
    row.text = std::move(text);
    return row;
}

Row link_row(const Link& link, std::size_t depth, bool reverse) {
    Row row;
    row.type = RowType::link;
    row.link = link;
    row.depth = depth;
    row.reverse = reverse;
    return row;
}

// Why depclean keeps package: the root, then each package down the chain to it.
std::vector<Row> kept_rows(const Store& store, const Kept& kept, std::uint32_t package,
                           bool build_deps) {
    std::vector<Row> rows{
        text_row(RowType::heading, build_deps ? "Kept by" : "Kept by, at run time")};
    const auto path = why(kept, package);
    if (!path) {
        rows.push_back(text_row(RowType::alert, store.roots.empty()
                                                    ? "nothing: @world is empty, which depclean "
                                                      "refuses to work with"
                                                    : "nothing: depclean would remove it"));
    } else {
        const auto& root = store.roots.at(path->root.root);
        auto root_row = text_row(RowType::root, std::format("@{}", store.string(root.set)));
        root_row.link.atom = root.atom;
        rows.push_back(std::move(root_row));
        auto first = link_row({.package = path->root.child, .atom = root.atom}, 1, false);
        first.type = RowType::path;
        rows.push_back(std::move(first));
        for (const auto& edge : path->edges) {
            Link link{.package = edge.child, .atom = edge.atom, .choice = edge.choice};
            if (const auto index = shorthand_of(edge.kind); index < kind_shorthands.size()) {
                link.kinds.at(index) = true;
            }
            auto row = link_row(link, rows.back().depth + 1, false);
            row.type = RowType::path;
            rows.push_back(std::move(row));
        }
    }
    rows.push_back(text_row(RowType::note, ""));
    return rows;
}

// What package needs that nothing installed satisfies, then the build-time dependencies since
// replaced; one row per dependency with its kinds and what is installed in its place.
std::vector<Row> unsatisfied_rows(const Store& store, std::uint32_t package, bool build_deps) {
    std::vector<Row> missing;
    for (const auto& dependency : unsatisfied(store, package)) {
        if (!build_deps && is_build_kind(dependency.kind)) {
            continue;
        }
        auto instead = installed_instead(store, dependency);
        const auto type = replaced(dependency, instead) ? RowType::replaced : RowType::missing;
        auto text = render(store, dependency);
        auto row = std::ranges::find_if(
            missing, [&](const Row& other) { return other.type == type && other.text == text; });
        if (row == missing.end()) {
            missing.push_back(text_row(type, std::move(text)));
            row = missing.end() - 1;
            row->instead = std::move(instead);
        }
        if (const auto index = shorthand_of(dependency.kind); index < kind_shorthands.size()) {
            row->link.kinds.at(index) = true;
        }
    }
    std::vector<Row> rows;
    for (const auto type : {RowType::missing, RowType::replaced}) {
        const auto count = std::ranges::count(missing, type, &Row::type);
        if (count == 0) {
            continue;
        }
        rows.push_back(text_row(
            RowType::heading,
            std::format("{}  {}",
                        type == RowType::missing ? "Not installed" : "Built with, since replaced",
                        count)));
        std::ranges::copy_if(missing, std::back_inserter(rows),
                             [&](const Row& row) { return row.type == type; });
        rows.push_back(text_row(RowType::note, ""));
    }
    return rows;
}

std::vector<Row> page_rows(const Store& store, const Graph& graph, std::uint32_t package) {
    std::vector<Row> rows;
    for (const bool reverse : {false, true}) {
        const auto found = links(store, graph, package, reverse);
        if (reverse) {
            rows.push_back(text_row(RowType::note, ""));
        }
        rows.push_back(
            text_row(RowType::heading,
                     std::format("{}  {}", reverse ? "Needed by" : "Depends on", found.size())));
        if (found.empty()) {
            rows.push_back(text_row(RowType::note, "nothing"));
        }
        for (const auto& link : found) {
            rows.push_back(link_row(link, 0, reverse));
        }
    }
    return rows;
}

// The row of the link a nested row hangs from.
std::optional<std::size_t> parent_of(const std::vector<Row>& rows, std::size_t index) {
    const auto depth = rows.at(index).depth;
    if (rows.at(index).type != RowType::link || depth == 0) {
        return std::nullopt;
    }
    for (std::size_t i = index; i-- > 0;) {
        if (rows.at(i).type == RowType::link && rows.at(i).depth == depth - 1) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

bool available() {
    return EGRAPH_HAVE_TUI != 0;
}

void thread(std::vector<Row>& rows) {
    std::vector<bool> later;
    for (auto& row : std::ranges::reverse_view(rows)) {
        const auto depth = selectable(row) ? row.depth : 0;
        later.resize(std::max(later.size(), depth + 1), false);
        row.last = !later.at(depth);
        row.rails.assign(depth > 1 ? depth - 1 : 0, false);
        for (std::size_t level = 1; level < depth; ++level) {
            row.rails.at(level - 1) = later.at(level);
        }
        later.at(depth) = true;
        std::fill(later.begin() + static_cast<std::ptrdiff_t>(depth) + 1, later.end(), false);
    }
}

std::vector<Link> links(const Store& store, const Graph& graph, std::uint32_t package,
                        bool reverse) {
    std::vector<Link> found;
    for (const auto& edge : reverse ? graph.rdeps(package) : graph.deps(package)) {
        const auto other = reverse ? edge.parent : edge.child;
        auto link = std::ranges::find_if(found, [&](const Link& candidate) {
            return candidate.package == other && candidate.atom == edge.atom &&
                   candidate.choice == edge.choice;
        });
        if (link == found.end()) {
            found.push_back({.package = other, .atom = edge.atom, .choice = edge.choice});
            link = found.end() - 1;
        }
        if (const auto index = shorthand_of(edge.kind); index < kind_shorthands.size()) {
            link->kinds.at(index) = true;
        }
    }
    std::ranges::sort(found, [&](const Link& a, const Link& b) {
        return std::tuple{store.string(store.packages.at(a.package).cpv), store.string(a.atom),
                          a.choice} < std::tuple{store.string(store.packages.at(b.package).cpv),
                                                 store.string(b.atom), b.choice};
    });
    return found;
}

App::App(const Store& store, const Graph& graph, Update update)
    : store_(store), graph_(graph), update_(update) {
    index();
    recompute();
    filter();
}

void App::index() {
    const auto& store = this->store();
    const auto& graph = graph_.get();
    const auto count = store.packages.size();
    folded_.clear();
    folded_.reserve(count);
    dependencies_.assign(count, 0);
    dependents_.assign(count, 0);
    broken_.clear();
    broken_.reserve(count);
    broken_at_run_time_.clear();
    broken_at_run_time_.reserve(count);
    for (std::uint32_t id = 0; id < count; ++id) {
        folded_.push_back(folded(store.string(store.packages.at(id).cpv)));
        std::vector<std::uint32_t> children;
        for (const auto& edge : graph.deps(id)) {
            children.push_back(edge.child);
        }
        std::ranges::sort(children);
        const auto [first, last] = std::ranges::unique(children);
        children.erase(first, last);
        dependencies_.at(id) = children.size();
        std::size_t broken = 0;
        std::size_t at_run_time = 0;
        for (const auto& dependency : unsatisfied(store, id)) {
            if (!is_build_kind(dependency.kind)) {
                ++at_run_time;
                ++broken;
            } else if (!replaced(dependency, installed_instead(store, dependency))) {
                ++broken;
            }
        }
        broken_.push_back(broken);
        broken_at_run_time_.push_back(at_run_time);
        for (const auto child : children) {
            ++dependents_.at(child);
        }
    }
}

void App::adopt(Store store, Source source) {
    auto loaded = std::make_unique<const Loaded>(std::move(store));
    store_ = loaded->store;
    graph_ = loaded->graph;
    owned_ = std::move(loaded);
    source_ = source;
    pages_.clear();
    index();
    recompute();
    filter();
}

void App::recompute() {
    kept_ = keep(store(), {.build_deps = build_deps_});
    root_of_.assign(store().packages.size(), std::nullopt);
    for (const auto& pull : kept_.roots) {
        auto& root = root_of_.at(pull.child);
        if (!root || pull.root < *root) {
            root = pull.root;
        }
    }
}

void App::filter() {
    const auto query = folded(list_.query);
    list_.shown.clear();
    for (std::uint32_t id = 0; id < folded_.size(); ++id) {
        if ((list_.only == Only::orphans && kept_.packages.at(id)) ||
            (list_.only == Only::broken && broken(id) == 0)) {
            continue;
        }
        if (folded_.at(id).find(query) != std::string::npos) {
            list_.shown.push_back(id);
        }
    }
    list_.cursor = {};
}

void App::open(std::uint32_t package) {
    Page page{
        .package = package, .rows = kept_rows(store(), kept_, package, build_deps_), .cursor = {}};
    std::ranges::move(unsatisfied_rows(store(), package, build_deps_),
                      std::back_inserter(page.rows));
    std::ranges::move(page_rows(store(), graph_.get(), package), std::back_inserter(page.rows));
    thread(page.rows);
    // On the first dependency, else on whatever can be selected.
    const auto first = std::ranges::find(page.rows, RowType::link, &Row::type);
    const auto any = std::ranges::find_if(page.rows, selectable);
    page.cursor.at = static_cast<std::size_t>((first != page.rows.end() ? first
                                               : any != page.rows.end() ? any
                                                                        : page.rows.begin()) -
                                              page.rows.begin());
    keep_visible(page.cursor, height_);
    if (page.cursor.at < height_) {
        page.cursor.top = 0;
    }
    pages_.push_back(std::move(page));
}

void App::handle(const Key& key) {
    if (key.kind == KeyKind::closed) {
        done_ = true;
    } else if (key.kind == KeyKind::tick) {
        if (watched_ && pages_.empty()) {
            watched_->due = true;
        }
    } else if (dialog_) {
        dialog_.reset();
    } else if (!pages_.empty()) {
        handle_page(key);
        // Back at the emerge view, whatever it shows is stale.
        if (pages_.empty() && watched_) {
            watched_->due = true;
        }
    } else if (checked_) {
        handle_check(key);
    } else if (watched_) {
        handle_watch(key);
    } else {
        handle_list(key);
    }
}

void App::finish_check(CheckResult result) {
    if (!checked_) {
        return;
    }
    checked_->stage = Checked::Stage::checked;
    checked_->cursor = {};
    if (result) {
        checked_->drift = std::move(result->drift);
        if (update_ == Update::preview && !checked_->drift.empty()) {
            checked_->fresh = std::move(result->store);
        }
    } else {
        checked_.reset();
        show({.error = true, .title = "The check could not run", .lines = lines(result.error())});
    }
}

void App::finish_rebuild(std::expected<Store, std::string> result) {
    if (!checked_) {
        return;
    }
    if (!result) {
        // The drift still stands, and u can try again.
        checked_->stage = Checked::Stage::checked;
        show({.error = true,
              .title = "The rebuild failed; the store is as it was",
              .lines = lines(result.error())});
        return;
    }
    checked_->stage = Checked::Stage::rebuilt;
    checked_->cursor = {};
    checked_->drift.clear();
    adopt(std::move(*result), Source::saved);
}

bool App::watch_requested() const {
    return watched_ && watched_->due && pages_.empty();
}

void App::finish_watch(std::vector<emerge::Snapshot> snapshots, const pressure::Sample& sample) {
    if (!watched_) {
        return;
    }
    auto& watched = *watched_;
    watched.snapshots = std::move(snapshots);
    if (sample.cpu || sample.mem_available || sample.load) {
        watched.history.add(sample);
    }
    watched.due = false;
    ++watched.frame;
    const auto count = tasks(watched).size();
    watched.cursor.at = std::min(watched.cursor.at, count == 0 ? 0 : count - 1);
}

std::optional<std::chrono::milliseconds> App::refresh() const {
    if (watched_ && pages_.empty()) {
        return watch_interval;
    }
    return std::nullopt;
}

void App::handle_watch(const Key& key) {
    if (!watched_) {
        return;
    }
    auto& watched = *watched_;
    const auto all = tasks(watched);
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        watched_.reset();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (watched.cursor.at >= all.size()) {
            return;
        }
        // What is building is not installed yet; its page is the installed version's.
        const auto parts = split_cpv(all.at(watched.cursor.at).get().cpv);
        const auto cp = std::format("{}/{}", parts.category, parts.name);
        const auto installed = resolve(store(), cp);
        if (installed && !installed->empty()) {
            open(installed->back());
        } else {
            show({.error = false,
                  .title = std::format("No version of {} is installed yet", cp),
                  .lines = {"Pages show installed packages, and it has none so far."}});
        }
    } else if (is_move(key)) {
        move(watched.cursor, all.size(), key, height_);
    }
}

std::optional<std::uint32_t> App::find(std::string_view cpv) const {
    for (std::uint32_t id = 0; id < store().packages.size(); ++id) {
        if (store().string(store().packages.at(id).cpv) == cpv) {
            return id;
        }
    }
    return std::nullopt;
}

void App::handle_check(const Key& key) {
    if (!checked_ || checked_->stage == Checked::Stage::checking ||
        checked_->stage == Checked::Stage::rebuilding) {
        return;
    }
    auto& checked = *checked_;
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace ||
               key.kind == KeyKind::left || is(key, U'h')) {
        checked_.reset();
    } else if (is(key, U'r')) {
        checked = {};
    } else if (is(key, U'u')) {
        if (checked.stage != Checked::Stage::checked || checked.drift.empty()) {
            return;
        }
        if (update_ == Update::save) {
            checked.stage = Checked::Stage::rebuilding;
        } else if (checked.fresh) {
            auto fresh = std::move(*checked.fresh);
            checked.fresh.reset();
            checked.drift.clear();
            checked.stage = Checked::Stage::rebuilt;
            checked.cursor = {};
            adopt(std::move(fresh), Source::preview);
        }
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (checked.cursor.at < checked.drift.size()) {
            const auto cpv = std::string_view{checked.drift.at(checked.cursor.at)}.substr(1);
            if (const auto id = find(cpv)) {
                open(*id);
            } else {
                show({.error = false,
                      .title = std::format("{} is not in the store", cpv),
                      .lines = {"It was installed after the store was built.",
                                update_ == Update::save
                                    ? "u rebuilds the store to include it."
                                    : "u shows the fresh build, which has it."}});
            }
        }
    } else if (is_move(key)) {
        move(checked.cursor, checked.drift.size(), key, height_);
    }
}

void App::handle_list(const Key& key) {
    if (list_.searching) {
        if (key.kind == KeyKind::character && key.code >= U' ') {
            append_utf8(list_.query, key.code);
            filter();
        } else if (key.kind == KeyKind::backspace) {
            pop_code_point(list_.query);
            filter();
        } else if (key.kind == KeyKind::enter) {
            list_.searching = false;
        } else if (key.kind == KeyKind::escape) {
            list_.searching = false;
            list_.query.clear();
            filter();
        } else if (is_move(key) && key.kind != KeyKind::character) {
            move(list_.cursor, list_.shown.size(), key, height_);
        }
        return;
    }
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (is(key, U'/')) {
        list_.searching = true;
    } else if (is(key, U'o')) {
        list_.only = list_.only == Only::orphans ? Only::all : Only::orphans;
        filter();
    } else if (is(key, U'!')) {
        list_.only = list_.only == Only::broken ? Only::all : Only::broken;
        filter();
    } else if (is(key, U'c')) {
        checked_.emplace();
    } else if (is(key, U'e')) {
        watched_.emplace();
    } else if (is(key, U'b')) {
        build_deps_ = !build_deps_;
        recompute();
        filter();
    } else if (key.kind == KeyKind::escape && !list_.query.empty()) {
        list_.query.clear();
        filter();
    } else if (key.kind == KeyKind::enter || key.kind == KeyKind::right || is(key, U'l')) {
        if (list_.cursor.at < list_.shown.size()) {
            open(list_.shown.at(list_.cursor.at));
        }
    } else if (is_move(key)) {
        move(list_.cursor, list_.shown.size(), key, height_);
    }
}

bool App::can_unfold(const Row& row) const {
    if (row.type != RowType::link || row.cycle) {
        return false;
    }
    return (row.reverse ? dependents(row.link.package) : dependencies(row.link.package)) > 0;
}

void App::unfold(Page& page) {
    const auto at = page.cursor.at;
    const auto& row = page.rows.at(at);
    if (row.unfolded || !can_unfold(row)) {
        return;
    }
    std::vector<std::uint32_t> path{page.package, row.link.package};
    for (auto up = parent_of(page.rows, at); up; up = parent_of(page.rows, *up)) {
        path.push_back(page.rows.at(*up).link.package);
    }
    const auto depth = row.depth + 1;
    const auto reverse = row.reverse;
    std::vector<Row> children;
    for (const auto& link : links(store(), graph_.get(), row.link.package, reverse)) {
        children.push_back(link_row(link, depth, reverse));
        children.back().cycle = std::ranges::contains(path, link.package);
    }
    page.rows.at(at).unfolded = true;
    const auto count = children.size();
    page.rows.insert(page.rows.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                     std::make_move_iterator(children.begin()),
                     std::make_move_iterator(children.end()));
    thread(page.rows);
    // Bring the unfolded rows into view, as far as the cursor stays on screen.
    if (at + count >= page.cursor.top + height_) {
        page.cursor.top = std::min(at, at + count + 1 - height_);
    }
}

void App::fold(Page& page) {
    const auto at = page.cursor.at;
    const auto depth = page.rows.at(at).depth;
    auto end = at + 1;
    while (end < page.rows.size() && page.rows.at(end).type == RowType::link &&
           page.rows.at(end).depth > depth) {
        ++end;
    }
    page.rows.erase(page.rows.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                    page.rows.begin() + static_cast<std::ptrdiff_t>(end));
    page.rows.at(at).unfolded = false;
    thread(page.rows);
}

void App::handle_page(const Key& key) {
    auto& page = pages_.back();
    const bool on_link =
        page.cursor.at < page.rows.size() && selectable(page.rows.at(page.cursor.at));
    if (is(key, U'q') || is(key, U'Q')) {
        done_ = true;
    } else if (key.kind == KeyKind::escape || key.kind == KeyKind::backspace) {
        pages_.pop_back();
    } else if (!on_link) {
        if (key.kind == KeyKind::left || is(key, U'h')) {
            pages_.pop_back();
        }
    } else if (key.kind == KeyKind::enter) {
        if (const auto target = page.rows.at(page.cursor.at).link.package; target != page.package) {
            open(target);
        }
    } else if (is(key, U' ') || key.kind == KeyKind::tab) {
        if (page.rows.at(page.cursor.at).unfolded) {
            fold(page);
        } else {
            unfold(page);
        }
    } else if (key.kind == KeyKind::right || is(key, U'l')) {
        // Unfolds, or steps onto the first child of an unfolded link.
        if (page.rows.at(page.cursor.at).unfolded) {
            ++page.cursor.at;
            keep_visible(page.cursor, height_);
        } else {
            unfold(page);
        }
    } else if (key.kind == KeyKind::left || is(key, U'h')) {
        // Folds, steps up to the parent link, or at the top goes back.
        if (page.rows.at(page.cursor.at).unfolded) {
            fold(page);
        } else if (const auto up = parent_of(page.rows, page.cursor.at)) {
            page.cursor.at = *up;
            keep_visible(page.cursor, height_);
        } else {
            pages_.pop_back();
        }
    } else if (is_move(key)) {
        move_on_page(page, key, height_);
    }
}

std::size_t columns(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return !is_continuation(c); }));
}

std::string duration(double seconds) {
    const auto whole = seconds > 0 ? static_cast<std::uint64_t>(seconds) : 0;
    if (whole < 60) {
        return std::format("{}s", whole);
    }
    if (whole < 3600) {
        return std::format("{}m{:02}s", whole / 60, whole % 60);
    }
    return std::format("{}h{:02}m", whole / 3600, whole % 3600 / 60);
}

std::string byte_size(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    constexpr std::array<std::string_view, 5> units{"KiB", "MiB", "GiB", "TiB", "PiB"};
    auto value = static_cast<double>(bytes) / 1024;
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units.at(unit));
}

std::string progress_bar(std::uint64_t done, std::uint64_t total, std::size_t width,
                         const Glyphs& glyph) {
    const auto eighths = columns(glyph.bar_eighths);
    // In eighths of a cell, whether or not the glyphs can show them.
    const auto filled = total == 0 ? 0 : std::min(done, total) * width * 8 / total;
    const auto full = static_cast<std::size_t>(filled / 8);
    const auto part = static_cast<std::size_t>(filled % 8);
    std::string bar = repeat(glyph.bar_full, full);
    std::size_t used = full;
    if (part > 0 && eighths >= 7 && used < width) {
        bar += code_points(glyph.bar_eighths, part - 1, 1);
        ++used;
    }
    return bar + repeat(glyph.bar_empty, width - used);
}

std::string spinner_frame(std::size_t count, const Glyphs& glyph) {
    const auto frames = columns(glyph.spinner);
    return frames == 0 ? std::string{} : code_points(glyph.spinner, count % frames, 1);
}

std::vector<Span> sparkline(const std::vector<std::optional<double>>& values, double max,
                            std::size_t width, const Glyphs& glyph, Tone tone,
                            std::optional<double> limit) {
    const auto levels = columns(glyph.spark);
    std::vector<Span> spans;
    // Each cell joins the span before it when it is drawn in the same tone.
    std::optional<Tone> last_tone;
    const auto add = [&](std::string cell, Tone cell_tone) {
        if (last_tone == cell_tone) {
            spans.back().text += cell;
        } else {
            spans.push_back({std::move(cell), tone_pen(cell_tone)});
            last_tone = cell_tone;
        }
    };
    const auto shown = std::min(values.size(), width);
    for (std::size_t i = shown; i < width; ++i) {
        add(" ", tone);
    }
    for (std::size_t i = values.size() - shown; i < values.size(); ++i) {
        const auto& value = values.at(i);
        if (!value || levels == 0) {
            add(" ", tone);
            continue;
        }
        const auto scaled = max > 0 ? std::clamp(*value / max, 0.0, 1.0) : 0.0;
        const auto level =
            static_cast<std::size_t>(std::lround(scaled * static_cast<double>(levels - 1)));
        add(code_points(glyph.spark, level, 1), limit && *value > *limit ? Tone::bad : tone);
    }
    return spans;
}

std::vector<std::vector<Span>> pressure_lines(const pressure::History& history,
                                              const Limits& limits, std::size_t width,
                                              const Glyphs& glyph) {
    const auto& readings = history.readings();
    const auto& latest = history.latest();
    const auto series = [&](auto value) {
        std::vector<std::optional<double>> values;
        values.reserve(readings.size());
        for (const auto& reading : readings) {
            values.push_back(value(reading));
        }
        return values;
    };
    const auto label = [](std::string_view name) {
        return Span{std::format(" {:<9}", name), tone_pen(Tone::heading)};
    };
    const auto note = [](std::string text) { return Span{std::move(text), tone_pen(Tone::note)}; };
    const auto last = [](const std::vector<std::optional<double>>& values) {
        return values.empty() ? std::nullopt : values.back();
    };
    std::vector<std::vector<Span>> lines{{{" System", tone_pen(Tone::heading)}}};

    const auto cpu = series([](const pressure::Reading& r) {
        return r.cpu_busy ? std::optional<double>{*r.cpu_busy * 100} : std::nullopt;
    });
    auto line = std::vector<Span>{label("CPU")};
    std::ranges::move(sparkline(cpu, 100, width, glyph, Tone::choice), std::back_inserter(line));
    const auto cpu_now = last(cpu);
    line.push_back(
        {cpu_now ? std::format("  {:>5.0f}%", *cpu_now) : "      -", tone_pen(Tone::version)});
    if (latest && latest->cpus > 0) {
        line.push_back(note(std::format("   {} CPUs", latest->cpus)));
    }
    lines.push_back(std::move(line));

    // Memory in use, so that higher is harder pressed like the rest.
    const auto total = latest && latest->mem_total ? static_cast<double>(*latest->mem_total) : 0.0;
    const auto used = series([&](const pressure::Reading& r) {
        return r.mem_available && total > 0
                   ? std::optional<double>{total - static_cast<double>(*r.mem_available)}
                   : std::nullopt;
    });
    const auto used_limit =
        limits.min_available && total > 0
            ? std::optional<double>{total - static_cast<double>(*limits.min_available)}
            : std::nullopt;
    line = {label("Memory")};
    std::ranges::move(sparkline(used, total, width, glyph, Tone::host, used_limit),
                      std::back_inserter(line));
    if (latest && latest->mem_available && latest->mem_total) {
        line.push_back({std::format("  {} available of {}", byte_size(*latest->mem_available),
                                    byte_size(*latest->mem_total)),
                        tone_pen(Tone::version)});
    }
    if (limits.min_available) {
        line.push_back(
            note(std::format("   steve waits under {}", byte_size(*limits.min_available))));
    }
    lines.push_back(std::move(line));

    const auto load = series([](const pressure::Reading& r) { return r.load; });
    auto load_max =
        std::max(latest ? static_cast<double>(latest->cpus) : 0.0, limits.load.value_or(0.0));
    for (const auto& value : load) {
        load_max = std::max(load_max, value.value_or(0.0));
    }
    line = {label("Load")};
    std::ranges::move(sparkline(load, load_max, width, glyph, Tone::runtime, limits.load),
                      std::back_inserter(line));
    const auto load_now = last(load);
    line.push_back(
        {load_now ? std::format("  {:>6.2f}", *load_now) : "       -", tone_pen(Tone::version)});
    if (limits.load) {
        line.push_back(note(std::format("   steve waits over {:g}", *limits.load)));
    }
    lines.push_back(std::move(line));

    line = {label("Stalls")};
    const auto stalls = latest ? latest->stalls : pressure::Stalls{};
    if (stalls.cpu || stalls.memory || stalls.io) {
        const auto percent = [](std::optional<double> value) {
            return value ? std::format("{:.1f}%", *value) : std::string{"-"};
        };
        line.push_back({std::format("cpu {}  memory {}  io {}", percent(stalls.cpu),
                                    percent(stalls.memory), percent(stalls.io)),
                        tone_pen(Tone::version)});
        line.push_back(note("   of the last 10 s"));
    } else {
        line.push_back(note("not kept by this kernel; psi=1 on its command line turns them on"));
    }
    lines.push_back(std::move(line));
    return lines;
}

std::string task_state(const emerge::Task& task) {
    if (task.merge_wait) {
        return "waiting to merge";
    }
    if (!task.phase.empty()) {
        return task.phase;
    }
    return task.kind == emerge::TaskKind::merge ? "merging" : "starting";
}

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

std::string clip(std::string_view text, std::size_t width) {
    std::size_t kept = 0;
    std::size_t bytes = 0;
    while (bytes < text.size() && kept < width) {
        ++bytes;
        while (bytes < text.size() && is_continuation(text.at(bytes))) {
            ++bytes;
        }
        ++kept;
    }
    return std::string{text.substr(0, bytes)};
}

Pen tone_pen(Tone tone) {
    const auto style = tone_style(tone);
    return {.fg = Color{.red = style.red, .green = style.green, .blue = style.blue},
            .bg = std::nullopt,
            .bold = style.bold,
            .italic = style.italic};
}

std::vector<Span> cpv_spans(std::string_view cpv) {
    const auto parts = split_cpv(cpv);
    std::vector<Span> spans;
    if (!parts.category.empty()) {
        spans.push_back({std::string{parts.category}, tone_pen(Tone::category)});
        spans.push_back({"/", tone_pen(Tone::note)});
    }
    spans.push_back({std::string{parts.name}, tone_pen(Tone::name)});
    if (!parts.version.empty()) {
        spans.push_back({"-", tone_pen(Tone::note)});
        spans.push_back({std::string{parts.version}, tone_pen(Tone::version)});
    }
    return spans;
}

DriftSign drift_sign(char sign) {
    switch (sign) {
    case '+':
        return {.sign = "+", .meaning = "installed since the store was built", .tone = Tone::good};
    case '-':
        return {.sign = "-", .meaning = "gone since the store was built", .tone = Tone::bad};
    default:
        return {.sign = "~", .meaning = "changed since the store was built", .tone = Tone::choice};
    }
}

Exit open_and_run(const Store& store, GlyphSet glyphs, const Services& services,
                  std::span<const std::string> warnings, std::ostream& err) {
#if EGRAPH_HAVE_TUI
    auto screen = Screen::open();
    if (!screen) {
        err << "egraph: tui: " << screen.error() << '\n';
        return Exit::failure;
    }
    const auto graph = build_graph(store);
    App app{store, graph, services.rebuild ? Update::save : Update::preview};
    if (!warnings.empty()) {
        app.show({.error = false, .title = "Warning", .lines = {warnings.begin(), warnings.end()}});
    }
    run(*screen, app, egraph::glyphs(glyphs), services);
    return Exit::ok;
#else
    (void)store;
    (void)glyphs;
    (void)services;
    (void)warnings;
    err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
    return Exit::not_implemented;
#endif
}

} // namespace egraph::tui
