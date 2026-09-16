# Soname symlinks for the gathered third-party libraries (Linux).
#
# WHY: the minimal_runtime_deps copy emits the versioned FILE only --
# libcurl.so.4.8.0, no libcurl.so.4 link beside it. That is correct on macOS
# (LC_LOAD_DYLIB records the full versioned name, @rpath/libcurl.4.8.0.dylib,
# which is exactly what gets copied), but on Linux our objects record DT_NEEDED
# as the SONAME -- libcurl.so.4 -- so the loader needs the soname symlink next
# to the real file or every gathered library fails to resolve at startup.
#
# ldconfig would create the links in <prefix>/lib for free, but the runtime
# deps deliberately do not live there (ldconfig indexes lib/ non-recursively --
# that exclusion is the whole point, see MINIMAL_RUNTIME_DEP_INSTALL_DIR), so
# the links are made here instead, at install time.
#
# Registered from the top-level CMakeLists.txt:
#     include(cmake/InstallSonameLinks.cmake)
#     minimal_install_soname_links("${MINIMAL_RUNTIME_DEP_INSTALL_DIR}")
#
# Readelf must be present on any Linux build host (it is how CMake's own
# runtime-dependency machinery already inspects these files). The DT_SONAME
# read happens at install time, not configure time: the set resolves during
# install, not before it.
#
# An existing symlink is never clobbered: a re-install over a live tree must
# not transiently break a running server's libraries, and cmake -E
# create_symlink removes the destination itself before linking.

function(minimal_install_soname_links rel_dir)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()

    set(_script "
        set(_dir \"\${CMAKE_INSTALL_PREFIX}/${rel_dir}\")
        if(NOT IS_DIRECTORY \"\${_dir}\")
            return()  # a Homebrew/system-package build gathers nothing
        endif()
        file(GLOB _libs RELATIVE \"\${_dir}\" \"\${_dir}/*.so.*\")
        foreach(_lib IN LISTS _libs)
            if(IS_SYMLINK \"\${_dir}/\${_lib}\")
                continue()
            endif()
            execute_process(COMMAND readelf -d \"\${_dir}/\${_lib}\"
                            OUTPUT_VARIABLE _readelf_out ERROR_QUIET RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0 OR NOT _readelf_out MATCHES \"SONAME\\\\]\\\\s*\\\\[([^]\\\\n]+)\\\\]\")
                continue()  # not an ELF, or no soname -- nothing can need it by one
            endif()
            set(_soname \"\${CMAKE_MATCH_1}\")
            if(_soname STREQUAL _lib)
                continue()
            endif()
            set(_link \"\${_dir}/\${_soname}\")
            if(NOT EXISTS \"\${_link}\")
                file(CREATE_LINK \"\${_lib}\" \"\${_link}\" SYMBOLIC)
            endif()
        endforeach()
    ")
    install(CODE "${_script}")
endfunction()
