minimal_server_module(session
    DEPS llm store
    SOURCES
        ulid.cpp
        message_codec.cpp
        session_database.cpp
        sqlite_history_store.cpp
        session_reader.cpp
)
