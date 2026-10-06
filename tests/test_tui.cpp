#include "tui.hpp"

#include "index_builder.hpp"
#include "store_writer.hpp"
#include "system_builder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

using egraph::tui::Key;
using egraph::tui::KeyKind;
using egraph::tui::RowType;

namespace {

// A grid of cells (one code point each) in place of a terminal, fed keys from a script.
class FakeScreen {
  public:
    FakeScreen(unsigned rows, unsigned cols, std::deque<Key> keys)
        : rows_(rows), cols_(cols), keys_(std::move(keys)) {
        clear();
    }

    [[nodiscard]] egraph::tui::Size size() const { return {.rows = rows_, .cols = cols_}; }
    void clear() { cells_.assign(rows_, std::vector<std::string>(cols_, " ")); }
    void put(unsigned row, unsigned col, std::string_view text, const egraph::tui::Pen&) {
        for (std::size_t i = 0; i < text.size() && row < rows_ && col < cols_; ++col) {
            std::size_t length = 1;
            while (i + length < text.size() &&
                   (static_cast<unsigned char>(text.at(i + length)) & 0xC0U) == 0x80U) {
                ++length;
            }
            cells_.at(row).at(col) = std::string{text.substr(i, length)};
            i += length;
        }
    }
    void fill_row(unsigned row, const egraph::tui::Pen& pen) {
        put(row, 0, std::string(cols_, ' '), pen);
    }
    void render() { ++renders; }
    Key read(std::optional<std::chrono::milliseconds> timeout) {
        timeouts.push_back(timeout);
        if (keys_.empty()) {
            return {.kind = KeyKind::closed};
        }
        const auto key = keys_.front();
        keys_.pop_front();
        return key;
    }

    [[nodiscard]] std::string line(unsigned row) const {
        std::string out;
        for (const auto& cell : cells_.at(row)) {
            out += cell;
        }
        return out;
    }
    [[nodiscard]] std::string text() const {
        std::string out;
        for (unsigned row = 0; row < rows_; ++row) {
            out += line(row) + '\n';
        }
        return out;
    }
    [[nodiscard]] bool keys_left() const { return !keys_.empty(); }
    int renders = 0;
    // Each read's timeout.
    std::vector<std::optional<std::chrono::milliseconds>> timeouts;

  private:
    unsigned rows_;
    unsigned cols_;
    std::vector<std::vector<std::string>> cells_;
    std::deque<Key> keys_;
};

Key character(char32_t code) {
    return {.kind = KeyKind::character, .code = code};
}

Key key(KeyKind kind) {
    return {.kind = kind};
}

// app-misc/a-1 RDEPEND "|| ( dev-libs/b dev-libs/missing ) !app-misc/old", and dev-libs/b-1.
egraph::Store sample() {
    auto store = egraph::decode(egraph::test::fresh_sample());
    REQUIRE(store.has_value());
    return std::move(*store);
}

// The sample, but b-1 also has RDEPEND app-misc/a, closing a cycle.
egraph::Store cyclic() {
    egraph::test::Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(1);
    packages.varint(0).varint(0).varint(2).list({0});
    packages.varint(0).varint(0);
    const auto bytes = egraph::test::with_section(4, packages);
    auto decoded = egraph::decode(bytes);
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

// The sample, but a-1 only needs b-1 to build: DEPEND dev-libs/b.
egraph::Store build_only() {
    egraph::test::Bytes packages;
    packages.varint(2);
    packages.varints({1, 2, 3, 3, 4, 5, 1}).list({6}).list({6}).varint(0);
    packages.varint(0).varint(1);
    packages.varint(0).varint(0).varint(7).list({1});
    packages.varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    packages.varints({8, 7, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    packages.varint(0).varint(0);
    auto decoded = egraph::decode(egraph::test::with_section(4, packages));
    REQUIRE(decoded.has_value());
    return std::move(*decoded);
}

// The evaluated store beside the sample (store_writer.hpp): a-1 depends on dev-libs/b:= under
// dynamic deps, would depend on dev-libs/b with +flag and on dev-libs/gone with +flag -minimal,
// and is rebuilt for flag* -new%; b-1 is masked, not visible, and upgrades to b-2.
egraph::Evaluated decoded_evaluated(const std::vector<std::byte>& bytes) {
    auto evaluated = egraph::decode_evaluated(bytes);
    REQUIRE(evaluated.has_value());
    return std::move(*evaluated);
}

egraph::Evaluated evaluated_sample() {
    return decoded_evaluated(egraph::test::assemble_evaluated(egraph::test::evaluated_sections()));
}

// The evaluated sample with its dependencies section replaced.
egraph::Evaluated evaluated_sample(const egraph::test::Bytes& dependencies) {
    return decoded_evaluated(egraph::test::evaluated_with_section(4, dependencies));
}

egraph::Stores both() {
    return {.installed = sample(), .evaluated = evaluated_sample()};
}

// Both, but nothing pending for a-1.
egraph::Stores only_b_updates() {
    egraph::test::Bytes dependencies;
    dependencies.varint(2);
    dependencies.varints({1, 0, 3}).varint(1).varint(11).varint(12);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(1);
    dependencies.varints({0, 0, 4}).list({1});
    dependencies.varint(2);
    dependencies.varints({4, 13, 0}).list({1}).list({8});
    dependencies.varints({0, 14, 1}).list({}).list({8, 15});
    dependencies.varints({1, 0, 1, 0}).list({}).varints({0, 0, 0, 0, 0, 0});
    dependencies.varints({2, 1, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({0, 1, 1, 3}).list({}).varints({0, 0, 0, 0, 0, 0});
    return {.installed = sample(), .evaluated = evaluated_sample(dependencies)};
}

const auto& ascii = egraph::glyphs(egraph::GlyphSet::ascii);

// Services whose jobs have their results at once.
egraph::tui::Checker checking(std::function<egraph::tui::CheckResult(const egraph::Store&)> check) {
    return [check = std::move(check)](const egraph::Store& stored) {
        return egraph::ready(check(stored));
    };
}
egraph::tui::Rebuilder
rebuilding(std::function<std::expected<egraph::Stores, std::string>()> rebuild) {
    return [rebuild = std::move(rebuild)] {
        return egraph::ready(rebuild().transform([](egraph::Stores stores) {
            return std::make_shared<const egraph::Stores>(std::move(stores));
        }));
    };
}

// A check whose fresh build is the cyclic store, one package differing.
const egraph::tui::Checker cyclic_check =
    checking([](const egraph::Store&) -> egraph::tui::CheckResult {
        return egraph::tui::Fresh{.store = cyclic(), .drift = {"~dev-libs/b-1"}};
    });

const egraph::tui::Checker no_check =
    checking([](const egraph::Store&) -> egraph::tui::CheckResult {
        return std::unexpected("no builder in tests");
    });

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("links group a package's edges by package and atom") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    const auto deps = egraph::tui::links(store, graph, 0, false);
    REQUIRE(deps.size() == 1);
    CHECK(deps.front().package == 1);
    CHECK(deps.front().choice);
    // RDEPEND, the first shorthand.
    CHECK(deps.front().kinds == std::array<bool, 5>{true, false, false, false, false});
    const auto rdeps = egraph::tui::links(store, graph, 1, true);
    REQUIRE(rdeps.size() == 1);
    CHECK(rdeps.front().package == 0);
}

TEST_CASE("the list shows every package and quits on q") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{10, 100, {key(KeyKind::down), character(U'q'), character(U'z')}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(contains(screen.line(0), "2 of 2 packages"));
    // a-1 is marked as kept by @selected.
    CHECK(screen.line(3).starts_with("   @  app-misc/a-1"));
    // The cursor moved down to b-1 before q.
    CHECK(app.list().cursor.at == 1);
    CHECK(screen.line(4).starts_with(" >    dev-libs/b-1"));
    CHECK(contains(screen.line(9), "q quit"));
    CHECK(screen.keys_left());
}

TEST_CASE("typing after / filters the list as it goes") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& pressed :
         {character(U'/'), character(U'L'), character(U'I'), character(U'B')}) {
        app.handle(pressed);
    }
    CHECK(app.list().searching);
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    app.handle(key(KeyKind::backspace));
    CHECK(app.list().query == "LI");
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.list().searching);
    // q is a key again once the search is kept.
    app.handle(key(KeyKind::escape));
    CHECK(app.list().query.empty());
    CHECK(app.list().shown.size() == 2);
    app.handle(character(U'/'));
    app.handle(character(U'q'));
    CHECK_FALSE(app.done());
    CHECK(app.list().shown.empty());
}

TEST_CASE("enter opens a package's page, and pages open from there") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{16, 80, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    REQUIRE(app.pages().size() == 1);
    const auto& page = app.pages().back();
    CHECK(page.package == 0);
    CHECK(page.rows.at(page.cursor.at).type == RowType::link);
    const auto text = screen.text();
    CHECK(contains(text, "Depends on  1"));
    CHECK(contains(text, "R....   dev-libs/b-1"));
    CHECK(contains(text, "dev-libs/b |"));
    CHECK(contains(text, "Needed by  0"));
    CHECK(contains(text, "nothing"));
    CHECK(contains(screen.line(0), "> a"));

    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 1);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "> a > b"));
    CHECK(contains(screen.text(), "Needed by  1"));

    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::backspace));
    CHECK(app.pages().empty());
    app.handle(character(U'q'));
    CHECK(app.done());
}

TEST_CASE("the end of input quits, and small terminals get nothing drawn") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const unsigned rows : {0U, 1U, 4U, 5U}) {
        egraph::tui::App app{store, graph};
        FakeScreen screen{rows, 12, {}};
        egraph::tui::run(screen, app, ascii, {.check = no_check});
        CHECK(screen.renders == 1);
        CHECK(app.done());
    }
}

TEST_CASE("thread draws each level's line down to its last child") {
    // a, with children b (with child c) and d.
    std::vector<egraph::tui::Row> rows(5);
    rows.at(0).type = RowType::heading;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        rows.at(i).type = RowType::link;
    }
    rows.at(1).depth = 0;
    rows.at(2).depth = 1;
    rows.at(3).depth = 2;
    rows.at(4).depth = 1;
    egraph::tui::thread(rows);
    CHECK_FALSE(rows.at(2).last);
    CHECK(rows.at(3).last);
    CHECK(rows.at(3).rails == std::vector<bool>{true});
    CHECK(rows.at(4).last);
    CHECK(rows.at(4).rails.empty());

    // With d gone, nothing follows b at level 1.
    rows.pop_back();
    egraph::tui::thread(rows);
    CHECK(rows.at(2).last);
    CHECK(rows.at(3).rails == std::vector<bool>{false});
}

