#pragma once

// The generic client channel's endpoint, provided as "checklist.endpoint"
// (plugin_abi.h's PluginEndpoint convention). The server's PluginCall
// dispatch prefers ".endpoint" over ".rpc", so once this is Provided every
// client PluginCall lands here:
//   - Call(method, payload) is the ".rpc" service verbatim (rpc_service.hpp).
//     A plugin-domain refusal throws, which the transport answers INTERNAL --
//     the caller sees the same failed call ".rpc" gave it.
//   - Subscribe("changes", _) attaches the stream to the push hub
//     (checklist_hub.hpp); anything else is refused (NOT_FOUND).

#include <memory>
#include <string>

#include "checklist_hub.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "rpc_service.hpp"

namespace minimal::checklist {

class ChecklistEndpoint final : public plugins::PluginEndpoint {
public:
    // Borrowed; process lifetime.
    ChecklistEndpoint(ChecklistRpcService& rpc, ChecklistHub& hub) : rpc_(rpc), hub_(hub) {}

    std::string Call(const std::string& method, const std::string& json_payload) override;

    bool Subscribe(const std::string& method, const std::string& json_payload,
                   std::shared_ptr<plugins::PluginSubscriptionSink> sink) override;

private:
    ChecklistRpcService& rpc_;
    ChecklistHub& hub_;
};

}  // namespace minimal::checklist
