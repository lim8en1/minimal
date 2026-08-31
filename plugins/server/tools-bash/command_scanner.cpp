#include "command_scanner.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace minimal::tools_bash {
namespace {

constexpr int kMaxDepth = 8;  // substitution nesting; deeper is pathological

// Builtins that execute text in the current shell, so the program they run is
// not a literal anywhere in the string. Listing them is not an interpreter
// denylist: `bash -c '...'` keeps `bash` as its program, which resolves to a
// path and is therefore grantable, whereas these have no program at all.
bool RunsUnnamedCode(std::string_view word) {
    return word == "eval" || word == "." || word == "source" || word == "exec";
}

// Shell reserved words. These sit in a command position without BEING commands:
// in `if true; then rm -rf /; fi` the program of the second list is `rm`, and
// `then` is grammar.
//
// Recording them as programs was not merely untidy, it was unsound. Consuming
// the command slot made the real program an argument, so `rm` was never named --
// while `complete` stayed true, because nothing here had failed. A held grant on
// some other, harmless program in the same string then satisfied the whole
// conjunction and the command ran unprompted. Skipping them leaves
// `expecting_command_` set, so the next word is scanned as the program it is.
//
// The list is deliberately the WHOLE reserved set rather than the subset that
// is followed by a command list. `for`/`in`/`[[` are followed by a name, a word
// list and an expression respectively, so skipping them records one extra word
// -- `f`, `*.txt`, `-f` -- that resolves to no binary and is dropped at
// disclosure. That is the same over-approximation ResolveOnPath already accepts,
// and it errs toward more keys in the conjunction, never fewer.
bool IsReservedWord(std::string_view word) {
    static constexpr std::string_view kReserved[] = {
        "!",    "[[",       "]]", "case",   "coproc", "do",   "done",
        "elif", "else",     "esac", "fi",   "for",    "function",
        "if",   "in",       "select", "then", "time", "until", "while",
    };
    return std::find(std::begin(kReserved), std::end(kReserved), word) != std::end(kReserved);
}

bool IsAssignmentPrefix(std::string_view word) {
    const auto eq = word.find('=');
    if (eq == std::string_view::npos || eq == 0) return false;
    if (!std::isalpha(static_cast<unsigned char>(word.front())) && word.front() != '_') return false;
    for (std::size_t i = 0; i < eq; ++i) {
        const char c = word[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

// Index just past the `)` matching the `(` that `open` points at, or npos.
// Quote-aware, because `$(echo ")")` must not close early.
std::size_t MatchingParen(std::string_view s, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'' || c == '"') {
            const char quote = c;
            for (++i; i < s.size(); ++i) {
                if (s[i] == '\\' && quote == '"' && i + 1 < s.size()) { ++i; continue; }
                if (s[i] == quote) break;
            }
            if (i >= s.size()) return std::string_view::npos;  // unterminated
            continue;
        }
        if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
        if (c == '(') ++depth;
        if (c == ')' && --depth == 0) return i;
    }
    return std::string_view::npos;
}

void Scan(std::string_view s, ScanResult& out, int depth);

void Record(ScanResult& out, std::string word) {
    if (word.empty()) return;
    if (std::find(out.command_words.begin(), out.command_words.end(), word) ==
        out.command_words.end()) {
        out.command_words.push_back(std::move(word));
    }
}

class Walker {
public:
    Walker(std::string_view s, ScanResult& out, int depth) : s_(s), out_(out), depth_(depth) {}

    void Run() {
        for (std::size_t i = 0; i < s_.size(); ++i) {
            const char c = s_[i];

            if (c == '\'') {  // literal, no substitution
                const auto close = s_.find('\'', i + 1);
                if (close == std::string_view::npos) { out_.complete = false; return; }
                Append(s_.substr(i + 1, close - i - 1));
                i = close;
                continue;
            }
            if (c == '"') {
                i = ScanDoubleQuoted(i);
                if (i == std::string_view::npos) { out_.complete = false; return; }
                continue;
            }
            if (c == '\\') {
                if (++i < s_.size()) Append(s_.substr(i, 1));
                continue;
            }
            if (c == '$' || c == '`') {
                const auto next = ScanDollarOrBacktick(i, /*in_double_quotes=*/false);
                if (next == std::string_view::npos) { out_.complete = false; return; }
                i = next;
                continue;
            }
            if (c == ' ' || c == '\t') { Flush(); continue; }
            if (IsSeparator(c)) {
                Flush();
                expecting_command_ = true;
                redirect_pending_ = false;
                continue;
            }
            if (c == '<' || c == '>') {
                Flush();
                // Consume the whole operator (">>", "<<", "<<-", ">&") so its
                // operand -- a filename or heredoc delimiter, never a program --
                // can be dropped without disturbing `expecting_command_`.
                while (i + 1 < s_.size() && (s_[i + 1] == '>' || s_[i + 1] == '<' ||
                                             s_[i + 1] == '&' || s_[i + 1] == '-')) {
                    ++i;
                }
                redirect_pending_ = true;
                continue;
            }
            Append(s_.substr(i, 1));
        }
        Flush();
    }

private:
    static bool IsSeparator(char c) {
        return c == '|' || c == '&' || c == ';' || c == '\n' || c == '\r' || c == '(' ||
               c == ')' || c == '{' || c == '}';
    }

    void Append(std::string_view text) {
        word_ += text;
        has_word_ = true;
    }

    void Flush() {
        if (!has_word_) return;
        const std::string word = std::move(word_);
        word_.clear();
        has_word_ = false;
        const bool began_with_expansion = std::exchange(word_began_with_expansion_, false);

        if (redirect_pending_) {  // a redirection operand, not a program
            redirect_pending_ = false;
            return;
        }
        if (!expecting_command_) return;  // an argument

        if (began_with_expansion) {
            // `$X args` -- the program is not a literal, so there is nothing to
            // name and nothing meaningful to grant.
            out_.complete = false;
            expecting_command_ = false;
            return;
        }
        if (IsAssignmentPrefix(word)) return;  // `FOO=1 cmd`: still expecting
        if (IsReservedWord(word)) return;      // grammar, not a program: still expecting

        if (RunsUnnamedCode(word)) out_.complete = false;
        Record(out_, word);
        expecting_command_ = false;
    }

    // Returns the index of the closing quote, or npos if unterminated.
    std::size_t ScanDoubleQuoted(std::size_t open) {
        for (std::size_t i = open + 1; i < s_.size(); ++i) {
            const char c = s_[i];
            if (c == '\\' && i + 1 < s_.size()) {
                Append(s_.substr(++i, 1));
                continue;
            }
            if (c == '"') { has_word_ = true; return i; }
            if (c == '$' || c == '`') {
                const auto next = ScanDollarOrBacktick(i, /*in_double_quotes=*/true);
                if (next == std::string_view::npos) return std::string_view::npos;
                i = next;
                continue;
            }
            Append(s_.substr(i, 1));
        }
        return std::string_view::npos;
    }

    // Handles `$(...)`, backticks, `$((...))`, `${...}` and bare `$name`.
    // Returns the index of the last character consumed, or npos on a malformed
    // construct. Substitutions RECURSE: they are a nested command context, so
    // `make -j$(nproc)` names both make and nproc rather than giving up.
    std::size_t ScanDollarOrBacktick(std::size_t i, bool in_double_quotes) {
        const bool at_word_start = !has_word_;

        if (s_[i] == '`') {
            const auto close = s_.find('`', i + 1);
            if (close == std::string_view::npos) return std::string_view::npos;
            Recurse(s_.substr(i + 1, close - i - 1));
            MarkExpansion(at_word_start);
            return close;
        }

        // `$((` arithmetic: not a command context, and must be tested before `$(`.
        if (s_.compare(i, 3, "$((") == 0) {
            const auto close = s_.find("))", i + 3);
            if (close == std::string_view::npos) return std::string_view::npos;
            MarkExpansion(at_word_start);
            return close + 1;
        }
        if (s_.compare(i, 2, "$(") == 0) {
            const auto close = MatchingParen(s_, i + 1);
            if (close == std::string_view::npos) return std::string_view::npos;
            Recurse(s_.substr(i + 2, close - i - 2));
            MarkExpansion(at_word_start);
            return close;
        }
        if (s_.compare(i, 2, "${") == 0) {  // parameter expansion, not a command
            const auto close = s_.find('}', i + 2);
            if (close == std::string_view::npos) return std::string_view::npos;
            MarkExpansion(at_word_start);
            return close;
        }

        // `$'...'` (ANSI-C quoting) and `$"..."` (locale lookup) are QUOTED
        // LITERALS, not expansions. The bare-`$name` fallthrough below consumed
        // the `$` and the OPENING quote as one two-character token, which left
        // the closing quote to open a fresh quote region and swallow the rest of
        // the line. Handled here, `$'rm'` is the literal word it is -- so this
        // deliberately does not MarkExpansion: there is a program named, and
        // calling it an expansion would report the disclosure incomplete for a
        // string that names its program perfectly well.
        if (s_.compare(i, 2, "$'") == 0) {
            for (std::size_t j = i + 2; j < s_.size(); ++j) {
                if (s_[j] == '\\' && j + 1 < s_.size()) {
                    Append(s_.substr(++j, 1));
                    continue;
                }
                if (s_[j] == '\'') {
                    has_word_ = true;  // `$''` is an empty word, but still a word
                    return j;
                }
                Append(s_.substr(j, 1));
            }
            return std::string_view::npos;  // unterminated: fails closed
        }
        if (s_.compare(i, 2, "$\"") == 0) return ScanDoubleQuoted(i + 1);

        // Bare `$name` / `$1` / `$?`.
        std::size_t end = i + 1;
        while (end < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[end])) ||
                                   s_[end] == '_')) {
            ++end;
        }
        if (end == i + 1 && end < s_.size()) ++end;  // single-char special like $? or $1
        MarkExpansion(at_word_start);
        (void)in_double_quotes;
        return end - 1;
    }

    void MarkExpansion(bool at_word_start) {
        has_word_ = true;  // an expansion produces a word even if it is empty
        if (at_word_start && expecting_command_ && !redirect_pending_) {
            word_began_with_expansion_ = true;
        }
    }

    void Recurse(std::string_view inner) {
        if (depth_ >= kMaxDepth) {
            out_.complete = false;
            return;
        }
        Scan(inner, out_, depth_ + 1);
    }

    std::string_view s_;
    ScanResult& out_;
    int depth_;

    std::string word_;
    bool has_word_ = false;
    bool expecting_command_ = true;
    bool redirect_pending_ = false;
    bool word_began_with_expansion_ = false;
};

void Scan(std::string_view s, ScanResult& out, int depth) { Walker(s, out, depth).Run(); }

}  // namespace

ScanResult ScanCommand(std::string_view command) {
    ScanResult result;
    Scan(command, result, 0);
    // Nothing nameable at all -- an empty command, or one made entirely of
    // constructs we could not resolve. Either way there is nothing to grant.
    if (result.command_words.empty()) result.complete = false;
    return result;
}

}  // namespace minimal::tools_bash
