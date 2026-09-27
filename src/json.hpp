#pragma once

#include "store.hpp"

#include <iosfwd>
#include <string>
#include <string_view>

namespace egraph {

// Writes bytes as a JSON string spelled exactly as Python's json.dumps spells
// bytes.decode("utf-8", "surrogateescape"), so both tools export identical bytes.
void write_json_string(std::ostream& out, std::string_view bytes);

// One package's object in write_json's output.
[[nodiscard]] std::string package_json(const Store& store, const Package& pkg);

// The store's packages as egraph_build.installed.to_json writes them.
void write_json(std::ostream& out, const Store& store);

} // namespace egraph
