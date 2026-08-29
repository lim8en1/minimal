// tools-fs -- read / write / edit.
//
// A bundled plugin, using exactly the seams a third-party plugin uses. These tools
// are not structurally privileged; they are pre-installed and enabled by default.
//
// Path safety is NOT reimplemented here: these tools declare
// path_confinement_supported() and ExtractPath(), and core does the classification.
// A plugin tool must call the shared path_confinement utility, never roll its own.

#include "minimal/common/abi_tag.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-fs"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

// TODO: void minimal_plugin_register_tools(ToolRegistry&) -- Read, Write, Edit.
// Edit follows Anthropic's text_editor_20250728 vocabulary (view/create/
// str_replace/insert) so an Anthropic provider can later be handed the native
// schema-less tool without changing file-mutation semantics.

}  // extern "C"
