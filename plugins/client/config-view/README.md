# config-view

Plugin configuration in the TUI: `/config`.

## What it adds

- **View** `config` (modal): every plugin, server and TUI, each on or off
  with what it provides; space turns one on or off for its next start
  (`plugins.json` / `client-plugins.json`) -- refused, with the reason, for
  the last provider providers.json's models need, and for what the TUI
  cannot start without (chat-view, prompt-editor, ui-kit, consent-prompts).
  Enter on a configurable server plugin opens its form — every field with its value (`*` = set in the file, the rest
  are defaults), whether it applies **live** or on **restart**, and
  **restart pending** where the file differs from what the running plugin
  uses; a field the current mode does not use (memory's http fields under
  `strategy: sidecar`) is dimmed and tagged **not used**, still editable. The
  selected field's type, default, range, doc and, when unused, why show below.
- **Core** rows above the plugins: core config files with a form of their
  own — `permissions.json` (docs/CONFIGURATION.md). Same form and keys as a
  plugin's, no on/off (space says so), every change on **restart**. A
  running plugin that a core field could name but does not (an advisor
  plugin `permissions.json` does not choose) says `not the advisor in
  permissions.json` on its row.
- **Command** `/config [plugin]` — opens the list, or straight onto a
  plugin's form.

Keys in the form: up/down (`k`/`j`) select; enter toggles a boolean, opens a
chooser for an enum (its choices -- space steps to the next one instead), a
model ref (the server's models, an "empty" row that unsets it, and any typed
`<provider>/<model>` the list does not have) or a model id (the same list,
answering the name the provider is sent), or edits a value inline (enter
saves, esc cancels; a secret is typed masked); `d` resets a field to its
default (a set secret asks twice); left or esc goes back to the list
(Backspace does not: a habitual delete right after the editor closed used to
leave the form); esc on the list closes.

Structured values open as forms of their own, level by level, edited as a
draft and saved once, when the first level is left (`x` discards a level):

- a **list** (rate_sources) shows a row per entry and an add row; `d` twice
  deletes one, ctrl+up/down (`K`/`J`) moves it;
- a **map** (rates) shows a row per entry, by name; its form starts with the
  name -- chosen like any field (a model id from the model list) -- and a
  name another entry has is refused;
- anything **without a schema** (decision-advisor's rubric) opens as a form
  inferred from its value: each key or item typed by what it holds, nested
  objects and lists one Enter further in; `a` adds a key (its name, then a
  value -- JSON when it parses, text otherwise) or an item, `d` twice deletes
  one, `K`/`J` move an item; on the plugin's form, `e` edits the whole value
  as one line of JSON instead.

A plugin with a config check (docs/PLUGINS.md) is checked after every save
and on `c`: a line above the fields says "checking..." and then what was
found, each finding marks its field (✓ fine, ⚠ worth a look, ✗ a problem)
and the selected field's finding is shown below it; a list or map entry
carries the worst of its fields'. The plugin list shows a plugin's failed
checks too.

A core file's edits take the same path, checked by the parser the server
starts with and written to the file itself (never removed: `{}` at the least).

Edits go to core's `plugins` endpoint: the plugin's own parser checks the
value (a refusal shows in red, nothing is written), the file
`plugins.d/<name>.json` is written, and the change applies live when every
changed field is live — otherwise the form says to restart the server.

## Configuration

None.

## Built on

The widget kit (`ClientPluginRegistrationContext::widgets`): its schema form
and filterable menu. The form is reusable by any view with a
`describe`-shaped description —
`{schema, config, pending}`, the schema being what
`minimal_plugin_config_schema` records.
