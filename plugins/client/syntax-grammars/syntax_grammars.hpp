#pragma once

// syntax-grammars -- the bundled tree-sitter grammar set for code-block
// highlighting: cpp, python, javascript, typescript, bash, json, yaml, rust,
// go. Each language comes with its node-type table, its function-name
// positions and the fence labels that also mean it ("py", "sh", "yml").

#include <vector>

#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::syntax_grammars {

void RegisterGrammars(plugins::SyntaxGrammarRegistry& registry);

// The same set as the highlighter's table takes it, for tests that build a
// highlighter without a host.
std::vector<LanguageDefinition> Definitions();

}  // namespace minimal::client::syntax_grammars
