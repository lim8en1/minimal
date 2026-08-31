#pragma once

// Server-side @<path> EXPANSION (ARCHITECTURE.md "@-mention file references").
// The complement to mention_resolver.hpp, which handles suggestion only.
//
// The shape is BLOCKS IN ONE MESSAGE, not one message per file:
//
//   Message{User, [
//     TextBlock{ "take a look and @a.md and @b.md" },   <- verbatim, tokens intact
//     TextBlock{ "<file path=\"a.md\">\n...\n</file>" },
//     TextBlock{ "<file path=\"b.md\">\n...\n</file>" },
//   ]}
//
// A User message with several TextBlocks is native on both wire formats -- the
// OpenAI provider concatenates them into one content string
// (openai_request.cpp), which is harmless because the framing is
// self-delimiting -- whereas consecutive User MESSAGES would be refused by the
// Anthropic Messages API's role alternation. One message also keeps
// AgentLoop::RunTurn's one-seed-message contract untouched.

#include <filesystem>
#include <string_view>

#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"

namespace minimal::agent {

// Scans `text` for mention tokens -- '@' at the start of the text or directly
// after whitespace, running to the next whitespace; a bare '@' is not a token,
// and `a@b.com` is untouched -- and returns the message to seed the turn with.
//
// The first block is the ORIGINAL text verbatim, tokens included. Each
// successfully resolved mention appends one framed block naming its path as
// typed:
//
//     <file path="a.md">
//     ...windowed content...
//     [truncated: showing lines 1-1000 of 4231 -- use the read tool for more]
//     </file>
//
// A directory mention lists a bounded tree instead:
//
//     <file-tree path="src/">
//     src-relative paths, one per line
//     </file-tree>
//
// NOT PermissionChecker-gated: the human typed the reference, exactly as if
// they had pasted the content. Paths outside project_root are therefore allowed
// too; only paths the filesystem cannot safely resolve at all (embedded NUL,
// symlink loop) fail, via path_confinement::confine fail-closing.
//
// A mention that cannot be resolved (typo, binary file, non-regular file)
// produces NO block and its token stays verbatim in the text -- the model sees
// the reference and can tell the user it does not exist. There is no error
// channel and no dedup: a mention means "read this now", so the same file
// mentioned twice, in one message or across the conversation, is read twice.
//
// No mentions -> a message byte-identical to the plain TextBlock wrap that used
// to be constructed inline, which is what makes this safe to call unconditionally.
llm::Message ExpandMentions(std::string_view text,
                            const std::filesystem::path& project_root,
                            const common::Logger& logger);

}  // namespace minimal::agent
