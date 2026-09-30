#pragma once

// The client-side half of the /-command surface, as ONE table. A client
// command is either the client's own (/new, /rewind, /theme: listed in the
// catalog, never sent to the server) or an intercept of a server command's
// particular forms (bare /agent opens the picker; /help <name> still goes to
// the server). RunTui used to keep the listed ones in a hand-maintained
// catalog vector and their behavior in a separate 260-line if-chain -- a
// command added to one and not the other was either undiscoverable or dead.

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "minimal/client/command_catalog.hpp"
#include "minimal/client/command_line.hpp"

namespace minimal::client {

struct ClientCommand {
    std::string name;
    // Non-empty: the command is the CLIENT's, offered in /-autocomplete and
    // the /help overlay with this summary. Empty: a server command whose
    // `run` only intercepts some forms (the catalog lists the server's own
    // entry for it).
    std::string summary;
    bool read_only = false;
    bool requires_session = false;
    // Runs the command. Returns false to pass the line on (the server path)
    // -- how an intercept declines the forms it does not own.
    std::function<bool(const CommandInvocationText& parsed)> run;
};

class CommandRouter {
public:
    // A later Add of the same name replaces the earlier one.
    void Add(ClientCommand command);
    bool Has(const std::string& name) const { return commands_.count(name) != 0; }

    // The listed (summary-bearing) commands, as catalog entries, in name
    // order -- what CommandCatalogCache appends to every server listing.
    std::vector<CommandCatalogEntry> CatalogEntries() const;

    // Runs the command `line` names, if the client has one. True when it
    // handled the line; false when the line is not a /-command, names no
    // client command, or the command passed it on.
    bool Dispatch(const std::string& line) const;

private:
    std::map<std::string, ClientCommand> commands_;
};

}  // namespace minimal::client