TEST_CASE("links unfold in place, and stop at a cycle") {
    const auto store = cyclic();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{14, 80, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    REQUIRE(app.pages().size() == 1);
    CHECK(contains(screen.text(), "R.... + dev-libs/b-1"));
    CHECK(contains(screen.text(), "space unfold"));

    const auto at = app.pages().back().cursor.at;
    app.handle(character(U' '));
    egraph::tui::draw(screen, app, ascii);
    const auto& rows = app.pages().back().rows;
    REQUIRE(rows.at(at + 1).type == RowType::link);
    CHECK(rows.at(at + 1).depth == 1);
    CHECK(rows.at(at + 1).cycle);
    CHECK(contains(screen.text(), "R.... - dev-libs/b-1"));
    CHECK(contains(screen.text(), "R.... ^ `- app-misc/a-1"));

    // The cycle does not unfold; h climbs to its parent, folds it, then goes back.
    app.handle(character(U'j'));
    CHECK(app.pages().back().cursor.at == at + 1);
    app.handle(character(U' '));
    CHECK(app.pages().back().rows.size() == rows.size());
    app.handle(character(U'h'));
    CHECK(app.pages().back().cursor.at == at);
    app.handle(character(U'h'));
    CHECK_FALSE(app.pages().back().rows.at(at).unfolded);
    CHECK(app.pages().back().rows.at(at + 1).type != RowType::link);
    app.handle(character(U'h'));
    CHECK(app.pages().empty());
}

TEST_CASE("l unfolds and then steps in; rdeps unfold upwards") {
    const auto store = cyclic();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.set_height(10);
    app.handle(key(KeyKind::enter));
    const auto at = app.pages().back().cursor.at;
    app.handle(key(KeyKind::right));
    CHECK(app.pages().back().rows.at(at).unfolded);
    app.handle(key(KeyKind::right));
    CHECK(app.pages().back().cursor.at == at + 1);

    // Down past the cycle row to b-1 under Needed by, whose dependents are a-1 again.
    app.handle(key(KeyKind::down));
    const auto& page = app.pages().back();
    REQUIRE(page.rows.at(page.cursor.at).reverse);
    app.handle(key(KeyKind::tab));
    const auto& child = app.pages().back().rows.back();
    CHECK(child.reverse);
    CHECK(child.depth == 1);
    CHECK(child.link.package == 0);
    CHECK(child.cycle);
    app.handle(key(KeyKind::tab));
    CHECK(app.pages().back().rows.back().depth == 0);
}

TEST_CASE("a page starts with the chain that keeps its package") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{16, 80, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(screen.line(3), "Kept by"));
    CHECK(contains(screen.line(4), "@ @selected  app-misc/a"));
    CHECK(contains(screen.line(5), ".....   `- app-misc/a-1"));
    CHECK(contains(screen.line(6), "R....     `- dev-libs/b-1"));
    CHECK(contains(screen.line(6), "dev-libs/b |"));

    // The cursor starts on the dependents; up reaches the chain, and enter follows it, except
    // onto the page's own package.
    const auto& page = app.pages().back();
    CHECK(page.rows.at(page.cursor.at).type == RowType::link);
    app.handle(key(KeyKind::up));
    CHECK(app.pages().back().rows.at(app.pages().back().cursor.at).type == RowType::path);
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().size() == 1);
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 0);
}

TEST_CASE("o shows only orphans, and b drops build-time dependencies") {
    const auto store = build_only();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U'o'));
    CHECK(app.list().only == egraph::tui::Only::orphans);
    CHECK(app.list().shown.empty());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "0 orphans"));
    CHECK(contains(screen.text(), "nothing to remove"));
    CHECK(contains(screen.text(), "o all"));

    app.handle(character(U'b'));
    CHECK_FALSE(app.build_deps());
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 orphans  run-time deps only"));
    CHECK(screen.line(3).starts_with(" > -  dev-libs/b-1"));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Kept by, at run time"));
    CHECK(contains(screen.text(), "- nothing: depclean would remove it"));
}

TEST_CASE("without roots depclean refuses, and the orphans view says so") {
    const auto store = [] {
        auto decoded =
            egraph::decode(egraph::test::with_section(5, egraph::test::Bytes{}.varint(0)));
        REQUIRE(decoded.has_value());
        return std::move(*decoded);
    }();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U'o'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "2 orphans  ! depclean would refuse to run"));
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "nothing: @world is empty"));
}

TEST_CASE("! shows only broken packages, and a page lists what is not installed") {
    // a-1 alone, RDEPEND: dev-libs/missing dev-libs/b, neither installed.
    const auto store = [] {
        auto decoded = egraph::decode(egraph::test::with_rdepend(2, [](egraph::test::Bytes& b) {
            b.varints({0, 0, 14}).list({});
            b.varints({0, 0, 7}).list({});
        }));
        REQUIRE(decoded.has_value());
        return std::move(*decoded);
    }();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK(app.broken(0) == 2);
    FakeScreen screen{14, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(3).starts_with(" > @! app-misc/a-1"));
    CHECK(contains(screen.text(), "! broken"));

    app.handle(character(U'!'));
    CHECK(app.list().shown == std::vector<std::uint32_t>{0});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 broken"));
    CHECK(contains(screen.text(), "! all"));

    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, "Not installed  2"));
    CHECK(contains(text, "R.... ! dev-libs/b"));
    CHECK(contains(text, "R.... ! dev-libs/missing"));
}

TEST_CASE("without build-time dependencies, only run-time ones count as broken") {
    // a-1 alone, DEPEND: dev-libs/missing.
    egraph::test::Bytes packages;
    packages.varint(1).varints({1, 2, 3, 3, 4, 5, 1}).list({}).list({}).varint(0);
    packages.varint(0).varint(1).varints({0, 0, 14}).list({});
    packages.varint(0).varint(0).varint(0).varint(0).varint(0);
    auto decoded = egraph::decode(egraph::test::with_section(4, packages));
    REQUIRE(decoded.has_value());
    const auto& store = *decoded;
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'!'));
    CHECK(app.list().shown.size() == 1);
    app.handle(character(U'b'));
    CHECK(app.broken(0) == 0);
    CHECK(app.list().shown.empty());
    app.handle(character(U'!'));
    app.handle(key(KeyKind::enter));
    const auto& rows = app.pages().back().rows;
    CHECK(std::ranges::none_of(rows, [](const auto& row) { return row.type == RowType::missing; }));
}

TEST_CASE("c checks the store against a fresh build, showing a wait first") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {character(U'c')}};
    std::string waiting;
    int checks = 0;
    const egraph::tui::Checker check =
        checking([&](const egraph::Store& stored) -> egraph::tui::CheckResult {
            CHECK(&stored == &store);
            waiting = screen.text();
            ++checks;
            return egraph::tui::Fresh{.store = sample(), .drift = {"+x/new-1", "~dev-libs/b-1"}};
        });
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(checks == 1);
    CHECK(contains(waiting, "Building a fresh store"));
    REQUIRE(app.checked().has_value());
    CHECK(app.checked()->stage == egraph::tui::Checked::Stage::checked);
    const auto text = screen.text();
    CHECK(contains(text, "differs from a fresh build  2"));
    CHECK(contains(text, " > + x/new-1"));
    CHECK(contains(text, "installed since the store was built"));
    CHECK(contains(text, "~ dev-libs/b-1"));

    // What only the fresh build has cannot be opened, and says so; the rest opens its page, and
    // esc comes back here, then to the list.
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "x/new-1 is not in the store");
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "u shows the fresh build, which has it."));
    // The key that closes a dialog does nothing else.
    app.handle(key(KeyKind::down));
    CHECK_FALSE(app.dialog().has_value());
    CHECK(app.checked()->cursor.at == 0);
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    app.handle(key(KeyKind::escape));
    CHECK(app.checked().has_value());
    app.handle(character(U'r'));
    CHECK(app.check_requested());
    app.finish_check(egraph::tui::Fresh{.store = sample(), .drift = {}});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "+ The store matches a fresh build"));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.checked().has_value());
}

TEST_CASE("a check's build runs in the background while the screen still answers") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 120, {character(U'c'), key(KeyKind::tick), key(KeyKind::tick)}};
    int polls = 0;
    std::vector<std::string> waiting;
    const egraph::tui::Checker check =
        [&](const egraph::Store&) -> egraph::Job<egraph::tui::CheckResult> {
        return [&]() -> std::optional<egraph::tui::CheckResult> {
            waiting.push_back(screen.text());
            if (++polls < 3) {
                return std::nullopt;
            }
            return egraph::tui::Fresh{.store = sample(), .drift = {}};
        };
    };
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(polls == 3);
    // Read with a timeout while it runs, so the spinner turns; then without.
    REQUIRE(screen.timeouts.size() >= 3);
    CHECK(screen.timeouts.at(1) == egraph::tui::wait_interval);
    CHECK(screen.timeouts.at(2) == egraph::tui::wait_interval);
    CHECK(screen.timeouts.back() == std::nullopt);
    REQUIRE(waiting.size() == 3);
    CHECK(contains(waiting.at(0), "| Building a fresh store"));
    CHECK(contains(waiting.at(1), "/ Building a fresh store"));
    CHECK(contains(waiting.at(0), "esc stop"));
    CHECK(contains(screen.text(), "+ The store matches a fresh build"));
}

TEST_CASE("leaving the check view stops its build") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const auto leave : {key(KeyKind::escape), character(U'q')}) {
        egraph::tui::App app{store, graph};
        FakeScreen screen{12, 120, {character(U'c'), leave}};
        const auto alive = std::make_shared<int>(0);
        const egraph::tui::Checker check =
            [&](const egraph::Store&) -> egraph::Job<egraph::tui::CheckResult> {
            return [alive]() -> std::optional<egraph::tui::CheckResult> { return std::nullopt; };
        };
        egraph::tui::run(screen, app, ascii, {.check = check});
        CHECK(alive.use_count() == 1);
        if (leave.kind == KeyKind::escape) {
            CHECK_FALSE(app.checked().has_value());
        }
    }
}

TEST_CASE("a check that cannot run shows why") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {}};
    app.handle(character(U'c'));
    app.finish_check(
        std::unexpected("egraph-build exited with status 1:\nTraceback\nKeyError: 'x'"));
    egraph::tui::draw(screen, app, ascii);
    // Back at the list, under the dialog.
    CHECK_FALSE(app.checked().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->lines.size() == 3);
    const auto text = screen.text();
    CHECK(contains(text, "+- ! The check could not run -"));
    CHECK(contains(text, "|  egraph-build exited with status 1:"));
    CHECK(contains(text, "|  KeyError: 'x'"));
    CHECK(contains(text, "any key -+"));
    CHECK(contains(screen.line(0), "2 of 2 packages"));
    app.handle(character(U'q'));
    CHECK_FALSE(app.done());
    CHECK_FALSE(app.dialog().has_value());
}

TEST_CASE("a dialog sits centred in its frame and cuts what does not fit") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.show({.error = false, .title = "Warning", .lines = {"short", std::string(300, 'x')}});
    FakeScreen screen{12, 40, {}};
    egraph::tui::draw(screen, app, ascii);
    // Six rows: the edges, a blank margin above and below, and two lines.
    CHECK(screen.line(3) == " +- Warning --------------------------+ ");
    CHECK(screen.line(4) == " |                                    | ");
    CHECK(screen.line(5) == " |  short                             | ");
    CHECK(screen.line(6) == " |  " + std::string(32, 'x') + "  | ");
    CHECK(screen.line(7) == " |                                    | ");
    CHECK(screen.line(8) == " +-------------------------- any key -+ ");

    // More lines than rows: the first ones show, and the frame stays whole.
    app.show({.error = true, .title = "Many", .lines = std::vector<std::string>(20, "line")});
    FakeScreen small{8, 30, {}};
    egraph::tui::draw(small, app, ascii);
    CHECK(contains(small.line(0), "+- ! Many "));
    CHECK(contains(small.line(2), "|  line"));
    CHECK(contains(small.line(5), "|  line"));
    CHECK_FALSE(contains(small.line(6), "line"));
    CHECK(contains(small.line(7), "any key -+"));
}

