#include "consent_prompts.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "consent_modals.hpp"

namespace minimal::client::consent_prompts {

namespace {

using nlohmann::json;

PendingPermission ParsePermission(const json& payload) {
    if (!payload.is_object()) throw std::invalid_argument("permission payload is not an object");
    PendingPermission request;
    request.tool_name = payload.value("tool_name", "");
    v1::ToolCategory category = v1::TOOL_CATEGORY_UNSPECIFIED;
    if (v1::ToolCategory_Parse(payload.value("category", ""), &category)) {
        request.category = category;
    }
    request.summary = payload.value("summary", "");
    request.complete = payload.value("complete", false);
    request.reason = payload.value("reason", "");
    for (const auto& grant : payload.value("grants", json::array())) {
        v1::GrantOffer offer;
        offer.set_key(grant.value("key", ""));
        offer.set_label(grant.value("label", ""));
        request.grants.push_back(std::move(offer));
    }
    return request;
}

PendingConfirmation ParseConfirmation(const json& payload) {
    if (!payload.is_object()) throw std::invalid_argument("confirmation payload is not an object");
    PendingConfirmation request;
    request.kind = payload.value("kind", "");
    request.title = payload.value("title", "");
    request.detail = payload.value("detail", "");
    for (const auto& option : payload.value("options", json::array())) {
        if (option.is_string()) request.options.push_back(option.get<std::string>());
    }
    return request;
}

class PermissionRenderer final : public plugins::PromptRenderer {
public:
    explicit PermissionRenderer(const plugins::WidgetKit* widgets) : widgets_(widgets) {}
    std::string_view kind() const override { return "permission"; }
    ftxui::Component Make(const plugins::PromptView& prompt, const Theme& theme,
                          std::function<void(plugins::PromptReply)> on_reply) const override {
        const PendingPermission request = ParsePermission(prompt.payload);
        // One object shared by the modal (its checkboxes) and the answer.
        const auto toggles = std::make_shared<GrantToggles>();
        return MakePermissionModal(
            request, *toggles, *widgets_, theme,
            [on_reply = std::move(on_reply), toggles, request](PermissionVerdict verdict) {
                static constexpr const char* kDecisions[] = {"allow", "deny", "deny_with_message",
                                                             "deny_and_stop"};
                json answer = {{"decision", kDecisions[static_cast<int>(verdict.choice)]}};
                // Grants ride only an allow, and only what was offered.
                if (verdict.choice == PermissionChoice::Allow) {
                    answer["granted_keys"] = toggles->GrantedKeys(request);
                }
                if (verdict.choice == PermissionChoice::DenyWithMessage) {
                    answer["message"] = std::move(verdict.message);
                }
                on_reply({v1::PROMPT_OUTCOME_ANSWERED, std::move(answer)});
            });
    }

private:
    const plugins::WidgetKit* widgets_;
};

class ConfirmationRenderer final : public plugins::PromptRenderer {
public:
    explicit ConfirmationRenderer(const plugins::WidgetKit* widgets) : widgets_(widgets) {}
    std::string_view kind() const override { return "confirmation"; }
    ftxui::Component Make(const plugins::PromptView& prompt, const Theme& theme,
                          std::function<void(plugins::PromptReply)> on_reply) const override {
        return MakeConfirmationModal(
            ParseConfirmation(prompt.payload), *widgets_, theme,
            [on_reply = std::move(on_reply)](std::string chosen) {
                on_reply({v1::PROMPT_OUTCOME_ANSWERED, json{{"chosen", std::move(chosen)}}});
            });
    }

private:
    const plugins::WidgetKit* widgets_;
};

}  // namespace

std::vector<std::unique_ptr<plugins::PromptRenderer>> MakeRenderers(
    const plugins::WidgetKit* widgets) {
    std::vector<std::unique_ptr<plugins::PromptRenderer>> out;
    out.push_back(std::make_unique<PermissionRenderer>(widgets));
    out.push_back(std::make_unique<ConfirmationRenderer>(widgets));
    return out;
}

}  // namespace minimal::client::consent_prompts
