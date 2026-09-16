#include "command_scanner.hpp"

#include <tree_sitter/api.h>
#include <tree_sitter/parser.h>  // the vendored grammar's tree_sitter_bash()

#include <algorithm>
#include <memory>
#include <vector>
#include <string>
#include <utility>

extern "C" const TSLanguage* tree_sitter_bash(void);

namespace minimal::tools_bash {
namespace {

// Builtins that execute text in the current shell, so the program they run is
// not a literal anywhere in the string. Listing them is not an interpreter
// denylist: `bash -c '...'` keeps `bash` as its program, which resolves to a
// path and is therefore grantable, whereas these have no program at all.
bool RunsUnnamedCode(std::string_view word) {
    return word == "eval" || word == "." || word == "source" || word == "exec";
}

// Shell reserved words that THIS GRAMMAR leaves unstructured: `time` and
// `coproc` parse as a plain command_name with the real program demoted to an
// argument. The rest of the reserved set never appears as a command_name in a
// well-formed tree -- the grammar models `if`/`while`/`case`/... as their own
// node types -- so listing them here only matters for misplaced keywords,
// where there is no real program to lose. The whole set is kept (rather than
// just time/coproc) so the answer does not depend on the grammar's coverage
// of each keyword staying silently the same.
bool IsReservedWord(std::string_view word) {
    static constexpr std::string_view kReserved[] = {
        "!",    "[[",       "]]", "case",   "coproc", "do",   "done",
        "elif", "else",     "esac", "fi",   "for",    "function",
        "if",   "in",       "select", "then", "time", "until", "while",
    };
    return std::find(std::begin(kReserved), std::end(kReserved), word) != std::end(kReserved);
}

struct ParserDeleter {
    void operator()(TSParser* p) const { ts_parser_delete(p); }
};
struct TreeDeleter {
    void operator()(TSTree* t) const { ts_tree_delete(t); }
};

void Record(ScanResult& out, std::string word) {
    if (word.empty()) return;
    if (std::find(out.command_words.begin(), out.command_words.end(), word) ==
        out.command_words.end()) {
        out.command_words.push_back(std::move(word));
    }
}

std::string_view NodeText(TSNode node, std::string_view src) {
    const auto start = ts_node_start_byte(node);
    return src.substr(start, ts_node_end_byte(node) - start);
}

std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// A redirect that WRITES, vs one inert to a file-permission model. The
// operator is an ANONYMOUS child of file_redirect, so it is invisible to
// named-child iteration: `2>&1`/`>&-` name only file descriptors; `< f` is
// input-only; `2>/dev/null` names the one file nobody wants a rule about.
// Everything else -- `> f`, `>> f`, `&> f`, `2> f` -- is treated as a write.
// An expansion target or unknown shape is a write too: this is best-effort
// classification feeding the safe direction.
bool RedirectWrites(TSNode redirect, std::string_view src) {
    const std::uint32_t all = ts_node_child_count(redirect);
    // First pass over ALL children (the operator is anonymous): find the
    // operator and, for output ops, the destination word next to it.
    std::optional<std::string_view> output_op;
    for (std::uint32_t i = 0; i < all; ++i) {
        const TSNode child = ts_node_child(redirect, i);
        if (ts_node_is_named(child)) continue;
        const std::string_view op = NodeText(child, src);
        if (op == "<") {
            return false;  // input redirect: opens for reading, writes nothing
        }
        if (op == ">&" || op == ">&-") continue;  // fd-dup / fd-close: inert
        if (op == ">" || op == ">>" || op == "&>" || op == "&>>" || op == ">|") {
            output_op = op;
            break;
        }
    }
    if (!output_op) return false;  // no redirect operator at all (or fd-only)
    // Output operator: inert only when the destination is /dev/null. The
    // destination is the next named child after the operator (word/expansion);
    // an fd-dup NUMBER target was excluded above by the operator set.
    const std::uint32_t n = ts_node_named_child_count(redirect);
    for (std::uint32_t i = 0; i < n; ++i) {
        const TSNode child = ts_node_named_child(redirect, i);
        const std::string type = ts_node_type(child);
        if (type == "file_descriptor") continue;
        if (type == "word" && NodeText(child, src) == "/dev/null") return false;
        return true;  // destination exists and is a real file (or unreadable shape)
    }
    return true;  // operator with no recognizable destination: assume a write
}

// The node kinds a command_name (or one of its children) may be built from
// while still spelling a literal program. Anything else -- simple_expansion,
// command_substitution, variable_name, ... -- means the program is computed,
// not named. An allow-list, because the grammar invents new node kinds
// faster than any deny-list would be updated.
bool IsLiteralKind(std::string_view type) {
    return type == "word" || type == "string" || type == "raw_string" ||
           type == "ansi_c_string" || type == "string_content" ||
           type == "concatenation" || type == "number";
}

// True iff `node`'s subtree is entirely literal kinds; the dequoted text of
// the program it spells is appended to `out_text` either way (ignored by the
// caller when this returns false). Depth-capped: a literal is bounded by the
// nesting inside ONE command_name (concatenation/string structure), which is
// small; the cap turns a pathological input into "computed, not named" --
// complete = false -- instead of unbounded recursion.
bool CollectLiteral(TSNode node, std::string_view src, std::string& out_text, int depth = 0) {
    constexpr int kMaxLiteralDepth = 64;
    if (depth > kMaxLiteralDepth) return false;
    const std::string type = ts_node_type(node);
    if (!IsLiteralKind(type)) return false;

    const std::uint32_t n = ts_node_named_child_count(node);
    if (n == 0) {
        // A leaf literal. Quoted kinds strip their quotes: `$'rm'` must name
        // rm, or a program spelled in quotes could never be granted.
        const std::string_view text = NodeText(node, src);
        if (type == "string" || type == "raw_string") {
            if (text.size() >= 2) out_text.append(text.substr(1, text.size() - 2));
        } else if (type == "ansi_c_string") {
            if (text.size() >= 3) out_text.append(text.substr(2, text.size() - 3));
        } else {
            out_text.append(text);
        }
        return true;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!CollectLiteral(ts_node_named_child(node, i), src, out_text, depth + 1)) {
            return false;
        }
    }
    return true;
}

class Walker {
public:
    Walker(std::string_view src, ScanResult& out) : src_(src), out_(out) {}

