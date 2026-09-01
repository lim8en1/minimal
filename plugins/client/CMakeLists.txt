# Bundled CLIENT plugin implementations -- none yet.
#
# The host machinery lives in src/client/plugins (track E5); this subtree is for
# implementations, categorically identical to a third-party client plugin and
# differing only in who publishes it. The external todo plugin is the intended
# first consumer; until it lands, E5's proof is the test fixtures
# (tests/fixtures/client-*), which use minimal_add_client_fixture_plugin.