TEST_CASE("a build-time dependency since replaced is history, not breakage") {
    for (const bool build_time : {true, false}) {
        auto decoded = egraph::decode(egraph::test::newer_wanted(build_time));
        REQUIRE(decoded.has_value());
        const auto& store = *decoded;
        const auto graph = egraph::build_graph(store);
        egraph::tui::App app{store, graph};
        FakeScreen screen{16, 120, {}};
        egraph::tui::draw(screen, app, ascii);
        app.handle(key(KeyKind::enter));
        egraph::tui::draw(screen, app, ascii);
        const auto text = screen.text();
        if (build_time) {
            CHECK(app.broken(0) == 0);
            CHECK(contains(text, "Built with, since replaced  1"));
            CHECK(contains(text, "....B   || ( >=dev-libs/b-2 dev-libs/missing )  > dev-libs/b-1"));
            CHECK_FALSE(contains(text, "Not installed"));
        } else {
            // At run time the package may still need what it asked for.
            CHECK(app.broken(0) == 1);
            CHECK(contains(text, "Not installed  1"));
            CHECK(contains(text, "R.... ! || ( >=dev-libs/b-2 dev-libs/missing )  > dev-libs/b-1"));
        }
    }
}

TEST_CASE("u shows a check's fresh build without saving it") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK(app.dependents(0) == 0);
    FakeScreen screen{12, 120, {character(U'c')}};
    int rebuilds = 0;
    const egraph::tui::Rebuilder rebuild =
        rebuilding([&]() -> std::expected<egraph::Stores, std::string> {
            ++rebuilds;
            return egraph::Stores{.installed = cyclic(), .evaluated = {}};
        });
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check, .rebuild = rebuild});
    CHECK(contains(screen.text(), "u shows the fresh build, without saving it"));
    CHECK(contains(screen.line(11), "u preview"));
    app.handle(key(KeyKind::enter));
    app.handle(key(KeyKind::escape));
    app.handle(character(U'u'));
    CHECK(rebuilds == 0);
    CHECK(app.source() == egraph::tui::Source::preview);
    CHECK(app.pages().empty());
    // In the fresh build b-1 depends on a-1.
    CHECK(app.dependents(0) == 1);
    CHECK(&app.store() != &store);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(),
                   "+ Showing the fresh build   not saved: only root writes the store"));
    CHECK(screen.line(0).ends_with(" preview, not saved "));
    CHECK_FALSE(contains(screen.line(11), "u preview"));

    // Checking again compares the build on screen.
    app.handle(character(U'r'));
    REQUIRE(app.check_requested());
    const auto* shown = &app.store();
    app.finish_check(egraph::tui::Fresh{.store = cyclic(), .drift = {}});
    CHECK(shown == &app.store());
    app.handle(key(KeyKind::escape));
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(0).ends_with(" preview, not saved "));
    CHECK(contains(screen.text(), "app-misc/a-1"));

    // The adopted store moves with the app.
    auto moved = std::move(app);
    CHECK(moved.dependents(0) == 1);
    moved.handle(key(KeyKind::enter));
    CHECK(moved.pages().size() == 1);
}

TEST_CASE("u rebuilds the store where it can be written") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph, egraph::tui::Update::save};
    FakeScreen screen{12, 120, {character(U'c')}};
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check});
    REQUIRE(app.checked().has_value());
    // Only a preview needs the check's build.
    CHECK_FALSE(app.checked()->fresh.has_value());
    CHECK(contains(screen.text(), "differs from a fresh build  1   u rebuilds it"));
    CHECK(contains(screen.line(11), "u rebuild"));

    app.handle(character(U'u'));
    CHECK(app.rebuild_requested());
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Rebuilding the store"));
    app.handle(character(U'u'));
    app.finish_rebuild(std::make_shared<const egraph::Stores>(
        egraph::Stores{.installed = cyclic(), .evaluated = {}}));
    CHECK(app.source() == egraph::tui::Source::saved);
    CHECK(app.dependents(0) == 1);
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "+ Rebuilt the store   showing it now"));
    CHECK_FALSE(contains(screen.line(0), "preview"));
    app.handle(character(U'u'));
    CHECK_FALSE(app.rebuild_requested());
}

TEST_CASE("a failed rebuild leaves the store as it was") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph, egraph::tui::Update::save};
    FakeScreen screen{12, 120, {character(U'c'), character(U'u')}};
    const egraph::tui::Rebuilder rebuild =
        rebuilding([]() -> std::expected<egraph::Stores, std::string> {
            return std::unexpected("egraph-build exited with status 1:\nPermissionError");
        });
    egraph::tui::run(screen, app, ascii, {.check = cyclic_check, .rebuild = rebuild});
    CHECK(app.source() == egraph::tui::Source::opened);
    CHECK(&app.store() == &store);
    CHECK(app.dependents(0) == 0);
    CHECK(contains(screen.text(), "! The rebuild failed; the store is as it was"));
    CHECK(contains(screen.text(), "|  PermissionError"));
    // The drift still shows under the dialog, and u can try again.
    CHECK(contains(screen.text(), "differs from a fresh build  1"));
    app.handle(key(KeyKind::escape));
    CHECK(app.checked().has_value());
    app.handle(character(U'u'));
    CHECK(app.rebuild_requested());
}

TEST_CASE("u does nothing without drift to fix") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    for (const bool fails : {false, true}) {
        egraph::tui::App app{store, graph, egraph::tui::Update::save};
        FakeScreen screen{12, 120, {character(U'u'), character(U'c'), character(U'u')}};
        int rebuilds = 0;
        const egraph::tui::Checker check =
            checking([&](const egraph::Store&) -> egraph::tui::CheckResult {
                if (fails) {
                    return std::unexpected("no builder");
                }
                return egraph::tui::Fresh{.store = sample(), .drift = {}};
            });
        const egraph::tui::Rebuilder rebuild =
            rebuilding([&]() -> std::expected<egraph::Stores, std::string> {
                ++rebuilds;
                return egraph::Stores{.installed = sample(), .evaluated = {}};
            });
        egraph::tui::run(screen, app, ascii, {.check = check, .rebuild = rebuild});
        CHECK(rebuilds == 0);
        CHECK(app.source() == egraph::tui::Source::opened);
        // A failed check closes its view; the dialog took the second u.
        CHECK(app.checked().has_value() == !fails);
        CHECK_FALSE(app.dialog().has_value());
        if (!fails) {
            CHECK(app.checked()->stage == egraph::tui::Checked::Stage::checked);
            CHECK_FALSE(contains(screen.line(11), "u "));
        }
    }
}

namespace {

// dev-libs/b-2 compiling with cgroup counters, x/new-1 built and waiting to merge, and a
// binary package that has not started a phase.
std::vector<egraph::emerge::Snapshot> running_emerges() {
    using egraph::emerge::TaskKind;
    return {{.pid = 4321,
             .timestamp = 0,
             .jobs = {.running = 3, .max = 4, .completed = 13, .total = 40, .failed = 1},
             .tasks = {{.cpv = "dev-libs/b-2",
                        .kind = TaskKind::build,
                        .phase = "compile",
                        .pid = 5000,
                        .elapsed = 100.5,
                        .build_elapsed = 100.5,
                        .resources = {.cpu_usec = 402000000, .mem_peak = 2097152}},
                       {.cpv = "x/new-1",
                        .kind = TaskKind::merge,
                        .phase = "merge-wait",
                        .merge_wait = true,
                        .elapsed = 80,
                        .build_elapsed = 55.25},
                       {.cpv = "app-misc/baz-1", .binary = true}}}};
}

} // namespace

TEST_CASE("durations, sizes, bars and spinners read at a glance") {
    CHECK(egraph::tui::duration(0.4) == "0s");
    CHECK(egraph::tui::duration(45) == "45s");
    CHECK(egraph::tui::duration(100.5) == "1m40s");
    CHECK(egraph::tui::duration(3725) == "1h02m");
    CHECK(egraph::tui::duration(-3) == "0s");
    CHECK(egraph::tui::byte_size(512) == "512 B");
    CHECK(egraph::tui::byte_size(1536) == "1.5 KiB");
    CHECK(egraph::tui::byte_size(2097152) == "2.0 MiB");
    CHECK(egraph::tui::byte_size(5ULL << 30U) == "5.0 GiB");

    const auto& unicode = egraph::glyphs(egraph::GlyphSet::unicode);
    CHECK(egraph::tui::progress_bar(12, 40, 20, unicode) ==
          "██████" + std::string{} + "░░░░░░░░░░░░░░");
    // 6.5 cells: the half is an eighths glyph, where ascii rounds down.
    CHECK(egraph::tui::progress_bar(13, 40, 20, unicode) == "██████▌░░░░░░░░░░░░░");
    CHECK(egraph::tui::progress_bar(13, 40, 20, ascii) == "######--------------");
    CHECK(egraph::tui::progress_bar(0, 0, 4, ascii) == "----");
    CHECK(egraph::tui::progress_bar(9, 4, 4, ascii) == "####");

    CHECK(egraph::tui::spinner_frame(0, unicode) == "⠋");
    CHECK(egraph::tui::spinner_frame(11, unicode) == "⠙");
    CHECK(egraph::tui::spinner_frame(2, ascii) == "-");
}

TEST_CASE("e watches the running emerges, reading them again each second") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 140, {character(U'e'), key(KeyKind::tick), key(KeyKind::tick)}};
    int reads = 0;
    const egraph::tui::Watcher watch = [&] {
        ++reads;
        return running_emerges();
    };
    egraph::tui::run(screen, app, ascii, {.watch = watch});
    CHECK(reads == 3);
    REQUIRE(app.watched().has_value());
    CHECK(app.watched()->frame == 3);
    // The list waits for keys; the emerge view wakes to read again.
    CHECK(screen.timeouts.front() == std::nullopt);
    CHECK(screen.timeouts.back() == egraph::tui::watch_interval);

    const auto text = screen.text();
    CHECK(contains(screen.line(0), "egraph  > emerge"));
    CHECK(contains(text, " * emerge 4321   3 of 4 jobs   13 of 40 done   ######--------------  32%"
                         "   ! 1 failed"));
    // The spinner turned once a read, and the kinds: built, waiting, binary.
    CHECK(contains(text, " >   |- \\ b dev-libs/b-2"));
    CHECK(contains(text, "compile             1m40s   4.0x CPU   2.0 MiB peak"));
    CHECK(contains(text, "    |-   w x/new-1"));
    CHECK(contains(text, "waiting to merge      55s"));
    CHECK(contains(text, "    `- \\ p app-misc/baz-1"));
    CHECK(contains(text, "starting"));
    CHECK(contains(screen.line(11), "move"));
}

