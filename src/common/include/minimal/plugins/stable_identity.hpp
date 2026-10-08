#pragma once

// A registration's STABLE identity, "plugin.original_name" (core
// registrations have an empty plugin name, giving ".name"): what
// client-plugins.json / plugins.json `aliases` are keyed by. Split out of
// name_table.hpp so an SDK registry can key aliases without the host's
// collision machinery (ARCHITECTURE.md, "Name collisions and aliases").

#include <string>
#include <string_view>

namespace minimal::plugins {

std::string StableIdentity(std::string_view plugin_name, std::string_view original_name);

}  // namespace minimal::plugins
