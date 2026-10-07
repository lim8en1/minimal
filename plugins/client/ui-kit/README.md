# ui-kit

The widget kit: what every plugin view draws with.

## What it adds

The exclusive, required widget-kit slot (`registries.hpp` `WidgetKit`): the
filterable menu, the schema form, the model selector and the modal chrome.
Views reach it through `ClientPluginRegistrationContext::widgets`, never by
naming this plugin.

To restyle every view at once, disable this plugin in `client-plugins.json`
and enable one that registers another kit. With no kit at all the TUI
refuses to start.

## Configuration

None.