TEST_CASE("the emerge view says how to publish when nothing runs") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{12, 100, {character(U'e')}};
    egraph::tui::run(screen, app, ascii, {});
    CHECK(contains(screen.text(), "No emerge or egraph exec is publishing its progress"));
    CHECK(contains(screen.text(), "FEATURES=\"observability\""));
    CHECK(contains(screen.text(), "egraph exec to /run/egraph"));
    // Moving or opening in an empty view does nothing.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    CHECK_FALSE(app.dialog().has_value());
}

TEST_CASE("a task opens the page of its installed version, and esc comes back") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    REQUIRE(app.watch_requested());
    app.finish_watch(running_emerges());
    CHECK_FALSE(app.watch_requested());
    CHECK(app.refresh() == egraph::tui::watch_interval);

    // dev-libs/b-2 is building; b-1 is what is installed.
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 1);
    // No reads while the page covers the view, and none are owed.
    CHECK(app.refresh() == std::nullopt);
    app.handle(key(KeyKind::tick));
    CHECK_FALSE(app.watch_requested());
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.watch_requested());
    app.finish_watch(running_emerges());

    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "No version of x/new is installed yet");
    // A tick does not close the dialog.
    app.handle(key(KeyKind::tick));
    CHECK(app.dialog().has_value());
    app.handle(key(KeyKind::escape));

    // Fewer tasks on the next read keep the cursor on one of them.
    app.handle(key(KeyKind::end));
    CHECK(app.watched()->cursor.at == 2);
    auto fewer = running_emerges();
    fewer.front().tasks.pop_back();
    app.handle(key(KeyKind::tick));
    app.finish_watch(fewer);
    CHECK(app.watched()->cursor.at == 1);

    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.watched().has_value());
    CHECK(app.refresh() == std::nullopt);
}

namespace {

std::string joined(const std::vector<egraph::tui::Span>& spans) {
    std::string text;
    for (const auto& span : spans) {
        text += span.text;
    }
    return text;
}

// Two seconds of a 4-CPU machine with 8 GiB, half of the CPU time busy between them.
egraph::pressure::History two_samples(bool psi) {
    egraph::pressure::History history;
    const auto stalls = psi ? egraph::pressure::Stalls{.cpu = 12.5, .memory = 0.0, .io = 3.25}
                            : egraph::pressure::Stalls{};
    for (std::uint64_t tick = 1; tick <= 2; ++tick) {
        history.add({.cpu = egraph::pressure::CpuTimes{.busy = tick * 200, .total = tick * 400},
                     .cpus = 4,
                     .mem_total = 8ULL << 30U,
                     .mem_available = (8ULL - tick * 2) << 30U,
                     .load = 2.0 * static_cast<double>(tick),
                     .stalls = stalls});
    }
    return history;
}

} // namespace

TEST_CASE("sparklines scale to their maximum and mark what crosses the limit") {
    const auto& unicode = egraph::glyphs(egraph::GlyphSet::unicode);
    const std::vector<std::optional<double>> values{0.0, 50.0, std::nullopt, 100.0, 200.0};
    const auto spans = egraph::tui::sparkline(values, 100, 7, unicode, egraph::Tone::choice, 60);
    // Two blanks for the time before the first reading, one for the missing one.
    CHECK(joined(spans) == "  ▁▅ ██");
    // The last two cross the limit: one span for them, in the bad tone.
    REQUIRE(spans.size() == 2);
    CHECK(spans.back().text == "██");
    CHECK(spans.back().pen.fg->red == egraph::tui::tone_pen(egraph::Tone::bad).fg->red);
    // Only the last width values show.
    CHECK(joined(egraph::tui::sparkline(values, 100, 2, unicode, egraph::Tone::choice)) == "██");
    CHECK(joined(egraph::tui::sparkline({}, 100, 3, ascii, egraph::Tone::choice)) == "   ");
}

TEST_CASE("the pressure panel shows CPU, memory, load and stalls, with steve's limits") {
    const auto lines = egraph::tui::pressure_lines(
        two_samples(true), {.load = 3.0, .min_available = 4ULL << 30U}, 4, ascii);
    REQUIRE(lines.size() == egraph::tui::pressure_rows);
    CHECK(joined(lines.at(0)) == " System");
    // The first sample has nothing to measure CPU against. Load's second reading is over steve's
    // limit, drawn in the bad tone; memory's is at it, which is not.
    CHECK(joined(lines.at(1)) == " CPU         =     50%   4 CPUs");
    CHECK(joined(lines.at(2)) ==
          " Memory     -=  4.0 GiB available of 8.0 GiB   steve waits under 4.0 GiB");
    CHECK(joined(lines.at(3)) == " Load       =#    4.00   steve waits over 3");
    CHECK(joined(lines.at(4)) == " Stalls   cpu 12.5%  memory 0.0%  io 3.2%   of the last 10 s");

    const auto bare = egraph::tui::pressure_lines(two_samples(false), {}, 4, ascii);
    CHECK(contains(joined(bare.at(4)), "psi=1"));
    CHECK_FALSE(contains(joined(bare.at(3)), "steve"));
    const auto empty = egraph::tui::pressure_lines(egraph::pressure::History{}, {}, 4, ascii);
    CHECK(joined(empty.at(1)) == " CPU                -");
}

TEST_CASE("the emerge view shows the pressure panel where it fits") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    const auto history = two_samples(false);
    app.finish_watch(running_emerges(), *history.latest());
    FakeScreen tall{16, 120, {}};
    egraph::tui::draw(tall, app, ascii);
    CHECK(tall.line(8) == std::string(120, ' '));
    CHECK(tall.line(9).starts_with(" System"));
    CHECK(tall.line(11).starts_with(" Memory"));
    CHECK(tall.line(13).starts_with(" Stalls"));
    CHECK(tall.line(14).starts_with(" Steve    not running"));
    // Room for the emerge and its three tasks above.
    CHECK(contains(tall.text(), "app-misc/baz-1"));
    FakeScreen short_screen{12, 120, {}};
    egraph::tui::draw(short_screen, app, ascii);
    CHECK_FALSE(contains(short_screen.text(), " System"));
}

namespace {

// steve as the dev box runs it, 7 of its 12 jobs handed out.
egraph::steve::Status live_steve() {
    return {.live = true,
            .settings = {.tokens = 5,
                         .jobs = 12,
                         .min_jobs = 1,
                         .load_average = 12,
                         .recheck_timeout = 0.5,
                         .min_memory = 4096}};
}

} // namespace

TEST_CASE("steve's limits mark the graphs") {
    const auto limits = egraph::tui::limits_of(live_steve());
    CHECK(limits.load == 12.0);
    CHECK(limits.min_available == 4096ULL << 20U);
    CHECK_FALSE(egraph::tui::limits_of(std::nullopt).load.has_value());
    auto unlimited = live_steve();
    unlimited.settings.load_average.reset();
    unlimited.settings.min_memory.reset();
    CHECK_FALSE(egraph::tui::limits_of(unlimited).min_available.has_value());
}

TEST_CASE("steve's line shows its jobs and settings, and which one is changing") {
    CHECK(joined(egraph::tui::steve_line(std::nullopt, std::nullopt, ascii)) ==
          " Steve    not running");
    CHECK(joined(egraph::tui::steve_line(live_steve(), std::nullopt, ascii)) ==
          " Steve    #######-----  7 of 12 jobs in use   jobs 12  min jobs 1  load 12  memory 4.0 "
          "GiB  per process -  recheck 0.5s");

    const auto editing = egraph::tui::steve_line(live_steve(), 2, ascii);
    CHECK(contains(joined(editing), "load  12   memory"));
    const auto load = std::ranges::find(editing, std::string{" 12 "}, &egraph::tui::Span::text);
    REQUIRE(load != editing.end());
    CHECK(load->pen.bg.has_value());

    auto read_only = live_steve();
    read_only.live = false;
    read_only.settings.tokens.reset();
    read_only.problem = "unable to open /dev/steve: Permission denied";
    CHECK(joined(egraph::tui::steve_line(read_only, std::nullopt, ascii)) ==
          " Steve    jobs 12  min jobs 1  load 12  memory 4.0 GiB  per process -  recheck 0.5s   "
          "from its command line (unable to open /dev/steve: Permission denied)");
}

TEST_CASE("s changes steve's settings in place, each step at once") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({}, {}, live_steve());
    app.handle(character(U's'));
    REQUIRE(app.watched()->editing == 0);
    app.handle(key(KeyKind::left));
    CHECK(app.watched()->editing == 0);
    app.handle(key(KeyKind::right));
    CHECK(app.watched()->editing == 1);
    // min-jobs up by one, asked of run() and not yet made.
    app.handle(character(U'+'));
    auto change = app.steve_change_requested();
    REQUIRE(change.has_value());
    CHECK(change->setting == egraph::steve::Setting::min_jobs);
    CHECK(change->value == 2.0);
    app.finish_steve_change({});
    CHECK_FALSE(app.steve_change_requested().has_value());
    // Read again at once, to show what steve made of it.
    CHECK(app.watch_requested());
    app.finish_watch({}, {}, live_steve());

    // Down arrows change the setting rather than move.
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::down));
    change = app.steve_change_requested();
    REQUIRE(change.has_value());
    CHECK(change->setting == egraph::steve::Setting::load_average);
    CHECK(change->value == 11.0);
    app.finish_steve_change(std::unexpected("ioctl failed: Invalid argument"));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "stevie could not change it");
    CHECK(app.dialog()->lines == std::vector<std::string>{"ioctl failed: Invalid argument"});
    app.handle(key(KeyKind::escape));

    // A setting at its floor does not move.
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::right));
    app.handle(key(KeyKind::left));
    CHECK(app.watched()->editing == 3);
    auto floor = live_steve();
    floor.settings.min_memory.reset();
    app.finish_watch({}, {}, floor);
    app.handle(character(U'-'));
    CHECK_FALSE(app.steve_change_requested().has_value());

    // Esc and s leave the settings, not the view.
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.watched()->editing.has_value());
    CHECK(app.watched().has_value());
    app.handle(character(U's'));
    app.handle(character(U's'));
    CHECK_FALSE(app.watched()->editing.has_value());
}

TEST_CASE("steve read from its command line, or not running, cannot be changed") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    auto read_only = live_steve();
    read_only.live = false;
    read_only.problem = "unable to open /dev/steve: Permission denied";
    app.finish_watch({}, {}, read_only);
    app.handle(character(U's'));
    CHECK_FALSE(app.watched()->editing.has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "steve's settings can only be read here");
    CHECK(app.dialog()->lines.front() == read_only.problem);
    app.handle(key(KeyKind::escape));

    app.handle(key(KeyKind::tick));
    app.finish_watch({}, {}, std::nullopt);
    app.handle(character(U's'));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "steve is not running");
}

