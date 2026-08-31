#include "update_rates_command.hpp"

#include "minimal/commands/command_context.hpp"
#include "rates_fetch.hpp"

namespace minimal::cost_tracker {

commands::CommandResult UpdateRatesCommand::Execute(commands::CommandContext&,
                                                    const commands::CommandArgs&) {
    if (!store_.available()) {
        return commands::CommandResult::Error(
            "rates cannot be cached: the host does not provide plugin storage");
    }
    const auto outcome = FetchAndUpdateRates(settings_, store_, rates_);
    if (!outcome.ok) {
        // The previously fetched (or bundled) table stays in force -- a failed
        // refresh degrades to the stale price, never to no price.
        return commands::CommandResult::Error(outcome.error);
    }
    return commands::CommandResult::Ok("updated " + std::to_string(outcome.updated) +
                                       " model rates (snapshot of " + outcome.fetched_at + ")");
}

}  // namespace minimal::cost_tracker
