#pragma once

// /cost -- token usage and estimated cost for this session, or a one-line
// summary per session with "/cost all".
//
// A HarnessCommand, ReadOnly: it touches no conversation state, so it may run
// beside a live turn (on the RPC layer's command pool). It reads the same
// store the observer writes and resolves prices through the same RateTable
// the update command refreshes -- three surfaces, one state, which is the
// tools-bash/checklist "a plugin owns the commands that touch its state"
// shape, minus the tool.

#include "cost_model.hpp"
#include "cost_store.hpp"
#include "minimal/commands/command.hpp"

namespace minimal::cost_tracker {

std::string FormatCost(double usd);
std::string FormatTokens(std::int64_t tokens);
std::string FormatSessionTotals(const SessionTotals& totals);

class CostCommand : public commands::Command {
public:
    CostCommand(CostStore& store, RateTable& rates) : store_(store), rates_(rates) {}

    std::string_view Name() const override { return "cost"; }
    std::string_view Summary() const override {
        return "show token usage and estimated cost (this session, or `all`)";
    }
    std::string_view Usage() const override { return "/cost [all]"; }
    bool ReadOnly() const override { return true; }

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override;

private:
    CostStore& store_;
    RateTable& rates_;
};

}  // namespace minimal::cost_tracker