TEST_CASE("run makes steve's changes through stevie and reads the result") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{20, 140, {character(U'e'), character(U's'), character(U'+')}};
    auto steve = live_steve();
    std::vector<std::pair<egraph::steve::Setting, double>> changes;
    const egraph::tui::SteveReader read = [&] { return std::optional{steve}; };
    const egraph::tui::SteveSetter set = [&](egraph::steve::Setting setting,
                                             double value) -> std::expected<void, std::string> {
        changes.emplace_back(setting, value);
        steve.settings.jobs = static_cast<std::int64_t>(value);
        return {};
    };
    egraph::tui::run(screen, app, ascii, {.steve = read, .set_steve = set});
    REQUIRE(changes.size() == 1);
    CHECK(changes.front() == std::pair{egraph::steve::Setting::jobs, 13.0});
    CHECK(contains(screen.text(), " 13 "));
    CHECK(contains(screen.line(19), "+/- change"));
}

namespace {

// lib is building; app waits for it and plugin for app; doc is a binary package waiting for
// nothing. x/other-1 runs but is not on the list.
egraph::emerge::Snapshot building_lib() {
    return {.pid = 4321,
            .timestamp = 0,
            .jobs = {.running = 2, .max = 4, .completed = 1, .total = 5},
            .tasks = {{.cpv = "x/other-1", .phase = "compile"},
                      {.cpv = "dev-libs/lib-1", .phase = "configure"}}};
}

std::vector<egraph::emerge::Pending> lib_merge_list() {
    return {{.kind = "ebuild", .root = "/", .cpv = "dev-libs/lib-1"},
            {.kind = "ebuild", .root = "/", .cpv = "app-misc/app-1"},
            {.kind = "ebuild", .root = "/", .cpv = "app-misc/plugin-1"},
            {.kind = "binary", .root = "/", .cpv = "app-doc/doc-1"}};
}

const egraph::emerge::Waits lib_waits{{"dev-libs/lib-1", {}},
                                      {"app-misc/app-1", {"dev-libs/lib-1"}},
                                      {"app-misc/plugin-1", {"app-misc/app-1"}},
                                      {"app-doc/doc-1", {}}};

} // namespace

TEST_CASE("the merge list hangs under the emerge that runs it, what is running on top") {
    egraph::tui::Watched watched;
    watched.snapshots = {{.pid = 99, .timestamp = 0, .tasks = {{.cpv = "y/elsewhere-1"}}},
                         building_lib()};
    watched.merge_list = lib_merge_list();
    watched.waits = lib_waits;
    CHECK(egraph::tui::rows_owner(watched) == 1);
    const auto rows = egraph::tui::watch_rows(watched);
    std::vector<std::tuple<std::size_t, std::string, std::size_t, bool>> shape;
    for (const auto& row : rows) {
        shape.emplace_back(row.snapshot, row.cpv, row.depth, row.task.has_value());
    }
    CHECK(shape == std::vector<std::tuple<std::size_t, std::string, std::size_t, bool>>{
                       {0, "y/elsewhere-1", 1, true},
                       {1, "x/other-1", 1, true},
                       {1, "dev-libs/lib-1", 1, true},
                       {1, "app-misc/app-1", 2, false},
                       {1, "app-misc/plugin-1", 3, false},
                       {1, "app-doc/doc-1", 1, false}});
    CHECK(rows.at(0).last);
    CHECK_FALSE(rows.at(2).last);
    CHECK(rows.at(3).waiting_for == 1);
    CHECK(rows.at(5).binary);
    CHECK(rows.at(5).last);
    // The top level's line passes app and plugin on its way to doc.
    CHECK(rows.at(4).rails == std::vector<bool>{true, false});

    // Without the list, only what runs.
    watched.merge_list.clear();
    CHECK(egraph::tui::watch_rows(watched).size() == 3);
}

TEST_CASE("egraph exec is named as such, and never takes emerge's merge list") {
    egraph::tui::Watched watched;
    auto run = building_lib();
    run.publisher = egraph::emerge::Publisher::exec;
    watched.snapshots = {run};
    watched.merge_list = lib_merge_list();
    watched.waits = lib_waits;
    CHECK(egraph::tui::rows_owner(watched) == 1);
    CHECK(std::ranges::none_of(egraph::tui::watch_rows(watched),
                               [](const auto& row) { return !row.task.has_value(); }));
    const auto spans = egraph::tui::emerge_spans(run, ascii);
    CHECK(spans.front().text == " * egraph exec 4321");
    // Beside an emerge between tasks, the list is the emerge's.
    watched.snapshots = {{.pid = 99, .timestamp = 0, .tasks = {}}, run};
    CHECK(egraph::tui::rows_owner(watched) == 0);
}
TEST_CASE("the emerge view draws the merge list as a tree that says what can start") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({building_lib()}, {}, std::nullopt, lib_merge_list());
    REQUIRE(app.plan_requested().has_value());
    app.finish_plan(lib_waits);
    FakeScreen screen{12, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    CHECK(contains(text, "  |- / b x/other-1"));
    CHECK(contains(text, "  |- / b dev-libs/lib-1"));
    CHECK(contains(text, "  | `-   o app-misc/app-1"));
    CHECK(contains(text, "waits for 1"));
    CHECK(contains(text, "  |   `-   o app-misc/plugin-1"));
    CHECK(contains(text, "  `-   p app-doc/doc-1"));
    CHECK(contains(text, "ready"));
}

TEST_CASE("the merge list is planned once, and a failure is not asked again") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    app.handle(character(U'e'));
    app.finish_watch({building_lib()}, {}, std::nullopt, lib_merge_list());
    const auto asked = app.plan_requested();
    REQUIRE(asked.has_value());
    CHECK(asked->size() == 4);
    app.finish_plan(std::unexpected("no ebuild for dev-libs/lib-1"));
    CHECK_FALSE(app.plan_requested().has_value());
    FakeScreen screen{12, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(),
                   "! what the merge list waits for is unknown: no ebuild for dev-libs/lib-1"));
    // A flat list meanwhile, every package at the top.
    CHECK(contains(screen.text(), "  |-   o app-misc/plugin-1"));

    // The list changes as packages merge: a smaller one is still asked, being new.
    auto shorter = lib_merge_list();
    shorter.erase(shorter.begin());
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()}, {}, std::nullopt, shorter);
    REQUIRE(app.plan_requested().has_value());
    app.finish_plan(lib_waits);
    CHECK_FALSE(app.watched()->plan_error.has_value());
    // Known now; what merges drops out without asking again.
    shorter.erase(shorter.begin());
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()}, {}, std::nullopt, shorter);
    CHECK_FALSE(app.plan_requested().has_value());
    // No list, no plan, and nothing kept of the last one.
    app.handle(key(KeyKind::tick));
    app.finish_watch({building_lib()});
    CHECK(app.watched()->waits.empty());
}

TEST_CASE("run reads the merge list each second and plans it once") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    FakeScreen screen{16, 120, {character(U'e'), key(KeyKind::tick), key(KeyKind::tick)}};
    int reads = 0;
    int plans = 0;
    egraph::tui::run(screen, app, ascii,
                     {.watch = [] { return std::vector{building_lib()}; },
                      .merge_list =
                          [&] {
                              ++reads;
                              return lib_merge_list();
                          },
                      .plan = [&](const std::vector<egraph::emerge::Pending>& list)
                          -> std::expected<egraph::emerge::Waits, std::string> {
                          ++plans;
                          CHECK(list.size() == 4);
                          return lib_waits;
                      }});
    CHECK(reads == 3);
    CHECK(plans == 1);
    CHECK(contains(screen.text(), "waits for 1"));
    // Enter on a package that is only on the list opens nothing installed, and says so.
    app.handle(key(KeyKind::end));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "No version of app-doc/doc is installed yet");
}

TEST_CASE("with the evaluated store, pages read dependencies as emerge does") {
    for (const bool dynamic_deps : {true, false}) {
        egraph::tui::App app{both(), dynamic_deps};
        CHECK(app.has_evaluated());
        FakeScreen screen{24, 100, {key(KeyKind::enter)}};
        egraph::tui::run(screen, app, ascii, {.check = no_check});
        REQUIRE(app.pages().size() == 1);
        const auto text = screen.text();
        CHECK(contains(text, "dev-libs/b:=") == dynamic_deps);
        CHECK(contains(text, "dev-libs/b |") != dynamic_deps);
        // The installed store stays as built, for the check.
        CHECK(app.installed().nodes_in(app.installed().packages.at(0).deps.at(4)).size() == 4);
    }
}

TEST_CASE("the list opens on pending updates, and u shows them among the rest") {
    egraph::tui::App app{only_b_updates(), true};
    CHECK(app.list().only == egraph::tui::Only::updates);
    CHECK(app.list().shown == std::vector<std::uint32_t>{1});
    FakeScreen screen{10, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(0), "1 updates"));
    CHECK(contains(screen.line(9), "u all"));
    app.handle(character(U'/'));
    app.handle(character(U'a'));
    app.handle(character(U'-'));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "no package matches"));
    app.handle(key(KeyKind::escape));
    app.handle(character(U'u'));
    CHECK(app.list().only == egraph::tui::Only::all);
    CHECK(app.list().shown.size() == 2);
    egraph::tui::draw(screen, app, ascii);
    CHECK_FALSE(contains(screen.line(3), "  U "));
    CHECK(contains(screen.line(4), "dev-libs/b-1  U 2"));
    CHECK(contains(screen.line(9), "u updates"));
}