    // An explicit-stack traversal, not recursion. tree-sitter-bash builds
    // LEFT-DEEP trees for operator chains (`a && b && c && ...`): depth is
    // linear in the term count, so a long command string -- one tool call, the
    // model's to write -- overflowed the session worker's stack. The overflow
    // is a SIGSEGV, uncatchable by the executor's Run guard; iteration makes
    // the failure mode unreachable rather than bounded (the RE2 lesson).
    void Run(TSNode root) {
        std::vector<TSNode> stack{root};
        while (!stack.empty()) {
            const TSNode node = stack.back();
            stack.pop_back();
            const std::string type = ts_node_type(node);
            const std::uint32_t n = ts_node_named_child_count(node);
            if (type == "command") RecordStage(node);
            if (type == "file_redirect" && RedirectWrites(node, src_)) out_.allow_eligible = false;
            if (type == "command_name") {
                if (HandleCommandName(node)) continue;
                // The program is computed, not named ($X args, foo$(nproc)
                // bar) -- nothing meaningful to grant. The subtree may still
                // contain command_substitutions whose commands ARE literal,
                // so keep walking: `echo `id`` names id even where the
                // program slot is dynamic. The descent shares THIS stack --
                // calling Run recursively here would put one C++ frame back
                // per substitution-nesting level, the same overflow the
                // iterative walk exists to make unreachable.
                out_.complete = false;
            }
            // Pushed in reverse so children are VISITED in order: disclosure
            // order is the command's order, and tests assert on it.
            for (std::uint32_t i = n; i-- > 0;) stack.push_back(ts_node_named_child(node, i));
        }
    }

