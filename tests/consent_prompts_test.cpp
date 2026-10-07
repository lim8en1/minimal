// The bundled consent-prompts plugin: the permission and confirmation modals
// (moved from core's view_factory tests with them), the prompt-kind
// renderers over them, and core's side of the two kinds -- the payloads it
// sends and the fail-closed reading of the answers.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "consent_modals.hpp"
#include "consent_prompts.hpp"
#include "ftxui_harness.hpp"
#include "widget_kit_fixture.hpp"

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/tab_ui.hpp"

using namespace minimal::client::consent_prompts;
using namespace minimal_test;
namespace ftx = ftxui;
namespace client = minimal::client;
namespace plugins = minimal::client::plugins;
namespace v1 = minimal::v1;
using Theme = minimal::client::Theme;
using minimal::client::ColorDepth;

namespace {

Theme TestTheme() {
    return Theme::Parse(R"({"name":"test","colors":{}})", ColorDepth::Truecolor);
}

const plugins::WidgetKit& Kit() { return CoreWidgetKit(); }

v1::GrantOffer MakeGrant(const std::string& key, const std::string& label) {
    v1::GrantOffer offer;
    offer.set_key(key);
    offer.set_label(label);
    return offer;
}

PendingPermission MakePermissionRequest() {
    PendingPermission request;
    request.request_id = "perm-1";
    request.tool_name = "bash";
    request.summary = "run make";
    request.complete = false;   // the lower-bound paragraph must render
    request.grants = {MakeGrant("bash:/usr/bin/make", "/usr/bin/make"),
                      MakeGrant("bash:/usr/bin/tail", "/usr/bin/tail")};
    return request;
}

client::PendingPermission CorePermissionRequest() {
    client::PendingPermission request;
    request.request_id = "perm-1";
    request.tool_name = "bash";
    request.category = v1::TOOL_CATEGORY_EXECUTE;
    request.summary = "run make";
    request.grants = {MakeGrant("bash:/usr/bin/make", "/usr/bin/make")};
    return request;
}

// The registered renderer for `kind`, the way core finds it.
const plugins::PromptRenderer& Renderer(plugins::PromptRendererRegistry& registry,
                                        std::string_view kind) {
    if (registry.Find(kind) == nullptr) {
        for (auto& renderer : MakeRenderers(&Kit())) registry.Register(std::move(renderer));
    }
    return *registry.Find(kind);
}

}  // namespace

// ---------------------------------------------------------------------------
// Permission modal (R1b-3)
// ---------------------------------------------------------------------------

TEST_CASE("permission modal: answers after the caller's request snapshot dies") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    auto request = std::make_unique<PendingPermission>(MakePermissionRequest());

    // The callback owns its copy (RunTui's does too): GrantedKeys reads the
    // request AFTER the caller's snapshot is gone.
    const PendingPermission answer_source = *request;
    std::optional<PermissionVerdict> answered;
    std::vector<std::string> keys;
    auto modal = MakePermissionModal(*request, toggles, Kit(), theme, [&](PermissionVerdict verdict) {
        answered = verdict;
        keys = toggles.GrantedKeys(answer_source);
    });

    request.reset();   // the per-frame snapshot dies; the modal must not care

    const auto frame = RenderFrame(modal);
    CHECK(frame.find("bash") != std::string::npos);
    CHECK(frame.find("may reach more") != std::string::npos);   // lower-bound obligation
    CHECK(frame.find("session grants") != std::string::npos);   // per-binary offers present
    // All four answers are listed, numbered in PermissionChoice order.
    CHECK(frame.find("1. allow") != std::string::npos);
    CHECK(frame.find("2. deny") != std::string::npos);
    CHECK(frame.find("3. deny with message") != std::string::npos);
    CHECK(frame.find("4. deny and stop") != std::string::npos);

    // Walk the cursor up onto the first grant (the default cursor is Allow,
    // not a grant -- see the unified-ladder test below), toggle it with
    // space, jump back to the answers with Tab, then allow: the granted key
    // is exactly what was offered, never minted.
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));   // Allow -> grant 1 (tail)
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));   // grant 1 -> grant 0 (make)
    REQUIRE(SendEvent(modal, ftx::Event::Character(" ")));   // toggle grant 0
    REQUIRE(SendEvent(modal, ftx::Event::Tab));              // jump to Allow
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Allow);
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("permission modal: Escape denies without recording grants") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });
    REQUIRE(SendEvent(modal, ftx::Event::Escape));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Deny);
}