TEST_CASE("the list says when nothing is pending") {
    egraph::test::Bytes dependencies;
    dependencies.varint(2);
    dependencies.varints({1, 0, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({1, 0, 0, 0}).list({}).varints({0, 0, 0, 0, 0, 0});
    dependencies.varints({2, 1, 3}).varint(0);
    dependencies.varint(0).varint(0).varint(0).varint(0).varint(0).varint(0);
    dependencies.varints({1, 0, 0, 0}).list({}).varints({0, 0, 0, 0, 0, 0});
    egraph::tui::App app{
        egraph::Stores{.installed = sample(), .evaluated = evaluated_sample(dependencies)}, true};
    FakeScreen screen{10, 120, {}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(app.list().shown.empty());
    CHECK(contains(screen.text(), "nothing to update"));
}

TEST_CASE("without an evaluated store there is nothing to update") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    CHECK_FALSE(app.has_evaluated());
    FakeScreen screen{10, 120, {character(U'u')}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    CHECK(app.list().only == egraph::tui::Only::all);
    CHECK_FALSE(contains(screen.line(9), "updates"));
}

TEST_CASE("a page shows its pending update and what toggled flags would add") {
    egraph::tui::App app{both(), true};
    FakeScreen screen{30, 120, {key(KeyKind::enter)}};
    egraph::tui::run(screen, app, ascii, {.check = no_check});
    auto text = screen.text();
    CHECK(contains(text, "Update"));
    CHECK(contains(text, "   R rebuild for flag* -new%  ::test_repo"));
    CHECK(contains(text, "Would depend on, with flags toggled  2"));
    CHECK(contains(text, "dev-libs/b  +flag"));
    CHECK(contains(text, "   ....B   dev-libs/gone |  +flag -minimal  not installed"));
    CHECK_FALSE(contains(text, "Would be needed by"));

    // Only what is installed can be opened: b-1, from the dependencies.
    app.handle(key(KeyKind::end));
    CHECK(app.pages().back().rows.at(app.pages().back().cursor.at).flags == "+flag");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(app.pages().back().package == 1);
    egraph::tui::draw(screen, app, ascii);
    text = screen.text();
    CHECK(contains(text, "   U upgrade to dev-libs/b-2  ::test_repo"));
    CHECK(contains(text, "Would be needed by, with flags toggled  1"));
    CHECK(contains(text, "app-misc/a-1"));
}

TEST_CASE("the check compares installed stores, and a preview keeps the evaluated one") {
    egraph::tui::App app{both(), true};
    std::size_t checked_nodes = 0;
    const egraph::tui::Checker check =
        checking([&](const egraph::Store& stored) -> egraph::tui::CheckResult {
            checked_nodes = stored.nodes_in(stored.packages.at(0).deps.at(4)).size();
            return egraph::tui::Fresh{
                .store = sample(), .evaluated = evaluated_sample(), .drift = {"~dev-libs/b-1"}};
        });
    FakeScreen screen{12, 120, {character(U'c'), character(U'u')}};
    egraph::tui::run(screen, app, ascii, {.check = check});
    CHECK(checked_nodes == 4);
    CHECK(app.source() == egraph::tui::Source::preview);
    CHECK(app.has_evaluated());
    CHECK(app.store().nodes_in(app.store().packages.at(0).deps.at(4)).size() == 1);
    CHECK(app.update_of(1).has_value());
}

namespace {

// Each character of text as a key.
std::deque<Key> typed(std::string_view text) {
    std::deque<Key> keys;
    for (const char c : text) {
        keys.push_back(character(static_cast<char32_t>(c)));
    }
    return keys;
}

} // namespace

TEST_CASE("a command typed at the prompt runs, and its output links to pages") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    auto keys = typed(":deps app-misc/a-1");
    keys.push_back(key(KeyKind::enter));
    std::vector<std::string> ran;
    const egraph::tui::Commander command = [&](const std::string& line) {
        ran.push_back(line);
        return egraph::tui::Answer{.exit = egraph::Exit::ok,
                                   .out =
                                       "app-misc/a-1\tRDEPEND\tdev-libs/b\tdev-libs/b-1\tany-of\n"
                                       "@selected\tapp-misc/a\n",
                                   .err = {}};
    };
    FakeScreen screen{12, 100, keys};
    egraph::tui::run(screen, app, ascii, {.command = command});
    CHECK(ran == std::vector<std::string>{"deps app-misc/a-1"});
    REQUIRE(app.output().has_value());
    CHECK(app.output()->rows.size() == 2);
    CHECK(app.output()->links == std::vector<std::optional<std::uint32_t>>{0, std::nullopt});
    const auto text = screen.text();
    CHECK(contains(screen.line(0), ":deps app-misc/a-1"));
    CHECK(contains(screen.line(0), "2 lines"));
    // Fields line up in columns.
    CHECK(contains(text, "app-misc/a-1  RDEPEND  dev-libs/b  dev-libs/b-1  any-of"));
    // Rows of another shape line up among themselves.
    CHECK(contains(text, "@selected  app-misc/a"));

    // Enter opens the linked package; Esc comes back to the output, then to the list.
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 0);
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.output().has_value());
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.output().has_value());
}

TEST_CASE("the prompt edits, draws and cancels") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& k : typed(":xy")) {
        app.handle(k);
    }
    app.handle(key(KeyKind::backspace));
    REQUIRE(app.prompt().has_value());
    CHECK(*app.prompt() == "x");
    FakeScreen screen{12, 60, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(screen.line(11).starts_with(":x"));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.prompt().has_value());
    CHECK_FALSE(app.command_requested().has_value());
    // An empty command runs nothing.
    app.handle(character(U':'));
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.prompt().has_value());
    CHECK_FALSE(app.command_requested().has_value());
    // While searching, : is part of the query.
    app.handle(character(U'/'));
    app.handle(character(U':'));
    CHECK_FALSE(app.prompt().has_value());
    CHECK(app.list().query == ":");
}

TEST_CASE("a command's errors and warnings show in a dialog") {
    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App app{store, graph};
    for (const auto& k : typed(":nope")) {
        app.handle(k);
    }
    app.handle(key(KeyKind::enter));
    REQUIRE(app.command_requested() == std::optional<std::string>{"nope"});
    app.finish_command({.exit = egraph::Exit::usage,
                        .out = {},
                        .err = "egraph: shell: nope: no such command (help lists them)\n"});
    CHECK_FALSE(app.command_requested().has_value());
    CHECK_FALSE(app.output().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->error);
    CHECK(app.dialog()->lines ==
          std::vector<std::string>{"egraph: shell: nope: no such command (help lists them)"});

    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'o'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok,
                        .out = "dev-libs/b-1\n",
                        .err = "egraph: warning: answering from a stale store (why)\n"});
    REQUIRE(app.output().has_value());
    REQUIRE(app.dialog().has_value());
    CHECK_FALSE(app.dialog()->error);

    // quit at the prompt ends the interface.
    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'q'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = {}, .err = {}, .quit = true});
    CHECK(app.done());

    // A command that prints nothing says so.
    app.handle(character(U'x'));
    app.handle(character(U':'));
    app.handle(character(U'o'));
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = {}, .err = {}});
    REQUIRE(app.output().has_value());
    CHECK(app.output()->rows.empty());
    FakeScreen screen{12, 60, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "the command printed nothing"));
}

TEST_CASE("held updates show in the list, and a held package's page offers its remedies") {
    // skin holds rgb, and only world keeps it; kwin holds lazy, and panel needs kwin.
    auto system = egraph::test::make_system(
        {{.cpv = "app-misc/kwin-1", .deps = {{"RDEPEND", "<dev-libs/lazy-2"}}},
         {.cpv = "app-misc/panel-1", .deps = {{"RDEPEND", "app-misc/kwin"}}},
         {.cpv = "app-misc/skin-1", .deps = {{"RDEPEND", "<dev-libs/rgb-2"}}},
         {.cpv = "dev-libs/lazy-1"},
         {.cpv = "dev-libs/rgb-1"},
         {.cpv = "dev-libs/up-1"}},
        {{.cpv = "dev-libs/lazy-1"},
         {.cpv = "dev-libs/lazy-2"},
         {.cpv = "dev-libs/rgb-1"},
         {.cpv = "dev-libs/rgb-2"},
         {.cpv = "dev-libs/up-1"},
         {.cpv = "dev-libs/up-2"}},
        {"app-misc/panel", "app-misc/skin"});
    egraph::tui::App app{egraph::Stores{.installed = std::move(system.store),
                                        .evaluated = std::move(system.evaluated)},
                         true};
    FakeScreen screen{30, 120, {}};
    CHECK(app.list().shown == std::vector<std::uint32_t>{3, 4, 5});
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "dev-libs/lazy-1  H held"));
    CHECK(contains(screen.text(), "dev-libs/up-1  U 2"));

    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(app.pages().back().package == 4);
    egraph::tui::draw(screen, app, ascii);
    auto text = screen.text();
    CHECK(contains(text, "Held back"));
    CHECK(contains(text, "   H upgrade to dev-libs/rgb-2  ::test_repo"));
    CHECK(contains(text, " > R....   app-misc/skin-1                           <dev-libs/rgb-2"));
    CHECK(contains(text, "nothing depends on it; only @selected keeps it"));
    CHECK(contains(text, "   to remove it: emerge --deselect app-misc/skin"));
    CHECK(contains(text, "                 emerge -C =app-misc/skin-1"));
    CHECK(contains(text, "                 emerge -1 =dev-libs/rgb-2"));
    CHECK(contains(text, "   to keep it:   emerge -1 --nodeps =dev-libs/rgb-2"));

    // The holder is a link to its own page.
    const auto& page = app.pages().back();
    const auto holder = std::ranges::find_if(page.rows, [](const egraph::tui::Row& row) {
        return row.type == RowType::link && row.link.package == 2;
    });
    REQUIRE(holder != page.rows.end());
    while (app.pages().back().cursor.at !=
           static_cast<std::size_t>(holder - app.pages().back().rows.begin())) {
        app.handle(key(KeyKind::down));
    }
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().back().package == 2);

    // A holder something else needs is only named.
    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::escape));
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().back().package == 3);
    egraph::tui::draw(screen, app, ascii);
    text = screen.text();
    CHECK(contains(text, "needed by app-misc/panel-1"));
    CHECK_FALSE(contains(text, "to remove"));
    CHECK(contains(text, "to keep it:"));
}

namespace {

std::shared_ptr<const egraph::Stores> shared(egraph::test::System system) {
    return std::make_shared<const egraph::Stores>(egraph::Stores{
        .installed = std::move(system.store), .evaluated = std::move(system.evaluated)});
}

// a/app-1 needing dev-libs/lib and x/other, lib at version.
egraph::test::System app_with_lib(const std::string& version, bool with_new = false) {
    std::vector<egraph::test::Installed> installed{
        {.cpv = "a/app-1", .deps = {{"RDEPEND", "dev-libs/lib x/other"}}},
        {.cpv = "dev-libs/lib-" + version}};
    if (with_new) {
        installed.push_back({.cpv = "x/new-1"});
    }
    installed.push_back({.cpv = "x/other-1"});
    std::vector<egraph::test::Available> available;
    for (const auto& pkg : installed) {
        available.push_back({.cpv = pkg.cpv});
    }
    return egraph::test::make_system(installed, available);
}

std::string cpv_of(const egraph::tui::App& app, std::uint32_t id) {
    return std::string{app.store().string(app.store().packages.at(id).cpv)};
}

// A clock that moves on by stale_interval with each key the screen reads.
egraph::tui::Clock ticking(const FakeScreen& screen) {
    return [&screen] {
        return std::chrono::steady_clock::time_point{} +
               egraph::tui::stale_interval * static_cast<int>(screen.timeouts.size());
    };
}

} // namespace

TEST_CASE("refreshed stores keep the list and pages where they were") {
    SECTION("the list's cursor stays on its package") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        app.handle(character(U'u'));
        app.handle(key(KeyKind::down));
        app.handle(key(KeyKind::down));
        REQUIRE(cpv_of(app, app.list().shown.at(app.list().cursor.at)) == "x/other-1");
        app.finish_stale_check("x/new-1 was installed");
        CHECK(app.refresh_requested());
        app.finish_refresh(shared(app_with_lib("1", true)));
        CHECK_FALSE(app.stale());
        CHECK(app.store().packages.size() == 4);
        CHECK(app.list().only == egraph::tui::Only::all);
        CHECK(app.list().cursor.at == 3);
        CHECK(cpv_of(app, app.list().shown.at(app.list().cursor.at)) == "x/other-1");
    }
    SECTION("a page stays open, on the row it was on, across an upgrade") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        app.handle(character(U'u'));
        app.handle(key(KeyKind::enter));
        REQUIRE(app.pages().size() == 1);
        const auto& before = app.pages().back();
        REQUIRE(before.rows.at(before.cursor.at).type == RowType::link);
        REQUIRE(cpv_of(app, before.rows.at(before.cursor.at).link.package) == "dev-libs/lib-1");
        app.handle(key(KeyKind::enter));
        REQUIRE(app.pages().size() == 2);

        app.finish_refresh(shared(app_with_lib("2")));
        REQUIRE(app.pages().size() == 2);
        const auto& page = app.pages().front();
        CHECK(cpv_of(app, page.package) == "a/app-1");
        CHECK(page.rows.at(page.cursor.at).type == RowType::link);
        CHECK(cpv_of(app, page.rows.at(page.cursor.at).link.package) == "dev-libs/lib-2");
        // The upgraded package's own page follows it to the new version.
        CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
    }
}

