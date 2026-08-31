#include "cost_command.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "minimal/commands/command_context.hpp"

namespace minimal::cost_tracker {
namespace {

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
    out << std::fixed;
    for (const auto& row : rows) {
        out << FormatCost(row.totals.total_cost);
        if (!row.totals.total_cost_known) out << "+";
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
    std::ostringstream out;
    out << std::fixed;
    out << "  model              turns        in       out   cache r/w            cost\n";
    for (const auto& model : totals.models) {
        out << std::setw(19) << std::left << (model.model.empty() ? "(unknown)" : model.model);
        out << std::setw(6) << std::right << model.turns;
        out << std::setw(11) << FormatTokens(model.input_tokens);
        out << std::setw(11) << FormatTokens(model.output_tokens);
        out << std::setw(13) << FormatTokens(model.cache_read_tokens) + "/" +
                   FormatTokens(model.cache_write_tokens);
        out << std::setw(15) << FormatCost(model.cost);
        if (!model.cost_known) out << "+";
        out << '\n';
    }
    out << "  total " << totals.total_turns << " turn(s)";
    out << ", est. " << FormatCost(totals.total_cost);
    if (!totals.total_cost_known) out << "+  (some turns unpriced -- run /cost-update-rates)";
    out << '\n';
    return out.str();
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
