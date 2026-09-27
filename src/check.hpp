#pragma once

#include "store.hpp"

#include <string>
#include <vector>

namespace egraph {

// Packages that differ between two stores, one line each: "+cpv" only in fresh, "-cpv" only in
// stored, "~cpv" in both but different. Inputs and meta are not compared.
[[nodiscard]] std::vector<std::string> drift(const Store& stored, const Store& fresh);

} // namespace egraph
