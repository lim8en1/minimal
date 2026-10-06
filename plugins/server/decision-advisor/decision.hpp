#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace minimal::decision_advisor {

// The protocol, not tunables: the question the advisor acts on and the options
// its answer must choose among. The configured rubric (questions.action) must
// define exactly these options -- ParseSettings checks it at load, so a rubric
// edit that renames one fails naming the field instead of abstaining forever.
inline constexpr std::string_view kActionQuestion = "action";
inline constexpr std::array<std::string_view, 3> kActionOptions = {"allow", "deny", "review"};

// The one decision the advisor can act on, resolved from a Choice answer over
// the fixed option set {allow, deny, review}. Anything below the operator's
// confidence bars abstains -- which the caller expresses as nullopt, the
// advisor contract's "the human decides".
enum class Action { kAllow, kDeny, kAbstain };

struct Decision {
    Action action = Action::kAbstain;
    // Why an abstain happened (empty for a decisive answer): the answer named
    // "review", a probability missed its floor, or the top two options were
    // too close. Goes to the log line, never to the model.
    std::string abstain_reason;
};

// The slice of the systemone answer the rule reads. Shape per api.md, verified
// live against OpenRouter's /api/alpha/decisions (the envelope is verbatim from
// both backends): {"choice": "...", "probabilities": {...}, "confidence": ...}.
struct ChoiceAnswer {
    std::string choice;
    double p_allow = 0.0;
    double p_deny = 0.0;
    double p_review = 0.0;
};

// Extracts answers["action"] as a ChoiceAnswer. nullopt when the body is not
// the expected envelope, the answer is missing, is not a choice, names an
// option outside the fixed set, or lacks a numeric probability for any option
// (an absent option is not "probability 0") -- every one of those is a contract violation
// the caller reports as a warn + abstain, never a guess.
std::optional<ChoiceAnswer> ExtractActionAnswer(const nlohmann::json& body);

// The census AND majority rule (docs/EXTENSIBILITY plan: both, per TypeSafe's
// choice-cookbook data -- their label flips happened exactly when top
// probabilities were close, and their production automation gate is top-pick
// plus an absolute floor; neither alone stops the other's failure mode).
//
//   choice == review                              -> abstain ("review")
//   p(choice) < floor(choice)                     -> abstain ("probability < floor")
//   p(choice) - best_other < margin               -> abstain ("margin")
//   choice == allow                               -> allow
//   choice == deny                                -> deny
//
// The floors come from Settings: allow >= min_probability_allow,
// deny >= min_probability_deny, review is never acted on so it needs none.
// The margin is one knob applied to every action.
Decision ResolveDecision(const ChoiceAnswer& answer, double min_probability_allow,
                         double min_probability_deny, double min_margin);

}  // namespace minimal::decision_advisor
