#pragma once

// The elog messages emerge's save_summary module appends to its summary log.

#include <string>
#include <string_view>
#include <vector>

namespace egraph {

// One package's messages of one class from one phase, as the log groups them.
struct ElogMessage {
    // INFO, LOG, WARN, ERROR or QA.
    std::string type;
    std::string phase;
    std::vector<std::string> lines;
};

struct ElogEntry {
    std::string package;
    std::vector<ElogMessage> messages;
};

// The entries of a summary log, or of the part of one written since an earlier read, in the
// order written. Text before the first entry's header is left out.
[[nodiscard]] std::vector<ElogEntry> parse_elog_summary(std::string_view text);

// "cpv<TAB>elog<TAB>type<TAB>phase<TAB>line" for each line of each message, in order.
[[nodiscard]] std::vector<std::string> elog_lines(const std::vector<ElogEntry>& entries);

} // namespace egraph
