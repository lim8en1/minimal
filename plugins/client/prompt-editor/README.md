# prompt-editor

The prompt: where you type messages and /-commands.

## What it adds

The exclusive, required prompt-editor slot (`sdk/prompt_editor.hpp`
`PromptEditorFactory`): a multi-line editor with soft wrap, undo/redo,
up-arrow recall of submitted lines, bracketed paste, and inline suggestion
lists for `@`-mentions and `/`-commands. Its keys are listed in the keymap
help and rebindable in `keybindings.json`: `prompt.newline` (alt+enter),
`prompt.undo` (alt+z), `prompt.redo` (ctrl+alt+z), `prompt.clear`
(ctrl+alt+u).

Core supplies what it edits against -- the submit paths, the command catalog
for the focused tab, `@`-mention answers, the shared history -- so a
replacement editor (a vim mode, say) gets the same. To use one, disable this
plugin in `client-plugins.json` and enable it. With no editor the TUI refuses
to start.

## Configuration

None.
