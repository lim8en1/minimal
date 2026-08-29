#pragma once

// Hand-written fakes, deliberately without a mocking framework.
//
// The interfaces here are tiny by design -- LlmProvider is two methods,
// PermissionChecker / ConfirmationBroker / MentionResolver are one each -- so
// fakes are cheaper than GMock. That is a property worth protecting: an interface
// that needs a mocking framework to test is an interface that grew too big.
//
// This header is an installed target, not test-local code: plugin authors link the
// same fakes, which matters when most functionality ships as plugins.

#include <string>
#include <vector>

#include "minimal/tools/permission.hpp"

namespace minimal::testing {

// Scripted responses, popped in order. Lets the whole agent loop -- tool-call
// cycles, retries, cancellation, compaction triggers, subagent paths -- be tested
// deterministically with no network.
//
// TODO: implement against minimal::llm::LlmProvider once that header lands.
class FakeLlmProvider {
public:
    void PushResponse(std::string text) { scripted_.push_back(std::move(text)); }
    std::size_t call_count() const { return call_count_; }

private:
    std::vector<std::string> scripted_;
    std::size_t call_count_ = 0;
};

// Fixed-answer permission policies, for exercising the gating paths without a
// client attached. Real PermissionChecker fakes land once that header exists.
inline tools::PermissionPolicy AllowAllPolicy() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = tools::PermissionMode::AlwaysAllow;
    return p;
}

inline tools::PermissionPolicy DenyAllPolicy() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = tools::PermissionMode::AlwaysDeny;
    return p;
}

}  // namespace minimal::testing
