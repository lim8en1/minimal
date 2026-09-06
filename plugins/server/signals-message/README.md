# signals-message

The bundled `message_session` tool: cross-session messaging over the core
signals service — the v1 of agent-to-agent exchange.

## What it adds

- **Tool**: `message_session` — sends a message from one session to another.
- Gated twice: the sender by this tool's permission mode (default Ask), the
  receiver by its own "sessions" subscription at start.

## Configuration

None. Ships ENABLED — unlike session-titler it spends no tokens unprompted.
