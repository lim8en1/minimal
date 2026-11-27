# node

Runs an agent preset as a pipeline step with JSON contracts. JSON goes in,
the agent works, and one JSON value comes out. Each end is checked against a
JSON Schema (draft-7) that the preset declares.

```sh
echo '{"text":"Total 12.50 EUR"}' \
  | minimal --agent node-extract -p - --output-format result
# {"currency":"EUR","total":12.5}
```

## What it adds

- **`submit_result` tool**:
  - A preset definition builder swaps in the preset's `output_schema` as the
    tool's input schema. The schema is sent `strict` when it is closed and
    fully required at every level, so providers enforce it while decoding.
  - Every call is validated against the same schema. Violations come back to
    the model as an error result listing each JSON-pointer path, and it
    retries.
  - A valid call finishes the turn: its input becomes the turn's structured
    result (`TurnCompletedEvent.result_json`).
- **Input gate** (`minimal_plugin_register_turn_gates`): for a preset with an
  `input_schema`, the turn's prompt must parse as JSON and satisfy that schema.
  Otherwise the turn is rejected before anything runs or is persisted
  (`STOP_REASON_REJECTED`; `minimal -p` exits 65).

Both schemas compile, and are checked against the draft-7 meta-schema, when
the preset is resolved. A broken contract fails the preset when it is
selected, not on the first turn. Schemas must be self-contained: a `$ref`
outside the schema is refused.

## Configuration

Per preset, in the agent's frontmatter. There is no `plugins.d` file.

```yaml
tool_choice: required          # core: the model cannot end a round in prose
allowed_tools: [submit_result] # plus whatever the node may use first
tool_config:
  node:
    input_schema:  {...}       # optional: the prompt's contract
    output_schema: {...}       # optional: the result's contract (enables submit_result)
    description: "..."         # optional: appended to submit_result's description
```

A preset without `tool_config.node` is not a node, and the plugin does nothing
for it. See `examples/agents/node-extract.md`.

- `tool_choice: required` is core's preset key, not this plugin's. Without it
  the model may answer in prose. `--output-format result` then exits 6 ("no
  structured result").
- With `tool_choice: required`, the turn ends only through a valid
  `submit_result` or the turn ceiling.
- Anthropic rejects `tool_choice: required` together with extended thinking.
  Don't enable thinking on a node preset there.

The input gate applies to top-level turns, i.e. headless runs and TUI turns.
A node spawned as a subagent gets its task from its parent model, unchecked.
Its `submit_result` contract still holds.

## Disabled

`submit_result` and the gate are gone. A node preset that lists
`submit_result` in `allowed_tools` simply doesn't get it, and
`--output-format result` exits 6.
