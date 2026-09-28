#include "tui.hpp"

#include "build_info.hpp"
#include "graph.hpp"

#include <ostream>

namespace egraph::tui {

bool available() {
    return EGRAPH_HAVE_TUI != 0;
}

Summary summarize(const Store& store) {
    return {.eroot = store.meta.eroot,
            .packages = store.packages.size(),
            .edges = build_graph(store).forward.size(),
            .root_atoms = store.roots.size()};
}

Exit open_and_run(const Store& store, GlyphSet glyphs, std::ostream& err) {
#if EGRAPH_HAVE_TUI
    auto screen = Screen::open();
    if (!screen) {
        err << "egraph: tui: " << screen.error() << '\n';
        return Exit::failure;
    }
    run(*screen, summarize(store), egraph::glyphs(glyphs));
    return Exit::ok;
#else
    (void)store;
    (void)glyphs;
    err << "egraph: tui: this egraph was built without Notcurses (meson -Dtui=enabled)\n";
    return Exit::not_implemented;
#endif
}

} // namespace egraph::tui
