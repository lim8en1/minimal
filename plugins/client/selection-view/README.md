# selection-view

The choosers for what a session runs with.

## What it adds

- **View** `agents` (modal): the project's agent presets
  (`ClientApi::ListAgentPresets`), each in its color. Enter chooses
  (`HostActions::ChooseAgent`).
- **View** `models` (modal): the server's models through the widget kit's
  model selector (ctrl+r refreshes discovery; a typed `<provider>/<model>`
  the list lacks is offered too), with a `default (<ref>)` row when the
  server names a default. Enter chooses (`ChooseModel` /
  `ChooseDefaultModel`).
- **View** `subagent-models` (modal): the same list for the focused session's
  subagent model, with a `same as parent` row (`SetSubagentModel`).

The view ids are roles: core opens them for `/agent`, `/model`,
`/subagent-model`, `alt+g` and `alt+m`, and for a first message that needs a
choice before it can create its session; the server's view hints name them
too. What a choice does -- staging the tab's desired agent or model, the
notice, sending the waiting message -- is core's.

## Configuration

None.
