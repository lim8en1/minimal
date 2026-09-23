include_guard(GLOBAL)

# Install-time only (never build-time): `gdb build/bin/minimal-server` must
# keep full debug info for local dev, so nothing here touches the build
# tree -- only install(CODE) blocks that run during `cmake --install` /
# `ninja install`, operating on the copies already placed under
# CMAKE_INSTALL_PREFIX by install(TARGETS).
#
# Gated to RelWithDebInfo/Release only: asan/tsan/debug are all
# CMAKE_BUILD_TYPE=Debug (see CMakePresets.json) and must never be
# stripped -- a stripped ASan build would silently produce unsymbolized
# reports.
set(_MINIMAL_STRIP_DEBUG_INFO_ENABLED FALSE)
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "Release")
    if(APPLE)
        find_program(MINIMAL_DSYMUTIL_PROGRAM dsymutil)
        if(MINIMAL_DSYMUTIL_PROGRAM AND CMAKE_STRIP)
            set(_MINIMAL_STRIP_DEBUG_INFO_ENABLED TRUE)
        else()
            message(WARNING "minimal: dsymutil or strip not found -- "
                             "installed binaries will keep full debug info")
        endif()
    else()
        if(CMAKE_OBJCOPY)
            set(_MINIMAL_STRIP_DEBUG_INFO_ENABLED TRUE)
        else()
            message(WARNING "minimal: CMAKE_OBJCOPY not found -- "
                             "installed binaries will keep full debug info")
        endif()
    endif()
endif()

# Splits debug info out of TARGET's installed copy (under DEST_SUBDIR,
# relative to CMAKE_INSTALL_PREFIX -- the same directory the caller's own
# install(TARGETS ... DESTINATION DEST_SUBDIR) used) into a sidecar file
# installed under MINIMAL_DEBUG_INFO_INSTALL_DIR/DEST_SUBDIR: a .debug file
# on Linux, a .dSYM bundle on macOS. NOT colocated with the binary -- see
# MINIMAL_DEBUG_INFO_INSTALL_DIR's comment in the top-level CMakeLists.txt
# for why (a .dSYM bundle contains a same-named copy of the binary one
# level down, which the bundled plugin scan's recursive directory iterator
# would otherwise pick up as a duplicate plugin). The tradeoff: gdb/lldb
# don't auto-discover it from the binary's own location; point
# `set debug-file-directory` / `settings set target.debug-file-search-paths`
# at MINIMAL_DEBUG_INFO_INSTALL_DIR/DEST_SUBDIR, or pass the .dSYM path
# directly. No-op when the RelWithDebInfo/Release gate above didn't enable
# stripping.
#
# Must be called after the target's own install(TARGETS ...) so the file
# this operates on already exists when `cmake --install` reaches this
# script.
function(minimal_strip_debug_info target dest_subdir)
    if(NOT _MINIMAL_STRIP_DEBUG_INFO_ENABLED)
        return()
    endif()

    if(APPLE)
        # Mach-O `strip` with no flags removes the ENTIRE symbol table,
        # including exported/global symbols -- fatal here, since plugins,
        # the executables, and (for minimal_proto) the two cores all
        # dynamically link against these exports. -S strips debug symbols
        # only.
        install(CODE "
            set(_msdi_file \"\${CMAKE_INSTALL_PREFIX}/${dest_subdir}/$<TARGET_FILE_NAME:${target}>\")
            set(_msdi_debug_dir \"\${CMAKE_INSTALL_PREFIX}/${MINIMAL_DEBUG_INFO_INSTALL_DIR}/${dest_subdir}\")
            if(DEFINED ENV{DESTDIR})
                set(_msdi_file \"\$ENV{DESTDIR}\${_msdi_file}\")
                set(_msdi_debug_dir \"\$ENV{DESTDIR}\${_msdi_debug_dir}\")
            endif()
            get_filename_component(_msdi_name \"\${_msdi_file}\" NAME)
            file(MAKE_DIRECTORY \"\${_msdi_debug_dir}\")
            message(STATUS \"minimal: dsymutil + strip \${_msdi_file}\")
            execute_process(
                COMMAND \"${MINIMAL_DSYMUTIL_PROGRAM}\" \"\${_msdi_file}\" -o \"\${_msdi_debug_dir}/\${_msdi_name}.dSYM\"
                RESULT_VARIABLE _msdi_rc)
            if(NOT _msdi_rc EQUAL 0)
                message(FATAL_ERROR \"minimal: dsymutil failed for \${_msdi_file}\")
            endif()
            execute_process(
                COMMAND \"${CMAKE_STRIP}\" -S \"\${_msdi_file}\"
                RESULT_VARIABLE _msdi_rc)
            if(NOT _msdi_rc EQUAL 0)
                message(FATAL_ERROR \"minimal: strip failed for \${_msdi_file}\")
            endif()
        " COMPONENT DebugInfo)
    else()
        install(CODE "
            set(_msdi_file \"\${CMAKE_INSTALL_PREFIX}/${dest_subdir}/$<TARGET_FILE_NAME:${target}>\")
            set(_msdi_debug_dir \"\${CMAKE_INSTALL_PREFIX}/${MINIMAL_DEBUG_INFO_INSTALL_DIR}/${dest_subdir}\")
            if(DEFINED ENV{DESTDIR})
                set(_msdi_file \"\$ENV{DESTDIR}\${_msdi_file}\")
                set(_msdi_debug_dir \"\$ENV{DESTDIR}\${_msdi_debug_dir}\")
            endif()
            get_filename_component(_msdi_name \"\${_msdi_file}\" NAME)
            file(MAKE_DIRECTORY \"\${_msdi_debug_dir}\")
            set(_msdi_debug \"\${_msdi_debug_dir}/\${_msdi_name}.debug\")
            message(STATUS \"minimal: splitting debug info for \${_msdi_file}\")
            execute_process(
                COMMAND \"${CMAKE_OBJCOPY}\" --only-keep-debug \"\${_msdi_file}\" \"\${_msdi_debug}\"
                RESULT_VARIABLE _msdi_rc)
            if(NOT _msdi_rc EQUAL 0)
                message(FATAL_ERROR \"minimal: objcopy --only-keep-debug failed for \${_msdi_file}\")
            endif()
            execute_process(
                COMMAND \"${CMAKE_OBJCOPY}\" --strip-debug --strip-unneeded \"\${_msdi_file}\"
                RESULT_VARIABLE _msdi_rc)
            if(NOT _msdi_rc EQUAL 0)
                message(FATAL_ERROR \"minimal: objcopy --strip-debug failed for \${_msdi_file}\")
            endif()
            execute_process(
                COMMAND \"${CMAKE_OBJCOPY}\" --add-gnu-debuglink=\"\${_msdi_debug}\" \"\${_msdi_file}\"
                RESULT_VARIABLE _msdi_rc)
            if(NOT _msdi_rc EQUAL 0)
                message(FATAL_ERROR \"minimal: objcopy --add-gnu-debuglink failed for \${_msdi_file}\")
            endif()
        " COMPONENT DebugInfo)
    endif()
endfunction()
