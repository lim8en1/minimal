#include "cost_command.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <vector>

#include "minimal/commands/command_context.hpp"

namespace minimal::cost_tracker {
namespace {

// "$X" when every turn of the row was priced; ">= $X" when the priced turns
// sum to something but others could not be priced (the total is a floor);
// "?" when nothing at all was priced. A missing price is never "$0.0000" --
// the cost_view status segment's "cost?" rule, same honesty, table shape.
std::string CostCell(double cost, bool known) {
    if (known) return FormatCost(cost);
    if (cost > 0.0) return ">= " + FormatCost(cost);
    return "?";
}

struct Column {
    std::string header;
    std::vector<std::string> cells;
    bool right_aligned;
};

std::size_t Width(const Column& column) {
    std::size_t width = column.header.size();
    for (const auto& cell : column.cells) width = std::max(width, cell.size());
    return width;
}

// Column widths come from the CONTENT -- a fixed-width model column let one
// long model id push every later column out of alignment.
std::string FormatTable(const std::vector<Column>& columns) {
    std::vector<std::size_t> widths;
    widths.reserve(columns.size());
    for (const auto& column : columns) widths.push_back(Width(column));

    constexpr std::size_t kHeader = static_cast<std::size_t>(-1);
    std::ostringstream out;
    const auto emit_row = [&](const std::size_t row) {
        out << "  ";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) out << "  ";
            const std::string& text =
                row < columns[i].cells.size() ? columns[i].cells[row] : columns[i].header;
            if (columns[i].right_aligned) {
                out << std::setw(static_cast<int>(widths[i])) << std::right << text;
            } else {
                out << std::setw(static_cast<int>(widths[i])) << std::left << text;
            }
        }
        out << '\n';
    };

    emit_row(kHeader);
    if (!columns.empty()) {
        for (std::size_t row = 0; row < columns.front().cells.size(); ++row) emit_row(row);
    }
    return out.str();
}

std::string FormatAllSessions(const std::vector<std::string>& session_ids, CostStore& store,
                              RateTable& rates) {
    struct Row {
        std::string session_id;
        std::string parent;
        SessionTotals totals;
    };
    std::vector<Row> rows;
    rows.reserve(session_ids.size());
    for (const auto& id : session_ids) {
        try {
            const auto history = store.Load(id);
            Row row;
            row.session_id = id;
            row.parent = history.parent_session_id;
            row.totals = Aggregate(id, history.parent_session_id, history.turns,
                                   [&rates](const std::string& model) {
                                       return rates.Resolve(model);
                                   });
            rows.push_back(std::move(row));
        } catch (const std::exception&) {
            // One corrupt blob must not lose the report: skip it, the
            // per-session /cost path surfaces the error with its name.
        }
    }
    // Root sessions first (most spend first), then their subagents adjacent to
    // their parent -- the same adjacency the tab bar uses.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        const bool a_child = !a.parent.empty();
        const bool b_child = !b.parent.empty();
        if (a_child != b_child) return !a_child;
        if (a_child && b_child) return a.parent < b.parent;
        return a.totals.total_cost > b.totals.total_cost;
    });

    std::ostringstream out;
    for (const auto& row : rows) {
        out << CostCell(row.totals.total_cost, row.totals.total_cost_known);
        out << "  " << row.session_id;
        if (!row.parent.empty()) out << "  (subagent of " << row.parent << ")";
        if (row.totals.unpriced_turns > 0) {
            out << "  [" << row.totals.unpriced_turns << " turn(s) unpriced]";
        }
        out << '\n';
    }
    return out.str();
}

}  // namespace

std::string FormatCost(double usd) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << '$' << usd;
    return out.str();
}

std::string FormatTokens(std::int64_t tokens) {
    std::string digits = std::to_string(tokens);
    int group = 0;
    for (int i = static_cast<int>(digits.size()) - 1; i > 0; --i) {
        if (++group % 3 == 0) digits.insert(static_cast<std::size_t>(i), ",");
    }
    return digits;
}

std::string FormatSessionTotals(const SessionTotals& totals) {
    // OpenAI-shaped backends have no cache writes (often no reads either);
    // an all-zero cache column is pure noise, dropped the same way the
    // {token_detail} status segment drops zero fields.
    const bool show_cache =
        std::any_of(totals.models.begin(), totals.models.end(),
                    [](const ModelTotals& model) {
                        return model.cache_read_tokens > 0 || model.cache_write_tokens > 0;
                    });

    Column models{"model", {}, false};
    Column turns{"turns", {}, true};
    Column in{"in", {}, true};
    Column out{"out", {}, true};
    Column cache{"cache r/w", {}, true};
    Column cost{"cost", {}, true};
    for (const auto& model : totals.models) {
        models.cells.push_back(model.model.empty() ? "(unknown)" : model.model);
        turns.cells.push_back(std::to_string(model.turns));
        in.cells.push_back(FormatTokens(model.input_tokens));
        out.cells.push_back(FormatTokens(model.output_tokens));
        if (show_cache) {
            cache.cells.push_back(FormatTokens(model.cache_read_tokens) + "/" +
                                  FormatTokens(model.cache_write_tokens));
        }
        cost.cells.push_back(CostCell(model.cost, model.cost_known));
    }

    std::vector<Column> columns;
    columns.reserve(6);
    columns.push_back(std::move(models));
    columns.push_back(std::move(turns));
    columns.push_back(std::move(in));
    columns.push_back(std::move(out));
    if (show_cache) columns.push_back(std::move(cache));
    columns.push_back(std::move(cost));

    std::string table = FormatTable(columns);

    std::ostringstream report;
    report << table;
    report << "  total " << totals.total_turns << " turn(s), est. "
           << CostCell(totals.total_cost, totals.total_cost_known);
    if (totals.unpriced_turns > 0) {
        report << "  (" << totals.unpriced_turns
               << " turn(s) unpriced -- run /cost-update-rates)";
    }
    report << '\n';
    return report.str();
}

commands::CommandResult CostCommand::Execute(commands::CommandContext& context,
                                             const commands::CommandArgs& args) {
    if (!store_.available()) {
        return commands::CommandResult::Error(
            "cost history is unavailable: the host does not provide plugin storage");
    }
    const bool want_all =
        std::any_of(args.positional.begin(), args.positional.end(), [](const std::string& arg) {
            return arg == "all";
        });
    if (want_all) {
        const auto session_ids = store_.SessionIds();
        if (session_ids.empty()) {
            return commands::CommandResult::Ok("no recorded cost yet");
        }
        return commands::CommandResult::Ok(FormatAllSessions(session_ids, store_, rates_));
    }

    const std::string& session_id = context.session().session_id();
    SessionTotals totals;
    try {
        const auto history = store_.Load(session_id);
        if (history.turns.empty()) {
            return commands::CommandResult::Ok("no recorded cost yet in this session");
        }
        totals = Aggregate(session_id, history.parent_session_id, history.turns,
                           [this](const std::string& model) { return rates_.Resolve(model); });
    } catch (const std::exception& error) {
        return commands::CommandResult::Error(error.what());
    }
    return commands::CommandResult::Ok(FormatSessionTotals(totals));
}

}  // namespace minimal::cost_tracker