TEST_CASE("permission modal: with grants offered, the footer advertises the "
          "tab jump") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();  // has two grants
    auto modal = MakePermissionModal(request, toggles, Kit(), theme, [](PermissionVerdict) {});
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("1-4: choose") != std::string::npos);
    CHECK(frame.find("tab: grants/answers") != std::string::npos);
}

TEST_CASE("permission modal: with no grants offered, the footer omits the dead "
          "tab affordance") {
    // Regression: the footer used to always advertise the grant keys, even
    // on a request with nothing to move between or toggle -- dead keys
    // advertised on the security-decision surface.
    const auto theme = TestTheme();
    GrantToggles toggles;
    auto request = MakePermissionRequest();
    request.grants.clear();
    auto modal = MakePermissionModal(request, toggles, Kit(), theme, [](PermissionVerdict) {});
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("enter: confirm") != std::string::npos);
    CHECK(frame.find("esc: deny") != std::string::npos);
    CHECK(frame.find("tab:") == std::string::npos);
}

TEST_CASE("permission modal: a Return as the FIRST event arms but does not "
          "approve; a second Return does") {
    // Regression: a Return landing in the same input batch as the request's
    // own arrival (a fast typist's unrelated keystroke, a paste's trailing
    // newline) used to approve a tool call the user had not read yet.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    SendEvent(modal, ftx::Event::Return);   // arms, does not approve
    CHECK_FALSE(answered.has_value());

    REQUIRE(SendEvent(modal, ftx::Event::Return));   // the user's real answer
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Allow);
}

TEST_CASE("permission modal: Escape still denies as the very first event") {
    // Denying is the safe default; only the other answers need an arming
    // delay.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });
    REQUIRE(SendEvent(modal, ftx::Event::Escape));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Deny);
}

TEST_CASE("permission modal: a digit as the FIRST event only selects; Return then "
          "answers the selection") {
    // The arming rule covers the deny answers too: a stray '4' must not stop
    // the turn behind a prompt nobody has read.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    REQUIRE(SendEvent(modal, ftx::Event::Character("4")));
    CHECK_FALSE(answered.has_value());
    CHECK(RenderFrame(modal).find("❯ 4. deny and stop") != std::string::npos);

    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::DenyAndStop);
}

TEST_CASE("permission modal: once armed, a digit answers directly") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));   // arms
    CHECK_FALSE(answered.has_value());
    REQUIRE(SendEvent(modal, ftx::Event::Character("2")));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Deny);
}

TEST_CASE("permission modal: arrows walk the whole ladder -- grants then "
          "answers -- and clamp at both ends") {
    // Regression shape: arrows used to move ONLY among the four answers,
    // never reaching the grant checkboxes at all (they were Tab+Space/mouse
    // only). One cursor now walks both sections.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();   // two grants
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    // Starts on Allow -- the default ACTION, not a grant.
    CHECK(RenderFrame(modal).find("❯ 1. allow") != std::string::npos);

    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));   // Allow -> grant 1 (tail)
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));   // grant 1 -> grant 0 (make)
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));   // top: clamps, stays on grant 0
    CHECK(RenderFrame(modal).find("❯ 1. allow") == std::string::npos);   // off the answers entirely

    for (int i = 0; i < 6; ++i) REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));
    // grant 0 -> grant 1 -> allow -> deny -> deny w/ message -> deny and stop -> clamps
    CHECK(RenderFrame(modal).find("❯ 4. deny and stop") != std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::ArrowUp));
    CHECK(RenderFrame(modal).find("❯ 3. deny with message") != std::string::npos);
    CHECK_FALSE(answered.has_value());   // moving never answers
}

