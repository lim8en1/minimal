// The status bar's width decision (src/client/view/status_bar.{hpp,cpp}),
// tested pure (ARCHITECTURE.md "Testing": render output itself is not
// unit-tested, the logic behind it is). Two regressions this covers:
//
//   1. An unfilled segment used to leave its queued literal separator
//      dangling, so consecutive skipped segments produced doubled " | "
//      runs (e.g. "0.1.0 |  |  | 682/262144 tok | ..." when {activity} was
//      empty).
//   2. With no width awareness at all, FTXUI's hbox shrink pass truncated
//      EVERY segment proportionally at narrow widths, including the token
//      count -- rendering "186/26214" at 80 columns instead of the real
//      "186/262144", a wrong number presented as fact.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "minimal/client/utf8.hpp"
#include "minimal/client/view/status_bar.hpp"

using minimal::client::ParseStatusFormat;
using minimal::client::ResolveStatusBar;
using minimal::client::StatusBarPart;

namespace {
std::vector<std::string> Texts(const std::vector<StatusBarPart>& parts) {
    std::vector<std::string> out;
    for (const auto& part : parts) out.push_back(part.text);
    return out;
}
}  // namespace

TEST_CASE("all segments filled renders every value with literals between", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {tokens} | {model}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"}, {"tokens", "551/262144 tok"}, {"model", "local/x"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "551/262144 tok", " | ", "local/x"});
}

TEST_CASE("an unfilled segment renders nothing and does not orphan its separator",
          "[status_bar]") {
    // Regression: {activity} empty used to leave a bare " | " where it sat,
    // rather than being absorbed into the run around it.
    const auto format = ParseStatusFormat("{version} | {activity} | {tokens}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"},
                                                        {"tokens", "682/262144 tok"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "682/262144 tok"});
}

TEST_CASE("two consecutive unfilled segments still leave exactly one separator",
          "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {activity} | {agent} | {tokens}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"},
                                                        {"tokens", "682/262144 tok"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0", " | ", "682/262144 tok"});
}

TEST_CASE("an unfilled segment first in the format renders no leading literal",
          "[status_bar]") {
    const auto format = ParseStatusFormat("{session} | {version}");
    const std::map<std::string, std::string> values = {{"version", "0.1.0"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    CHECK(Texts(parts) == std::vector<std::string>{"0.1.0"});
}

TEST_CASE("max_width <= 0 never clamps {session}", "[status_bar]") {
    const auto format = ParseStatusFormat("{session}");
    const std::map<std::string, std::string> values = {
        {"session", std::string(200, 'x')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/0);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].text.size() == 200);
}

TEST_CASE("{session} is capped at 24 columns even with plenty of room", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {session}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"}, {"session", std::string(200, 'x')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/500);
    REQUIRE(parts.size() == 3);
    CHECK(minimal::client::Utf8ColumnWidth(parts[2].text) == 24);
    CHECK(parts[2].text.back() != 'x');  // ellipsis, not a raw cut
}

TEST_CASE("other segments are NEVER truncated, only {session} gives way", "[status_bar]") {
    // The regression this guards: at 80 columns the old renderer shrank
    // EVERY child proportionally, including the token count. This asserts
    // the model/tokens/version/agent/permission_mode values survive intact
    // even when {session} alone is far too wide to fit.
    const auto format = ParseStatusFormat(
        "{version} | {session} | {tokens} | {model} | {agent} | {permission_mode}");
    const std::map<std::string, std::string> values = {
        {"version", "0.1.0"},
        {"session", "Reply with only a fenced code block tagged cpp containing: int main(){}"},
        {"tokens", "186/262144 tok"},
        {"model", "local/ornith-1.5-9b"},
        {"agent", "build"},
        {"permission_mode", "normal"}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/80);
    const auto texts = Texts(parts);
    CHECK(std::find(texts.begin(), texts.end(), "0.1.0") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "186/262144 tok") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "local/ornith-1.5-9b") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "build") != texts.end());
    CHECK(std::find(texts.begin(), texts.end(), "normal") != texts.end());
}

TEST_CASE("{session} is dropped, along with its leading separator, when there is no room "
          "at all", "[status_bar]") {
    const auto format = ParseStatusFormat("{version} | {session} | {tokens}");
    const std::map<std::string, std::string> values = {
        {"version", std::string(30, 'v')},
        {"session", "anything"},
        {"tokens", std::string(30, 't')}};
    const auto parts = ResolveStatusBar(format, values, /*max_width=*/60);
    CHECK(Texts(parts) == std::vector<std::string>{std::string(30, 'v'), " | ", std::string(30, 't')});
}
