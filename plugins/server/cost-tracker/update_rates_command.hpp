#pragma once

// /cost-update-rates -- the opt-in live fetch of the LiteLLM pricing snapshot.
// Deliberately a command, never startup work: no network at boot, and the
// fetched table is cached in the plugin's storage so it survives restarts and
// outranks the (aging) bundled snapshot until replaced.

#include "cost_model.hpp"
#include "cost_settings.hpp"
#include "cost_store.hpp"
#include "minimal/commands/command.hpp"

namespace minimal::cost_tracker {

class UpdateRatesCommand : public commands::Command {
public:
    UpdateRatesCommand(CostStore& store, RateTable& rates, const CostSettings& settings)
        : store_(store), rates_(rates), settings_(settings) {}

    std::string_view Name() const override { return "cost-update-rates"; }
    std::string_view Summary() const override {
        return "fetch current model prices from the LiteLLM pricing snapshot";
    }
    std::string_view Usage() const override { return "/cost-update-rates"; }
    bool ReadOnly() const override { return true; }

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override;

private:
    CostStore& store_;
    RateTable& rates_;
    const CostSettings& settings_;
};

}  // namespace minimal::cost_tracker
