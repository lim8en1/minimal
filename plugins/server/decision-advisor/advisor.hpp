#pragma once

#include <memory>
#include <optional>

#include "minimal/common/secret_handle.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/net/http_client.hpp"
#include "minimal/tools/permission_advisor.hpp"

#include "settings.hpp"

namespace minimal::decision_advisor {

// The track-J advisor: answers an Ask by asking Jev whether the tool call
// should be allowed, denied, or left to a human. Everything dangerous stays
// substrate-side -- this class is consulted only after the mode lattice, the
// precedence chains, the subagent ceilings and the run modes have resolved a
// call to Ask, and ToolExecutor re-filters whatever grants it returns against
// the disclosure. It can only narrow what reaches a person.
//
// Advise runs off the session worker on the checker's advise thread; it polls
// is_cancelled (before every HTTP attempt and during every backoff sleep) and
// returns promptly, per the advisor contract. Owned values only: the request
// arrives by value into a thread that may be detached and outlive the session.
class DecisionAdvisor : public tools::PermissionAdvisor {
public:
    // `api_key` is read per Advise call; one that does not resolve abstains.
    DecisionAdvisor(Settings settings, common::SecretHandle api_key, net::HttpClient http,
                    common::Logger logger = common::DefaultLogger());

    // nullopt = abstain: the human decides. A decisive Allow carries every key
    // the disclosure offered (the point of auto mode is to silence future
    // prompts; the grant filter downstream makes minting undisclosed keys
    // impossible). A decisive Deny carries the message the model reads.
    std::optional<tools::PermissionResponse> Advise(const tools::PermissionRequest& request,
                                                    const tools::AdvisorContext& context) override;

private:
    nlohmann::json StateOf(const tools::PermissionRequest& request) const;

    Settings settings_;
    common::SecretHandle api_key_;
    net::HttpClient http_;
    common::Logger logger_;
};

}  // namespace minimal::decision_advisor