TEST_CASE("permission modal: Tab jumps between the grants and the answers") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();   // two grants
    auto modal = MakePermissionModal(request, toggles, Kit(), theme, [](PermissionVerdict) {});

    // Starts among the answers (Allow); Tab jumps to the first grant.
    REQUIRE(SendEvent(modal, ftx::Event::Tab));
    CHECK(RenderFrame(modal).find("❯ 1. allow") == std::string::npos);
    // And back again, landing on the first answer (Allow).
    REQUIRE(SendEvent(modal, ftx::Event::Tab));
    CHECK(RenderFrame(modal).find("❯ 1. allow") != std::string::npos);
}

TEST_CASE("permission modal: clicking an answer row picks and submits it "
          "immediately, no arming delay") {
    // "Clicking an answer row selects and submits it" -- the capability the
    // old plain-text answer rows had no hit box to offer at all.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    const auto frame = RenderFrame(modal);
    const int y = ScreenLineOf(frame, "2. deny");
    REQUIRE(y >= 0);
    ftx::Mouse click;
    click.button = ftx::Mouse::Left;
    click.motion = ftx::Mouse::Pressed;
    click.x = 5;
    click.y = y;
    // A single click answers outright -- no separate arm-then-confirm step,
    // unlike a keyboard pick (see the arming-rule tests above): a click is
    // already the deliberate, aimed action that rule exists to require.
    REQUIRE(SendEvent(modal, ftx::Event::Mouse("", click)));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Deny);
}

TEST_CASE("permission modal: clicking a grant checkbox toggles it without "
          "answering") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    const auto frame = RenderFrame(modal);
    const int y = ScreenLineOf(frame, "/usr/bin/make");
    REQUIRE(y >= 0);
    ftx::Mouse click;
    click.button = ftx::Mouse::Left;
    click.motion = ftx::Mouse::Pressed;
    click.x = 5;
    click.y = y;
    REQUIRE(SendEvent(modal, ftx::Event::Mouse("", click)));
    CHECK_FALSE(answered.has_value());
    const auto keys = toggles.GrantedKeys(request);
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "bash:/usr/bin/make");
}

TEST_CASE("permission modal: a passive mouse move does not arm it") {
    // THE BUG this replaces: any mouse event -- including a hover with no
    // click at all -- used to satisfy the arming rule, so a Return arriving
    // right after could answer a request the user had not deliberately
    // acted on yet.
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    ftx::Mouse move;
    move.button = ftx::Mouse::None;
    move.motion = ftx::Mouse::Moved;
    move.x = 5;
    move.y = 5;
    SendEvent(modal, ftx::Event::Mouse("", move));   // a hover, not a click

    REQUIRE(SendEvent(modal, ftx::Event::Return));   // must still only ARM
    CHECK_FALSE(answered.has_value());
    REQUIRE(SendEvent(modal, ftx::Event::Return));   // the user's real answer
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Allow);
}

TEST_CASE("permission modal: deny with message collects typed text and sends it") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // arms + selects
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // opens the input
    CHECK_FALSE(answered.has_value());
    CHECK(RenderFrame(modal).find("message:") != std::string::npos);
    CHECK(RenderFrame(modal).find("enter: deny with this message") != std::string::npos);

    // Typed text -- a space included -- goes to the input, never to the
    // grant checkboxes behind it, and digits type rather than pick.
    for (const char c : std::string(" use make -j4 ")) {
        REQUIRE(SendEvent(modal, ftx::Event::Character(std::string(1, c))));
    }
    CHECK(toggles.GrantedKeys(request).empty());
    CHECK_FALSE(answered.has_value());

    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::DenyWithMessage);
    CHECK(answered->message == "use make -j4");   // trimmed
}

