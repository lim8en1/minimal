// syntax-grammars -- the bundled grammar set (syntax_grammars.hpp).

#include "minimal/client/sdk/client_plugin_abi.h"
#include "syntax_grammars.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("syntax-grammars")

void minimal_client_plugin_register_syntax_grammars(
    plugins::SyntaxGrammarRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    minimal::client::syntax_grammars::RegisterGrammars(registry);
}

}  // extern "C"
