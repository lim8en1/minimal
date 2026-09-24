#include "stage_normalize.hpp"

#include <algorithm>
#include <array>

namespace minimal::tools_bash {

namespace {

// Whitespace-split words with quotes and backslash escapes removed. Not a
// shell parser: `$VAR` stays literal, globs stay literal. Good enough for
// the one question asked here -- "what program does this word name?".
std::vector<std::string> Words(std::string_view stage) {
    std::vector<std::string> words;
    std::string current;
    bool in_word = false;
    char quote = 0;
    for (std::size_t i = 0; i < stage.size(); ++i) {
        const char c = stage[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < stage.size()) {
                current += stage[++i];
            } else {
                current += c;
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == '\\' && i + 1 < stage.size()) {
            current += stage[++i];
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (in_word) words.push_back(std::move(current));
            current.clear();
            in_word = false;
        } else {
            current += c;
            in_word = true;
        }
    }
    if (in_word) words.push_back(std::move(current));
    return words;
}

std::string Join(const std::vector<std::string>& words, std::size_t from) {
    std::string out;
    for (std::size_t i = from; i < words.size(); ++i) {
        if (!out.empty()) out += ' ';
        out += words[i];
    }
    return out;
}

std::string Basename(const std::string& word) {
    const auto slash = word.find_last_of('/');
    return slash == std::string::npos || slash + 1 == word.size() ? word : word.substr(slash + 1);
}

// Wrappers: programs whose job is to run their argument as a program. For
// each, the flags that CONSUME the next word, so peeling does not mistake a
// flag's value for the wrapped program.
struct Wrapper {
    std::string_view name;
    std::array<std::string_view, 8> value_flags;
    // Positional words the wrapper itself takes before the program
    // (timeout's duration).
    int leading_args = 0;
};

constexpr std::array<Wrapper, 13> kWrappers{{
    {"sudo", {"-u", "-g", "-h", "-p", "-C", "-D", "-r", "-t"}, 0},
    {"doas", {"-u", "-C", "", "", "", "", "", ""}, 0},
    {"env", {"-u", "-C", "-S", "", "", "", "", ""}, 0},
    {"command", {"", "", "", "", "", "", "", ""}, 0},
    {"builtin", {"", "", "", "", "", "", "", ""}, 0},
    {"exec", {"-a", "", "", "", "", "", "", ""}, 0},
    {"nice", {"-n", "", "", "", "", "", "", ""}, 0},
    {"nohup", {"", "", "", "", "", "", "", ""}, 0},
    {"time", {"-f", "-o", "", "", "", "", "", ""}, 0},
    {"timeout", {"-s", "-k", "", "", "", "", "", ""}, 1},
    {"stdbuf", {"-i", "-o", "-e", "", "", "", "", ""}, 0},
    {"ionice", {"-c", "-n", "-p", "", "", "", "", ""}, 0},
    {"xargs", {"-I", "-n", "-P", "-L", "-d", "-s", "-E", "-a"}, 0},
}};

const Wrapper* FindWrapper(const std::string& program) {
    for (const auto& wrapper : kWrappers) {
        if (wrapper.name == program) return &wrapper;
    }
    return nullptr;
}

// Index of the wrapped program's word, or words.size() when there is none.
std::size_t PeelWrapper(const std::vector<std::string>& words, std::size_t at,
                        const Wrapper& wrapper) {
    std::size_t i = at + 1;
    int leading = wrapper.leading_args;
    while (i < words.size()) {
        const std::string& word = words[i];
        if (word == "--") return i + 1;
        if (!word.empty() && word[0] == '-') {
            const bool takes_value =
                std::find(wrapper.value_flags.begin(), wrapper.value_flags.end(), word) !=
                wrapper.value_flags.end();
            i += takes_value ? 2 : 1;
            continue;
        }
        // env's VAR=value assignments precede the program.
        if (wrapper.name == "env" && word.find('=') != std::string::npos) {
            ++i;
            continue;
        }
        if (leading > 0) {
            --leading;
            ++i;
            continue;
        }
        return i;
    }
    return words.size();
}

}  // namespace

std::vector<std::string> NormalizedStageVariants(std::string_view stage) {
    std::vector<std::string> variants;
    const std::string original(stage);
    auto add = [&](std::string variant) {
        if (variant.empty() || variant == original) return;
        if (std::find(variants.begin(), variants.end(), variant) != variants.end()) return;
        variants.push_back(std::move(variant));
    };

    std::vector<std::string> words = Words(stage);
    std::size_t at = 0;
    // Bounded: wrappers nest (`sudo env X=1 nice rm`), but never deeply.
    for (int depth = 0; depth < 6 && at < words.size(); ++depth) {
        add(Join(words, at));  // unquoted spelling
        std::vector<std::string> by_basename(words.begin() + static_cast<long>(at), words.end());
        by_basename[0] = Basename(by_basename[0]);
        add(Join(by_basename, 0));  // `/bin/rm x` -> `rm x`

        const Wrapper* wrapper = FindWrapper(Basename(words[at]));
        if (wrapper == nullptr) break;
        at = PeelWrapper(words, at, *wrapper);
    }
    return variants;
}

}  // namespace minimal::tools_bash