TEST_CASE("permission modal: Escape backs out of the message input; an empty "
          "message degrades to a plain deny") {
    const auto theme = TestTheme();
    GrantToggles toggles;
    const auto request = MakePermissionRequest();
    std::optional<PermissionVerdict> answered;
    auto modal = MakePermissionModal(request, toggles, Kit(), theme,
                                     [&](PermissionVerdict verdict) { answered = verdict; });

    REQUIRE(SendEvent(modal, ftx::Event::ArrowDown));        // arms
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));   // opens the input
    REQUIRE(SendEvent(modal, ftx::Event::Escape));           // back, NOT a deny
    CHECK_FALSE(answered.has_value());
    CHECK(RenderFrame(modal).find("message:") == std::string::npos);

    // Ctrl+C falls through to the root's turn-cancel, input open or not.
    REQUIRE(SendEvent(modal, ftx::Event::Character("3")));
    CHECK_FALSE(SendEvent(modal, ftx::Event::CtrlC));

    REQUIRE(SendEvent(modal, ftx::Event::Return));   // nothing typed
    REQUIRE(answered.has_value());
    CHECK(answered->choice == PermissionChoice::Deny);
    CHECK(answered->message.empty());
}

// ---------------------------------------------------------------------------
// Confirmation modal
// ---------------------------------------------------------------------------

TEST_CASE("confirmation modal: renders the ask and answers the focused option") {
    const auto theme = TestTheme();
    PendingConfirmation request;
    request.kind = "app_quit";
    request.title = "destroy running work?";
    request.detail = "a session still has work in flight";
    request.options = {"close anyway", "cancel"};

    std::vector<std::string> answers;
    auto modal = MakeConfirmationModal(
        request, Kit(), theme, [&](const std::string& chosen) { answers.push_back(chosen); });

    const auto frame = RenderFrame(modal);
    CHECK(frame.find("destroy running work?") != std::string::npos);
    CHECK(frame.find("close anyway") != std::string::npos);

    // "cancel" is the safe default focus (see the dedicated test below), not
    // the destructive "close anyway" -- a bare Return answers "cancel".
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answers.size() == 1);
    CHECK(answers[0] == "cancel");
}

TEST_CASE("confirmation modal: default focus is the cancel option, not the "
          "destructive one") {
    // Regression: Container defaults ActiveChild() to index 0, and both real
    // call sites (close/quit's "close anyway", the picker's "delete") put
    // the destructive action first -- a reflexive Enter as the modal opened
    // fired it with no arming delay.
    const auto theme = TestTheme();
    PendingConfirmation request;
    request.kind = "delete_session";
    request.title = "delete this session?";
    request.detail = "this cannot be undone";
    request.options = {"delete", "cancel"};

    std::vector<std::string> answers;
    auto modal = MakeConfirmationModal(
        request, Kit(), theme, [&](const std::string& chosen) { answers.push_back(chosen); });

    RenderFrame(modal);   // ActiveChild is set at construction; render just settles the DOM
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answers.size() == 1);
    CHECK(answers[0] == "cancel");
}

TEST_CASE("confirmation modal: with no cancel option, the first Enter only arms it") {
    // The server's Yes/No confirms have no cancel option to default to, so
    // focus starts on "Yes" -- and a reflexive Enter (the user was mid-way
    // through submitting a message) answered it. The first Enter now arms
    // the modal, like the permission modal's rule; the second answers.
    const auto theme = TestTheme();
    PendingConfirmation request;
    request.kind = "wire_confirm";
    request.title = "clear the conversation?";
    request.detail = "";
    request.options = {"Yes", "No"};

    std::vector<std::string> answers;
    auto modal = MakeConfirmationModal(
        request, Kit(), theme, [&](const std::string& chosen) { answers.push_back(chosen); });

    REQUIRE(SendEvent(modal, ftx::Event::Return));   // arms
    CHECK(answers.empty());
    REQUIRE(SendEvent(modal, ftx::Event::Return));
    REQUIRE(answers.size() == 1);
    CHECK(answers[0] == "Yes");
}