TEST_CASE("the stores' inputs are looked at while idle, and refreshed in the background") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{
        12, 120, {key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick)}};
    int looks = 0;
    int refreshes = 0;
    std::string refreshing;
    const egraph::tui::Services services{
        .stale = [&](const egraph::Stores& stores) -> std::optional<std::string> {
            CHECK(&stores == app.shared().get());
            return ++looks == 1 ? std::optional<std::string>{"x/new-1 was installed"}
                                : std::nullopt;
        },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            return [&, polls = 0]() mutable -> std::optional<egraph::tui::RefreshResult> {
                if (++polls < 2) {
                    return std::nullopt;
                }
                refreshing = screen.line(0);
                return shared(app_with_lib("1", true));
            };
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(looks >= 2);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 4);
    CHECK(contains(refreshing, "/ refreshing"));
    CHECK_FALSE(contains(screen.line(0), "refreshing"));
    REQUIRE(screen.timeouts.size() >= 2);
    // Idle, it wakes when the next look is due; refreshing, it polls the build.
    CHECK(screen.timeouts.at(0) == egraph::tui::stale_interval);
    CHECK(screen.timeouts.at(1) == egraph::tui::wait_interval);
}

TEST_CASE("a failed refresh says why and waits before trying again") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12, 120, {key(KeyKind::tick), key(KeyKind::tick)}};
    int refreshes = 0;
    const egraph::tui::Services services{
        .stale = [](const egraph::Stores&) -> std::optional<std::string> { return "changed"; },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            return egraph::ready(egraph::tui::RefreshResult{
                std::unexpected("egraph-build exited with status 1:\nTraceback")});
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 3);
    REQUIRE(app.refresh_error().has_value());
    INFO(screen.line(0));
    CHECK(screen.line(0).ends_with(" refresh failed: egraph-build exited with status 1 "));
    CHECK(std::ranges::find(screen.timeouts, egraph::tui::retry_interval) != screen.timeouts.end());
}

TEST_CASE("no refresh runs while the check view is open") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12,
                      120,
                      {character(U'c'), key(KeyKind::tick), key(KeyKind::tick), key(KeyKind::tick),
                       key(KeyKind::escape), key(KeyKind::tick)}};
    int refreshes = 0;
    const egraph::tui::Services services{
        .check = cyclic_check,
        .stale = [&](const egraph::Stores&) -> std::optional<std::string> {
            return refreshes == 0 ? std::optional<std::string>{"changed"} : std::nullopt;
        },
        .refresh = [&]() -> egraph::Job<egraph::tui::RefreshResult> {
            ++refreshes;
            CHECK_FALSE(app.checked().has_value());
            return egraph::ready(egraph::tui::RefreshResult{shared(app_with_lib("1", true))});
        },
        .now = ticking(screen)};
    egraph::tui::run(screen, app, ascii, services);
    CHECK(refreshes == 1);
    CHECK(app.store().packages.size() == 4);
}

namespace {

// top needs glibmm, whose next version pulls in mm-common and through it chain; only world keeps
// top, and nothing keeps loose, installed at loose_version.
egraph::test::System glibmm_system(const std::string& loose_version = "1") {
    return egraph::test::make_system(
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/loose-" + loose_version},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/glibmm-2", .deps = {{"BDEPEND", "dev-cpp/mm-common"}}},
         {.cpv = "app-misc/loose-1"},
         {.cpv = "app-misc/loose-2"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}},
         {.cpv = "dev-cpp/mm-common-1", .deps = {{"RDEPEND", "dev-libs/chain"}}, .iuse = "doc"},
         {.cpv = "dev-libs/chain-1"}},
        {"app-misc/top"});
}

// Each row as its depth, label, and the merge's candidate cpv where it is one.
std::vector<std::string> described(const egraph::tui::App& app) {
    std::vector<std::string> out;
    for (const auto& row : app.planned()->rows) {
        auto line = std::format("{} {}", row.depth, row.label);
        if (row.merge) {
            const auto& evaluated = app.evaluated();
            line += std::format(
                " -> {}",
                evaluated.string(
                    evaluated.candidates.at(app.plan().merges.at(*row.merge).candidate).cpv));
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::string selected_label(const egraph::tui::App& app) {
    const auto& planned = *app.planned();
    return planned.rows.at(planned.cursor.at).label;
}

} // namespace

TEST_CASE("p shows the plan as a tree under each root set") {
    egraph::tui::App app{shared(glibmm_system()), true};
    FakeScreen screen{16, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.line(15), "p plan"));
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    CHECK(described(app) == std::vector<std::string>{
                                "0 ",
                                "1 app-misc/loose-1 -> app-misc/loose-2",
                                "0 @selected",
                                "1 app-misc/top-1",
                                "2 app-misc/glibmm-1 -> app-misc/glibmm-2",
                                "3 dev-cpp/mm-common-1 -> dev-cpp/mm-common-1",
                                "4 dev-libs/chain-1 -> dev-libs/chain-1",
                            });
    CHECK(selected_label(app) == "app-misc/loose-1");
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(screen.line(0), "plan  4 merges"));
    CHECK(contains(text, "\n   - nothing keeps"));
    CHECK(contains(text, "\n > `- U app-misc/loose  1 > 2  ::test_repo  1     "));
    CHECK(contains(text, "\n   @ @selected"));
    CHECK(contains(text, "\n   `- app-misc/top-1"));
    CHECK(contains(text, "\n     `- U app-misc/glibmm  1 > 2  ::test_repo  4  w 3"));
    CHECK(contains(text, "\n       `- N dev-cpp/mm-common  1  ::test_repo  3  w 2"));
    CHECK(contains(text, "\n         `- N dev-libs/chain  1  ::test_repo  2"));
    CHECK(contains(screen.line(15), "esc back"));
}

TEST_CASE("the plan view names a new slot's installed neighbours") {
    egraph::tui::App app{shared(egraph::test::make_system(
                             {{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "dev-lang/py:1"}}},
                              {.cpv = "dev-lang/py-1", .slot = "1"}},
                             {{.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "dev-lang/py:1"}}},
                              {.cpv = "app-misc/top-2", .deps = {{"RDEPEND", "dev-lang/py:2"}}},
                              {.cpv = "dev-lang/py-1", .slot = "1"},
                              {.cpv = "dev-lang/py-2", .slot = "2"}},
                             {"app-misc/top"})),
                         true};
    FakeScreen screen{16, 120, {}};
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "N dev-lang/py  2  ::test_repo  1  beside 1:1"));
}

TEST_CASE("the plan view moves over packages and opens the installed ones") {
    egraph::tui::App app{shared(glibmm_system()), true};
    app.handle(character(U'p'));
    // Past the set to its first package.
    app.handle(key(KeyKind::down));
    CHECK(selected_label(app) == "app-misc/top-1");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "app-misc/top-1");
    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    REQUIRE(app.planned().has_value());

    // A replacement opens the page of what it replaces.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "app-misc/glibmm-1");
    app.handle(key(KeyKind::escape));

    // A new package has no page, only what pulls it in and its USE.
    app.handle(key(KeyKind::down));
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.dialog().has_value());
    CHECK(app.dialog()->title == "dev-cpp/mm-common-1 is not installed");
    CHECK(
        app.dialog()->lines ==
        std::vector<std::string>{"The plan pulls it in for app-misc/glibmm-2's dev-cpp/mm-common.",
                                 "It would be built with USE=\"-doc\"."});
    app.handle(key(KeyKind::escape));

    app.handle(key(KeyKind::home));
    CHECK(selected_label(app) == "app-misc/loose-1");
    app.handle(key(KeyKind::end));
    CHECK(selected_label(app) == "dev-libs/chain-1");
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.planned().has_value());
    CHECK_FALSE(app.done());
    app.handle(character(U'p'));
    app.handle(character(U'q'));
    CHECK(app.done());
}

TEST_CASE("the plan view says when nothing is to merge, and needs the evaluated store") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U'p'));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.empty());
    FakeScreen screen{10, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "nothing to merge"));

    const auto store = sample();
    const auto graph = egraph::build_graph(store);
    egraph::tui::App bare{store, graph};
    bare.handle(character(U'p'));
    CHECK_FALSE(bare.planned().has_value());
}

TEST_CASE("refreshed stores keep the plan view on its package") {
    egraph::tui::App app{shared(glibmm_system()), true};
    app.handle(character(U'p'));
    app.handle(key(KeyKind::down));
    REQUIRE(selected_label(app) == "app-misc/top-1");
    app.finish_refresh(shared(glibmm_system("2")));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.size() == 5);
    CHECK(selected_label(app) == "app-misc/top-1");

    // What it was on is gone: the same place, on a package.
    app.handle(key(KeyKind::end));
    app.finish_refresh(shared(egraph::test::make_system(
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {{.cpv = "app-misc/glibmm-1"},
         {.cpv = "app-misc/glibmm-2"},
         {.cpv = "app-misc/top-1", .deps = {{"RDEPEND", "app-misc/glibmm"}}}},
        {"app-misc/top"})));
    REQUIRE(app.planned().has_value());
    CHECK(app.planned()->rows.size() == 3);
    CHECK(selected_label(app) == "app-misc/glibmm-1");
}

namespace {

// dev-libs/lib at 1 (installed with app_with_lib("1")), 2 and ~x86 3; app-misc/new-tool at 1.0
// and ~x86 2.0, not installed.
std::shared_ptr<const egraph::RepositoryIndex> repository_index() {
    egraph::test::IndexBuilder b{{"test_repo", "overlay"}};
    for (const auto* version : {"1", "2"}) {
        b.version({.cpv = std::format("dev-libs/lib-{}", version),
                   .repo = "test_repo",
                   .description = "A library"});
    }
    b.version({.cpv = "dev-libs/lib-3", .keywords = "~x86", .repo = "test_repo"});
    b.version({.cpv = "app-misc/new-tool-1.0",
               .license = "MIT",
               .repo = "overlay",
               .description = "A new tool",
               .homepage = "https://example.org"});
    b.version({.cpv = "app-misc/new-tool-2.0", .keywords = "~x86", .repo = "overlay"});
    return std::make_shared<const egraph::RepositoryIndex>(b.index());
}

void type(egraph::tui::App& app, std::string_view text) {
    for (const char c : text) {
        app.handle(character(static_cast<char32_t>(c)));
    }
}

std::vector<std::string> result_cps(const egraph::tui::App& app) {
    std::vector<std::string> cps;
    for (const auto& found : app.search()->results) {
        cps.push_back(found.cp);
    }
    return cps;
}

} // namespace

