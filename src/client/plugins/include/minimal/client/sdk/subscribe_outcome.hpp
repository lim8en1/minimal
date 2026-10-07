#pragma once

// Outcome of a PluginSubscribe (track R4): the single bool it replaced
// conflated "subscriber cancelled", "server closed cleanly" and "the call
// failed" -- none of which a plugin could tell apart. Shared by the transport
// (SessionClient) and the plugin seam (ClientApi), so it lives in its own
// header: the transport must not include the plugin-facing registries, and
// the registries must not include the transport.

namespace minimal::client {

enum class SubscribeOutcome {
    kCancelled,   // on_payload returned false: the subscriber chose to stop
    kEnded,       // the server closed a live subscription cleanly
    kError        // the call failed (refusal, transport death) -- check status
};

}  // namespace minimal::client
