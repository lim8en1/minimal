# Vendored tree-sitter grammars for the client's syntax highlighter.
#
# vcpkg packages the tree-sitter CORE library but not the per-language parsers
# (ARCHITECTURE.md "Dependency management"), so the curated grammar set is
# vendored the same way cpp-mcp is: FetchContent, pinned. Unlike cpp-mcp these
# repos publish releases, so the pins are TAGS, with the commit each tag resolved
# to recorded beside it -- a silent retag is then detectable in review.
#
# Each grammar is two files: src/parser.c (generated, self-contained) and
# optionally src/scanner.{c,cc} (the external scanner). Compiling them is one
# static library per grammar; none of them needs the tree-sitter core headers
# (parser.c includes only the repo's own src/tree_sitter/parser.h), so they
# carry no link dependency at all.
#
# SOURCE_SUBDIR points at a directory with no CMakeLists.txt on purpose:
# FetchContent then populates WITHOUT running the grammar repo's own CMakeLists
# (which would install its own targets into our prefix). We own the build.

include(FetchContent)

function(minimal_ts_grammar NAME REPO TAG GRAMMAR_SUBDIR)
    FetchContent_Declare(tsgrammar_${NAME}
        GIT_REPOSITORY "${REPO}"
        GIT_TAG        "${TAG}"
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  "no-such-subdirectory"  # populate-only; see header
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(tsgrammar_${NAME})

    set(_src "${tsgrammar_${NAME}_SOURCE_DIR}/${GRAMMAR_SUBDIR}src")
    if(NOT EXISTS "${_src}/parser.c")
        message(FATAL_ERROR "tree-sitter grammar ${NAME}: ${_src}/parser.c missing "
                            "(repo layout changed at ${TAG}?)")
    endif()

    add_library(tsgrammar_${NAME} STATIC "${_src}/parser.c")
    if(EXISTS "${_src}/scanner.cc")
        target_sources(tsgrammar_${NAME} PRIVATE "${_src}/scanner.cc")
    elseif(EXISTS "${_src}/scanner.c")
        target_sources(tsgrammar_${NAME} PRIVATE "${_src}/scanner.c")
    endif()
    target_include_directories(tsgrammar_${NAME} PRIVATE "${_src}")
    set_target_properties(tsgrammar_${NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Third-party generated C: the zero-warnings rule covers first-party code.
    target_compile_options(tsgrammar_${NAME} PRIVATE -w)
endfunction()

# tag -- commit it resolved to when pinned
minimal_ts_grammar(cpp        "https://github.com/tree-sitter/tree-sitter-cpp.git"           "v0.23.4" "")  # f41e1a044c8a84ea9fa8577fdd2eab92ec96de02
minimal_ts_grammar(python     "https://github.com/tree-sitter/tree-sitter-python.git"        "v0.25.0" "")  # 293fdc02038ee2bf0e2e206711b69c90ac0d413f
minimal_ts_grammar(javascript "https://github.com/tree-sitter/tree-sitter-javascript.git"    "v0.25.0" "")  # 44c892e0be055ac465d5eeddae6d3e194424e7de
minimal_ts_grammar(typescript "https://github.com/tree-sitter/tree-sitter-typescript.git"    "v0.23.2" "typescript/")  # f975a621f4e7f532fe322e13c4f79495e0a7b2e7
minimal_ts_grammar(bash       "https://github.com/tree-sitter/tree-sitter-bash.git"          "v0.25.1" "")  # a06c2e4415e9bc0346c6b86d401879ffb44058f7
minimal_ts_grammar(json       "https://github.com/tree-sitter/tree-sitter-json.git"          "v0.24.8" "")  # ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24
minimal_ts_grammar(yaml       "https://github.com/tree-sitter-grammars/tree-sitter-yaml.git" "v0.7.2"  "")  # 7708026449bed86239b1cd5bce6e3c34dbca6415
minimal_ts_grammar(rust       "https://github.com/tree-sitter/tree-sitter-rust.git"          "v0.24.2" "")  # 77a3747266f4d621d0757825e6b11edcbf991ca5
minimal_ts_grammar(go         "https://github.com/tree-sitter/tree-sitter-go.git"            "v0.25.0" "")  # 1547678a9da59885853f5f5cc8a99cc203fa2e2c
