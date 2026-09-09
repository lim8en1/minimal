# A user client plugin: one line. The kind (server vs client) is decided by
# which user-plugins/ subtree this directory is registered under, never by
# anything here -- moving the plugin between kinds means moving its directory
# (or symlink) into the other subtree.
minimal_add_user_plugin(@PLUGIN_NAME@ plugin.cpp)
