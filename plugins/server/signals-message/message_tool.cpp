#include "message_tool.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/agent/signal_service.hpp"

namespace minimal::signals_message {
namespace {

tools::ToolResult Error(const std::string& message) {
    return tools::ToolResult{true, message};
}

class MessageSessionTool final : public tools::Tool {
public:
    // `services` is captured at registration and resolved lazily (Find at
    // execute time) per the service contract -- this plugin may load before
    // or after any other, and the registry is sealed long before the first
    // tool call. The registry outlives every session (main owns it), so a
    // raw captured pointer is the titler's pattern, never dereferenced
    // unguarded.
    explicit MessageSessionTool(const plugins::ServiceRegistry* services)
        : services_(services) {
        spec_.name = "message_session";
        spec_.category = tools::ToolCategory::FileWrite;
        spec_.definition.name = spec_.name;
        spec_.definition.description =
            "Signal another session on this server. Delivery: at the recipient's next turn "
            "boundary, or immediately if it is idle and wakes on signals. A session that "
            "did not subscribe to signals at startup drops the message; you are told. "
            "target_session is a session id, not a name (children learn their parent's id "
            "at spawn). A signal to your own id is refused -- delayed self-reminders are a "
            "timer's job. In content, say what happened and what, if anything, the "
            "recipient should do.";
        spec_.definition.input_schema = {
            {"type", "object"},
            {"properties",
             {{"target_session",
               {{"type", "string"},
                {"description", "The recipient session's id (not its name)"}}},
              {"content",
               {{"type", "string"},
                {"description", "The message body. Written as from one agent to another: "
                                "say what happened and what, if anything, the recipient "
                                "should do about it."}}}}},
            {"required", nlohmann::json::array({"target_session", "content"})}};
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        const auto& object = input.is_object() ? input : nlohmann::json::object();
        const auto target_it = object.find("target_session");
        const auto content_it = object.find("content");
        if (target_it == object.end() || !target_it->is_string() ||
            target_it->get<std::string>().empty()) {
            return Error("message_session needs a non-empty 'target_session' session id");
        }
        if (content_it == object.end() || !content_it->is_string() ||
            content_it->get<std::string>().empty()) {
            return Error("message_session needs non-empty 'content'");
        }
        const std::string target = target_it->get<std::string>();
        const std::string content = content_it->get<std::string>();

        if (services_ == nullptr) {
            return Error("message_session is not available: this host offers no services");
        }
        auto* signal_service =
            static_cast<agent::SignalService*>(services_->Find(agent::kSignalServiceName));
        if (signal_service == nullptr) {
            return Error("message_session is not available: no core.signals service "
                         "(the host does not offer the seam)");
        }

        const agent::SignalDelivery delivery =
            signal_service->DeliverInternal(ctx.session_id, target, content);
        // Honest outcomes, not optimism: the sender is in-process and can
        // react to a miss -- unlike an external channel, whose drops are
        // silent by design.
        switch (delivery) {
            case agent::SignalDelivery::kDelivered:
                return tools::ToolResult{false, "delivered to session " + target};
            case agent::SignalDelivery::kNoSubscriber:
                return tools::ToolResult{
                    false, "NOT delivered: session " + target +
                               " does not exist, is not running, or did not subscribe to "
                               "\"sessions\" when it started (it is not accepting signals)"};
            case agent::SignalDelivery::kInvalid:
                // The router refuses for: empty sender/target/content, or
                // self-address. This tool has already validated its own
                // inputs (non-empty target and content), so the reachable
                // causes here are an empty CALLER id or self-address -- the
                // old fallback message ("needs a target and content")
                // described inputs this tool had already checked.
                return target == ctx.session_id
                           ? Error("message_session cannot target the calling session "
                                   "itself -- self-reminders are a timer's job, not a "
                                   "signal")
                           : Error("message_session could not identify the calling session "
                                   "(empty caller id) -- the signal was refused");
            case agent::SignalDelivery::kDropped:
                return Error("session " + target +
                             " is terminating; the signal was not staged");
        }
        return Error("message_session: unknown delivery outcome");
    }

private:
    tools::ToolSpec spec_;
    const plugins::ServiceRegistry* services_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeMessageSessionTool(
    const plugins::ServiceRegistry* services) {
    return std::make_unique<MessageSessionTool>(services);
}

}  // namespace minimal::signals_message