// ---------------------------------------------------------------------------
// The prompt kinds: core's payload in, the plugin's answer out
// ---------------------------------------------------------------------------

TEST_CASE("consent prompts: core's permission payload renders and Allow answers allow") {
    plugins::PromptRendererRegistry registry;
    const Theme theme = TestTheme();   // outlives the component (the contract)
    const auto request = CorePermissionRequest();
    std::optional<plugins::PromptReply> reply;
    auto modal = Renderer(registry, "permission")
                     .Make({.kind = "permission",
                            .payload = client::PermissionPayload(request),
                            .asker = "build"},
                           theme, [&](plugins::PromptReply r) { reply = std::move(r); });
    const auto frame = RenderFrame(modal);
    CHECK(frame.find("bash") != std::string::npos);
    CHECK(frame.find("/usr/bin/make") != std::string::npos);

    REQUIRE(SendEvent(modal, ftx::Event::Character('1')));   // first event: selects only
    CHECK_FALSE(reply.has_value());
    REQUIRE(SendEvent(modal, ftx::Event::Return));            // allow
    REQUIRE(reply.has_value());
    CHECK(reply->outcome == v1::PROMPT_OUTCOME_ANSWERED);
    CHECK(reply->answer.value("decision", "") == "allow");
    CHECK(reply->answer.value("granted_keys", nlohmann::json::array()).empty());
}

TEST_CASE("consent prompts: core's confirmation payload renders and answers the choice") {
    plugins::PromptRendererRegistry registry;
    const Theme theme = TestTheme();
    const std::vector<std::string> options = {"delete", "cancel"};
    std::optional<plugins::PromptReply> reply;
    auto modal =
        Renderer(registry, "confirmation")
            .Make({.kind = "confirmation",
                   .payload = client::ConfirmationPayload("delete_session", "delete it?", "",
                                                          options)},
                  theme, [&](plugins::PromptReply r) { reply = std::move(r); });
    CHECK(RenderFrame(modal).find("delete it?") != std::string::npos);
    REQUIRE(SendEvent(modal, ftx::Event::Return));   // the safe default: cancel
    REQUIRE(reply.has_value());
    CHECK(client::ConfirmationChoice(options, *reply) == "cancel");
}

TEST_CASE("consent prompts: core reads anything but an offered option as do-not-proceed") {
    const std::vector<std::string> options = {"Yes", "No"};
    using Reply = plugins::PromptReply;
    CHECK(client::ConfirmationChoice(options, Reply{v1::PROMPT_OUTCOME_ANSWERED,
                                                    {{"chosen", "Yes"}}}) == "Yes");
    CHECK(client::ConfirmationChoice(options, Reply{v1::PROMPT_OUTCOME_ANSWERED,
                                                    {{"chosen", "maybe"}}}).empty());
    CHECK(client::ConfirmationChoice(options, Reply{v1::PROMPT_OUTCOME_ANSWERED, "Yes"}).empty());
    CHECK(client::ConfirmationChoice(options, Reply{v1::PROMPT_OUTCOME_UNAVAILABLE,
                                                    {{"chosen", "Yes"}}}).empty());
}

TEST_CASE("consent prompts: no renderer, or a throwing one, builds no modal") {
    plugins::PromptRendererRegistry registry;
    CHECK(client::MakeConsentModal(registry, "permission", nlohmann::json::object(), "",
                                   TestTheme(), [](plugins::PromptReply) {}) == nullptr);

    struct Throwing final : plugins::PromptRenderer {
        std::string_view kind() const override { return "permission"; }
        ftxui::Component Make(const plugins::PromptView&, const Theme&,
                              std::function<void(plugins::PromptReply)>) const override {
            throw std::runtime_error("boom");
        }
    };
    registry.Register(std::make_unique<Throwing>());
    CHECK(client::MakeConsentModal(registry, "permission", nlohmann::json::object(), "",
                                   TestTheme(), [](plugins::PromptReply) {}) == nullptr);
}
