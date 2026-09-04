#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/session_naming.hpp"

using namespace minimal;

TEST_CASE("names are whitespace-collapsed and truncated at 60 code points", "[session_naming]") {
    CHECK(agent::DefaultSessionName("  Fix   the\nlogin\tbug  ") == "Fix the login bug");

    // ASCII: exactly 60 characters survive.
    CHECK(agent::DefaultSessionName(std::string(70, 'a')) == std::string(60, 'a'));

    // UTF-8: truncation counts code points, never splitting a sequence.
    std::string long_unicode;
    for (int i = 0; i < 70; ++i) long_unicode += "\xC3\xA9";  // é × 70
    CHECK(agent::DefaultSessionName(long_unicode) == long_unicode.substr(0, 120));  // 60 × 2 bytes
}

TEST_CASE("an empty candidate collapses to an empty name", "[session_naming]") {
    CHECK(agent::DefaultSessionName("").empty());
    CHECK(agent::DefaultSessionName("   \n\t  ").empty());
}