TEST_CASE("s searches the repositories, once the index is loaded") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    REQUIRE(app.search().has_value());
    CHECK(app.search()->typing);
    CHECK(app.index_requested());
    // : is part of the key while typing.
    type(app, "tool:");
    CHECK_FALSE(app.prompt().has_value());
    app.handle(key(KeyKind::backspace));
    app.handle(key(KeyKind::enter));
    CHECK_FALSE(app.search()->typing);
    CHECK(app.search()->pending);
    CHECK(app.search()->results.empty());

    app.finish_index(repository_index());
    CHECK_FALSE(app.index_requested());
    REQUIRE(app.catalogue().has_value());
    CHECK_FALSE(app.search()->pending);
    CHECK(app.search()->ran == "tool");
    REQUIRE(result_cps(app) == std::vector<std::string>{"app-misc/new-tool"});
    const auto& found = app.search()->results.front();
    CHECK(found.version == "1.0");
    CHECK(found.visible);
    CHECK(found.installed.empty());
    CHECK(found.description == "A new tool");

    // A new search, with the index already there.
    app.handle(character(U'/'));
    CHECK(app.search()->typing);
    for (int i = 0; i < 4; ++i) {
        app.handle(key(KeyKind::backspace));
    }
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    CHECK(app.search()->results.front().installed == "1");
    CHECK(app.search()->results.front().version == "2");
}

TEST_CASE("tab searches descriptions too, and esc before any search closes it") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.search().has_value());

    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "library");
    app.handle(key(KeyKind::enter));
    CHECK(app.search()->results.empty());
    app.handle(key(KeyKind::tab));
    CHECK(app.search()->descriptions);
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    // Esc after a search stops typing but keeps the results; again, back to the list.
    app.handle(character(U'/'));
    app.handle(key(KeyKind::escape));
    CHECK(app.search().has_value());
    CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.search().has_value());
}

TEST_CASE("an installed result opens its page, with the versions in the repositories") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    // Drawn first, as run() does, so the page knows its height.
    FakeScreen screen{30, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-1");
    std::vector<std::string> versions;
    for (const auto& row : app.pages().back().rows) {
        if (row.type == RowType::version) {
            REQUIRE(row.version.has_value());
            versions.push_back(row.version->version);
        }
    }
    CHECK(versions == std::vector<std::string>{"1", "2", "3"});

    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "Versions  3"));
    CHECK(contains(text, "::test_repo"));
    CHECK(contains(text, "masked: ~x86 keyword"));

    // Last on the page, after the dependencies, and the cursor reaches them.
    const auto& rows = app.pages().back().rows;
    CHECK(rows.back().type == RowType::version);
    CHECK(rows.at(app.pages().back().cursor.at).type == RowType::link);
    app.handle(character(U'G'));
    CHECK(app.pages().back().cursor.at == rows.size() - 1);
    egraph::tui::draw(screen, app, ascii);
    // The cursor on the last version, 3.
    CHECK(contains(screen.text(), ">   3 "));
    app.handle(key(KeyKind::up));
    app.handle(key(KeyKind::up));
    CHECK(rows.at(app.pages().back().cursor.at).version->version == "1");
    // This very version: nothing to open.
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().size() == 1);

    app.handle(key(KeyKind::escape));
    CHECK(app.pages().empty());
    CHECK(app.search().has_value());
}

TEST_CASE("enter on another installed version opens its page") {
    // dev-libs/lib in two slots, both installed.
    auto system = egraph::test::make_system(
        {{.cpv = "dev-libs/lib-1", .slot = "1"}, {.cpv = "dev-libs/lib-2", .slot = "2"}},
        {{.cpv = "dev-libs/lib-1", .slot = "1"}, {.cpv = "dev-libs/lib-2", .slot = "2"}});
    egraph::tui::App app{shared(std::move(system)), false};
    egraph::test::IndexBuilder b{{"test_repo"}};
    b.version({.cpv = "dev-libs/lib-1", .slot = "1", .repo = "test_repo"});
    b.version({.cpv = "dev-libs/lib-2", .slot = "2", .repo = "test_repo"});
    app.finish_index(std::make_shared<const egraph::RepositoryIndex>(b.index()));
    FakeScreen screen{30, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(character(U'u'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    REQUIRE(cpv_of(app, app.pages().back().package) == "dev-libs/lib-1");
    app.handle(character(U'G'));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 2);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
}

TEST_CASE("a result not installed opens a listing of its ebuild and versions") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    FakeScreen screen{20, 120, {}};
    egraph::tui::draw(screen, app, ascii);
    app.handle(key(KeyKind::enter));
    CHECK(app.pages().empty());
    REQUIRE(app.listing().has_value());
    const auto& listing = *app.listing();
    CHECK(listing.found.cp == "app-misc/new-tool");
    std::size_t versions = 0;
    for (const auto& row : listing.rows) {
        versions += row.type == RowType::version ? 1 : 0;
    }
    CHECK(versions == 2);
    REQUIRE(listing.cursor.at < listing.rows.size());
    CHECK(listing.rows.at(listing.cursor.at).type == RowType::version);

    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "search > new-tool"));
    CHECK(contains(text, "app-misc/new-tool  not installed"));
    CHECK(contains(text, "A new tool"));
    CHECK(contains(text, "https://example.org"));
    CHECK(contains(text, "MIT"));
    CHECK(contains(text, "::overlay"));
    CHECK(contains(text, "masked: ~x86 keyword"));

    // On the version the search shows, the best visible one.
    CHECK(listing.rows.at(listing.cursor.at).version->version == "1.0");
    app.handle(key(KeyKind::down));
    CHECK(listing.rows.at(listing.cursor.at).version->version == "2.0");
    app.handle(key(KeyKind::escape));
    CHECK_FALSE(app.listing().has_value());
    CHECK(app.search().has_value());
}

TEST_CASE("the search view shows its results, and a wait while the index loads") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    FakeScreen screen{12, 120, {}};
    app.handle(character(U's'));
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    egraph::tui::draw(screen, app, ascii);
    CHECK(contains(screen.text(), "Loading the repository index"));
    app.finish_index(repository_index());
    egraph::tui::draw(screen, app, ascii);
    const auto text = screen.text();
    INFO(text);
    CHECK(contains(text, "search  1 found"));
    CHECK(contains(text, "app-misc/new-tool"));
    CHECK(contains(text, "1.0"));
    CHECK(contains(text, "A new tool"));
}

TEST_CASE("run loads the index in the background, and a failure says why") {
    SECTION("loaded") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        FakeScreen screen{12,
                          120,
                          {character(U's'), character(U'l'), character(U'i'), character(U'b'),
                           key(KeyKind::enter), key(KeyKind::tick), key(KeyKind::tick)}};
        int loads = 0;
        std::string waiting;
        const egraph::tui::Services services{
            .load_index = [&]() -> egraph::Job<egraph::tui::IndexResult> {
                ++loads;
                return [&, polls = 0]() mutable -> std::optional<egraph::tui::IndexResult> {
                    if (++polls < 3) {
                        return std::nullopt;
                    }
                    waiting = screen.text();
                    return repository_index();
                };
            }};
        egraph::tui::run(screen, app, ascii, services);
        CHECK(loads == 1);
        CHECK(contains(waiting, "Loading the repository index"));
        REQUIRE(app.search().has_value());
        CHECK(result_cps(app) == std::vector<std::string>{"dev-libs/lib"});
        CHECK(std::ranges::find(screen.timeouts, egraph::tui::wait_interval) !=
              screen.timeouts.end());
    }
    SECTION("failed") {
        egraph::tui::App app{shared(app_with_lib("1")), false};
        FakeScreen screen{12, 120, {character(U's'), key(KeyKind::tick)}};
        int loads = 0;
        const egraph::tui::Services services{
            .load_index = [&]() -> egraph::Job<egraph::tui::IndexResult> {
                ++loads;
                return egraph::ready(
                    egraph::tui::IndexResult{std::unexpected("egraph-build exited with status 1")});
            }};
        egraph::tui::run(screen, app, ascii, services);
        CHECK(loads == 1);
        CHECK_FALSE(app.index_requested());
        REQUIRE(app.dialog().has_value());
        CHECK(app.dialog()->title == "The repository index could not be loaded");
        // Opening the search again tries again.
        app.handle(key(KeyKind::escape));
        app.handle(key(KeyKind::escape));
        app.handle(character(U's'));
        CHECK(app.index_requested());
    }
}

TEST_CASE("refreshed stores keep the search, its results now as installed") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "lib");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.search()->results.front().installed == "1");
    app.finish_refresh(shared(app_with_lib("2")));
    REQUIRE(app.search().has_value());
    CHECK(app.search()->results.front().installed == "2");
    app.handle(key(KeyKind::enter));
    REQUIRE(app.pages().size() == 1);
    CHECK(cpv_of(app, app.pages().back().package) == "dev-libs/lib-2");
}

TEST_CASE("a command's output from the search replaces a listing over it") {
    egraph::tui::App app{shared(app_with_lib("1")), false};
    app.handle(character(U's'));
    app.finish_index(repository_index());
    type(app, "tool");
    app.handle(key(KeyKind::enter));
    app.handle(key(KeyKind::enter));
    REQUIRE(app.listing().has_value());
    app.handle(character(U':'));
    type(app, "orphans");
    app.handle(key(KeyKind::enter));
    app.finish_command({.exit = egraph::Exit::ok, .out = "x/other-1\n", .err = {}, .quit = false});
    CHECK_FALSE(app.listing().has_value());
    REQUIRE(app.output().has_value());
    app.handle(key(KeyKind::escape));
    CHECK(app.search().has_value());
}

TEST_CASE("an action's command: exec for merges, remove for removals") {
    using egraph::tui::Action;
    using Words = std::vector<std::string>;
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::install, .targets = {"a/b"}}) ==
          Words{"exec", "a/b"});
    CHECK(egraph::tui::action_arguments(
              {.kind = Action::Kind::update, .targets = {"a/b:0", "c/d:2"}}) ==
          Words{"exec", "--oneshot", "-u", "-N", "a/b:0", "c/d:2"});
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::update}) ==
          Words{"exec", "--oneshot", "-u", "-N", "-D", "@installed"});
    CHECK(egraph::tui::action_arguments({.kind = Action::Kind::remove, .targets = {"=a/b-1"}}) ==
          Words{"remove", "=a/b-1"});
    CHECK(egraph::tui::action_arguments(
              {.kind = Action::Kind::remove, .targets = {"=a/b-1"}, .build_deps = false}) ==
          Words{"remove", "--with-bdeps", "n", "=a/b-1"});
}

TEST_CASE("a log's tail is what a terminal would leave of its last lines") {
    using Lines = std::vector<std::string>;
    CHECK(egraph::tui::plain_tail("a\nb\nc\nd\n", 2) == Lines{"c", "d"});
    CHECK(egraph::tui::plain_tail("a\nb", 5) == Lines{"a", "b"});
    // Colours, titles, progress redrawn over itself, and the blank lines at the end.
    CHECK(egraph::tui::plain_tail("\x1b[32;01m * \x1b[39;49;00mok\r\n\x1b]0;title\a10%\r50%\r"
                                  "100%\n\x1b]2;t\x1b\\x\ty\x07\n\n  \n",
                                  10) == Lines{" * ok", "100%", "x       y"});
    CHECK(egraph::tui::plain_tail("", 3).empty());
    CHECK(egraph::tui::plain_tail("cut off \x1b[", 3) == Lines{"cut off "});
}