    // One stage per `command` node, wherever the grammar puts one -- top level,
    // pipeline stage, subshell, do-group, command substitution. Stage text is
    // the node's raw text with leading variable_assignments stripped (an env
    // prefix is not part of the program a rule names) and a reserved word in
    // the command slot (`time`, `coproc`) dropped the same way command_words
    // demotes it. Substitution stages included: the allow conjunction must
    // cover EVERY program the parse names, which is exactly the grant model.
    void RecordStage(TSNode node) {
        const std::uint32_t n = ts_node_named_child_count(node);
        std::uint32_t first = 0;
        while (first < n &&
               std::string_view(ts_node_type(ts_node_named_child(node, first))) ==
                   "variable_assignment") {
            ++first;
        }
        if (first < n &&
            std::string_view(ts_node_type(ts_node_named_child(node, first))) ==
                "command_name") {
            // A quoted spelling ('time' make) is NOT the reserved word -- trim
            // before comparing, or a quoted program name would be demoted.
            const std::string_view name = Trim(NodeText(ts_node_named_child(node, first), src_));
            if (name.find_first_of("'\"") != std::string_view::npos) {
                // Quoted: treat as a program argument word, never a keyword.
            } else if (IsReservedWord(name)) {
                ++first;  // `time`/`coproc`: not the program
            }
        }
        if (first >= n) return;  // bare assignments: no stage to speak of
        // The stage is the WHOLE remainder of the command node -- program and
        // arguments -- not just the program word: "git status" must come out
        // of `git status -C x`, or argument-bearing rules could never match.
        // Named children only, so inter-child trivia (the space between words)
        // is skipped and the span starts at the first meaningful child.
        const TSNode first_child = ts_node_named_child(node, first);
        const TSNode last_child = ts_node_named_child(node, n - 1);
        const std::string_view span(
            src_.substr(ts_node_start_byte(first_child),
                        ts_node_end_byte(last_child) - ts_node_start_byte(first_child)));
        const std::string_view trimmed = Trim(span);
        if (trimmed.empty()) return;
        if (std::find(out_.stages.begin(), out_.stages.end(), trimmed) == out_.stages.end()) {
            out_.stages.emplace_back(trimmed);
        }
    }

private:
    // Returns true when the name was handled as a literal (recorded, or
    // reserved-word arguments recorded); false means "computed, not named" and
    // the CALLER descends into the children on the shared stack.
    bool HandleCommandName(TSNode node) {
        std::string text;
        bool literal = true;
        const std::uint32_t n = ts_node_named_child_count(node);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!CollectLiteral(ts_node_named_child(node, i), src_, text)) {
                literal = false;
                break;
            }
        }
        if (!literal) return false;
        HandleLiteralName(node, std::move(text));
        return true;
    }

    void HandleLiteralName(TSNode node, std::string text) {
        if (RunsUnnamedCode(text)) {
            Record(out_, std::move(text));
            out_.complete = false;
            return;
        }
        if (IsReservedWord(text)) {
            // time/coproc sit in the command slot without being commands, and
            // this grammar does not structure them, so the real program is
            // among the arguments. Recording every literal argument
            // over-discloses (non-programs like `-rf` fail PATH resolution
            // and contribute no grant offer), which errs toward more keys in
            // the conjunction -- the direction this design already accepts
            // everywhere else.
            const TSNode parent = ts_node_parent(node);
            const std::uint32_t n = ts_node_named_child_count(parent);
            for (std::uint32_t i = 0; i < n; ++i) {
                const TSNode child = ts_node_named_child(parent, i);
                if (child.id == node.id) continue;  // the reserved word itself
                std::string arg;
                if (CollectLiteral(child, src_, arg)) Record(out_, std::move(arg));
            }
            return;
        }
        // An empty spelling ('' foo) records nothing and runs nothing: Record
        // skips empty, and the empty-words rule at the bottom of ScanCommand
        // fails the whole disclosure closed.
        Record(out_, std::move(text));
    }

    std::string_view src_;
    ScanResult& out_;
};

}  // namespace

ScanResult ScanCommand(std::string_view command) {
    ScanResult result;

    // A parser per call: commands are small, Disclose is per-tool-call rather
    // than hot, and a shared parser would need a lock for no measurable gain.
    // All tree-sitter state here is call-local, so concurrent Disclose calls
    // from different session workers cannot touch each other.
    std::unique_ptr<TSParser, ParserDeleter> parser(ts_parser_new());
    if (!parser || !ts_parser_set_language(parser.get(), tree_sitter_bash())) {
        // A grammar the core runtime cannot load (ABI drift after an upgrade)
        // makes every disclosure incomplete: every bash call prompts. The
        // safe degradation -- and loud, since every complete-checking test
        // fails with it.
        result.complete = false;
        return result;
    }
    std::unique_ptr<TSTree, TreeDeleter> tree(ts_parser_parse_string(
        parser.get(), nullptr, command.data(), static_cast<std::uint32_t>(command.size())));
    if (tree) {
        // THE POSTURE THIS OWES ITS CALLER: tree-sitter is error-recovering --
        // built for editors, it produces a tree for input it does not
        // understand rather than refusing. So the grammar does not hand you
        // correctness for free: a parse containing ERROR or MISSING nodes is a
        // first-class "I did not understand this", and it maps onto
        // complete = false. has_error covers the whole tree including the
        // unnamed tokens (a missing `)` or `fi`) that a named-children walk
        // never visits. Skipping this check would trade the known-shape bug
        // of the hand-rolled scanner for an unknown-shape one.
        if (ts_node_has_error(ts_tree_root_node(tree.get()))) result.complete = false;
        Walker(command, result).Run(ts_tree_root_node(tree.get()));
    } else {
        result.complete = false;
    }

    // Nothing nameable at all -- an empty command, one made entirely of
    // constructs we could not resolve, or one whose only "command" is grammar.
    // Either way there is nothing to grant, and a complete disclosure with
    // nothing in it would satisfy the conjunction vacuously.
    if (result.command_words.empty()) result.complete = false;

    // Allow rules act on stages only when the parse named every program and
    // nothing writes a file. Anything shakier degrades to whole-line matching
    // for ask/deny and to the category default for allow -- never to a wider
    // allow than the operator wrote.
    if (!result.complete || result.stages.empty()) result.allow_eligible = false;
    return result;
}

}  // namespace minimal::tools_bash
