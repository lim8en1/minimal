#pragma once

// Every registry the client offers plugins, in one struct: what the loader
// fills and the host reads.

#include "minimal/client/sdk/chat.hpp"
#include "minimal/client/sdk/registries.hpp"

namespace minimal::client::plugins {

// Every registry the client offers plugins. The host owns one; the loader
// fills it (RegistryHooks in client_plugin_loader.hpp maps each to its
// register_* hook).
struct ClientRegistries {
    ViewRegistry views;
    StartViewRegistry start_views;
    ClientCommandRegistry client_commands;
    ToolResultRendererRegistry tool_renderers;
    DisplayRendererRegistry display_renderers;
    PromptRendererRegistry prompt_renderers;
    MessageRendererRegistry message_renderers;
    StatusSegmentRegistry status_segments;
    SyntaxGrammarRegistry grammars;
    ObserverRegistry observers;
    ChatSurfaceRegistry chat_surface;
    MarkdownEngineRegistry markdown_engine;
    WidgetKitRegistry widget_kit;
    PromptEditorRegistry prompt_editor;
    TabBarRegistry tab_bar;
};

}  // namespace minimal::client::plugins
