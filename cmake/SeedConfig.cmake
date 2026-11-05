# Seeds the user's config dir from bundled/, at `cmake --install` time -- and,
# on request, brings files the user never touched up to the current release.
#
# WHY INSTALL TIME, and not first server run. Everything under bundled/ is a file
# the USER owns once it lands -- prompts, presets, settings. Reading it from the
# install prefix at runtime, or compiling it into the binary, would make the
# harness's behaviour depend on files nobody can see or edit, which is the exact
# opposite of this project's premise. Copying at install time means there is
# exactly one copy of each of these, it lives in the config dir, and it is the one
# that runs. No runtime lookup path, no build-tree/installed-tree probe, no
# precedence rule to remember.
#
# NEVER OVERWRITES AN EDIT. An edited file is the user's. Silently reverting
# someone's compaction prompt or preset on an upgrade would be a far worse failure
# than shipping a stale default. So seeding never replaces an existing file, and
# the update mode replaces one only when it can PROVE the user never touched it.
#
# THE PROOF is the seed record, <config>/.bundled.sha256 (sha256sum format): the
# hash of every file as it was seeded. A bundled file's version IS its content
# hash -- nothing to bump by hand, nothing to forget. Per file, with B = this
# release's hash, R = recorded hash, D = the config dir's copy:
#   D == B            up to date
#   B == R            the release has not changed it since it was seeded: nothing
#                     to offer, edited or not
#   D == R, B != R    untouched stale default: update replaces it
#   D != R, B != R    edited AND changed upstream: update leaves it alone and
#                     writes <file>.bundled-new beside it for the user to merge
#   no R              predates the record: update offers .bundled-new once, then
#                     records B as the baseline
#
# Modes (MINIMAL_SEED_MODE):
#   seed    (default; the install step) copy missing files, record them, and
#           REPORT what an update would do -- never changes an existing file
#   check   report only, writes nothing; exits 0
#   update  seed, then apply the table above
#
# Invoked as:
#   install(SCRIPT ...)                               -- seed, during install
#   cmake --build build --target update-config        -- update
#   cmake --build build --target check-config         -- check
#   cmake -DMINIMAL_SEED_SOURCE=... -P ...            -- directly, how it is tested

# -P runs with every policy unset (OLD): IN_LIST and COPY_FILE need them set.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED MINIMAL_SEED_SOURCE)
    set(MINIMAL_SEED_SOURCE "@CMAKE_SOURCE_DIR@/bundled")
endif()
if(NOT DEFINED MINIMAL_SEED_MODE)
    set(MINIMAL_SEED_MODE "seed")
endif()
if(NOT MINIMAL_SEED_MODE MATCHES "^(seed|check|update)$")
    message(FATAL_ERROR "minimal: MINIMAL_SEED_MODE must be seed, check or update, "
                        "got '${MINIMAL_SEED_MODE}'")
endif()

# Placeholders, not defaults: providers.json seeds with no providers (see the
# note at the bottom). Offering its empty skeleton as an "update" to a filled-in
# file would only be noise.
set(_seed_only "providers.json")

set(_record_name ".bundled.sha256")
set(_new_suffix ".bundled-new")

# DESTDIR means a packaging root, not a live machine: there is no user whose
# config dir this is. Staging a package must not write into the builder's home.
if(DEFINED ENV{DESTDIR} AND NOT "$ENV{DESTDIR}" STREQUAL "")
    message(STATUS "minimal: DESTDIR set, skipping config seeding "
                   "(run the server once on the target machine, or seed by hand)")
    return()
endif()

# The same resolution order as common::ConfigDir(). Kept in step by hand, which
# is a real cost -- but the alternative is running the binary to ask it, and the
# binary may not be installed yet.
if(DEFINED MINIMAL_SEED_CONFIG_DIR)
    set(_config_dir "${MINIMAL_SEED_CONFIG_DIR}")   # test seam
elseif(DEFINED ENV{MINIMAL_CONFIG_DIR} AND NOT "$ENV{MINIMAL_CONFIG_DIR}" STREQUAL "")
    set(_config_dir "$ENV{MINIMAL_CONFIG_DIR}")
elseif(DEFINED ENV{XDG_CONFIG_HOME} AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
    set(_config_dir "$ENV{XDG_CONFIG_HOME}/minimal")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
    set(_config_dir "$ENV{HOME}/.config/minimal")
else()
    message(WARNING "minimal: no HOME or XDG_CONFIG_HOME; skipping config seeding")
    return()
endif()

# GLOB_RECURSE ... RELATIVE quietly matches nothing for a relative source.
get_filename_component(MINIMAL_SEED_SOURCE "${MINIMAL_SEED_SOURCE}" ABSOLUTE)
get_filename_component(_config_dir "${_config_dir}" ABSOLUTE)

if(NOT IS_DIRECTORY "${MINIMAL_SEED_SOURCE}")
    message(WARNING "minimal: no bundled config at ${MINIMAL_SEED_SOURCE}; nothing seeded")
    return()
endif()

set(_writes ON)
if(MINIMAL_SEED_MODE STREQUAL "check")
    set(_writes OFF)
endif()

message(STATUS "minimal: ${MINIMAL_SEED_MODE} config in ${_config_dir}")

# --- Ownership under sudo ----------------------------------------------------
# `sudo make install` seeds the INVOKING user's config dir (the Makefile points
# MINIMAL_CONFIG_DIR there), but everything written here would be root-owned
# and uneditable by that user. When running as root on behalf of a sudo user
# who owns the nearest existing ancestor of the config dir, every path written
# below -- and every directory created on the way to it -- is handed back to
# them. A root-owned target (a deliberate system-wide config) is left alone.
set(_chown_to "")
set(_chown_paths "")
if(_writes AND NOT "$ENV{SUDO_UID}" STREQUAL "" AND NOT "$ENV{SUDO_UID}" STREQUAL "0"
   AND NOT "$ENV{SUDO_GID}" STREQUAL "")
    execute_process(COMMAND id -u OUTPUT_VARIABLE _uid OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _uid STREQUAL "0")
        set(_anchor "${_config_dir}")
        while(NOT EXISTS "${_anchor}")
            list(APPEND _chown_paths "${_anchor}")   # created by the writes below
            get_filename_component(_anchor "${_anchor}" DIRECTORY)
        endwhile()
        if(APPLE)
            set(_stat_uid stat -f %u)
        else()
            set(_stat_uid stat -c %u)
        endif()
        execute_process(COMMAND ${_stat_uid} "${_anchor}" OUTPUT_VARIABLE _anchor_uid
                        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc)
        if(_rc EQUAL 0 AND _anchor_uid STREQUAL "$ENV{SUDO_UID}")
            set(_chown_to "$ENV{SUDO_UID}:$ENV{SUDO_GID}")
            list(APPEND _chown_paths "${_config_dir}")
        endif()
    endif()
endif()

# --- The seed record ---------------------------------------------------------
# Loaded into _rec_<relpath> variables. A line that does not parse is dropped:
# the record is a convenience, and losing an entry only costs the "untouched"
# proof for that one file (it then takes the no-R row).
set(_record "${_config_dir}/${_record_name}")
set(_recorded "")
if(EXISTS "${_record}")
    file(STRINGS "${_record}" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^([0-9a-f]+)  (.+)$")
            set("_rec_${CMAKE_MATCH_2}" "${CMAKE_MATCH_1}")
            list(APPEND _recorded "${CMAKE_MATCH_2}")
        endif()
    endforeach()
endif()

file(GLOB_RECURSE _bundled RELATIVE "${MINIMAL_SEED_SOURCE}" "${MINIMAL_SEED_SOURCE}/*")
list(FILTER _bundled EXCLUDE REGEX "(^|/)\\.DS_Store$")
list(SORT _bundled)

set(_seeded "")
set(_kept 0)
set(_updated "")     # untouched stale default: replaced (update) / replaceable
set(_offered "")     # edited and changed upstream: .bundled-new written / due
set(_current "")     # files that end up in the new record
foreach(_rel IN LISTS _bundled)
    set(_src "${MINIMAL_SEED_SOURCE}/${_rel}")
    set(_dst "${_config_dir}/${_rel}")
    if(IS_DIRECTORY "${_src}")
        continue()
    endif()
    file(SHA256 "${_src}" _b)
    if(DEFINED "_rec_${_rel}")
        set(_r "${_rec_${_rel}}")
    else()
        set(_r "")
    endif()

    if(NOT EXISTS "${_dst}")
        # Per file, not all-or-nothing: a config dir that has settings.json but
        # not agents/ still gets the presets.
        list(APPEND _seeded "${_rel}")
        if(_writes)
            get_filename_component(_dst_dir "${_dst}" DIRECTORY)
            file(MAKE_DIRECTORY "${_dst_dir}")
            file(COPY_FILE "${_src}" "${_dst}")
            list(APPEND _chown_paths "${_dst}")
            # The subdirectories on the way (agents/, prompts/, ...).
            while(NOT _dst_dir STREQUAL _config_dir)
                list(APPEND _chown_paths "${_dst_dir}")
                get_filename_component(_dst_dir "${_dst_dir}" DIRECTORY)
            endwhile()
        endif()
        set("_rec_${_rel}" "${_b}")
        list(APPEND _current "${_rel}")
        continue()
    endif()

    math(EXPR _kept "${_kept} + 1")
    file(SHA256 "${_dst}" _d)
    list(APPEND _current "${_rel}")
    if(_d STREQUAL _b)
        set("_rec_${_rel}" "${_b}")      # up to date (and adopts a pre-record file)
        continue()
    endif()
    if(_r STREQUAL _b OR _rel IN_LIST _seed_only)
        continue()                       # nothing new from this release
    endif()
    if(_d STREQUAL _r)
        list(APPEND _updated "${_rel}")
        if(MINIMAL_SEED_MODE STREQUAL "update")
            file(COPY_FILE "${_src}" "${_dst}")
            list(APPEND _chown_paths "${_dst}")
            set("_rec_${_rel}" "${_b}")
        endif()
        continue()
    endif()
    # Edited (or of unknown origin) and the release changed it.
    list(APPEND _offered "${_rel}")
    if(MINIMAL_SEED_MODE STREQUAL "update")
        file(COPY_FILE "${_src}" "${_dst}${_new_suffix}")
        list(APPEND _chown_paths "${_dst}${_new_suffix}")
        set("_rec_${_rel}" "${_b}")      # offered once; quiet until it changes again
    endif()
endforeach()

# Recorded but no longer shipped. Never deleted -- even an untouched one may be
# something the user now relies on -- only reported, and dropped from the record.
set(_dropped "")
foreach(_rel IN LISTS _recorded)
    if(NOT _rel IN_LIST _current AND EXISTS "${_config_dir}/${_rel}")
        list(APPEND _dropped "${_rel}")
    endif()
endforeach()

if(_writes)
    set(_out "")
    foreach(_rel IN LISTS _current)
        if(DEFINED "_rec_${_rel}")
            string(APPEND _out "${_rec_${_rel}}  ${_rel}\n")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${_config_dir}")
    file(WRITE "${_record}" "${_out}")
    list(APPEND _chown_paths "${_record}")
endif()

if(NOT _chown_to STREQUAL "" AND _chown_paths)
    list(REMOVE_DUPLICATES _chown_paths)
    # -h: a symlink is re-owned itself, never followed out of the config dir.
    execute_process(COMMAND chown -h "${_chown_to}" ${_chown_paths} RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(WARNING "minimal: could not hand the seeded config back to "
                        "uid ${_chown_to}; fix with: sudo chown -R ${_chown_to} ${_config_dir}")
    endif()
endif()

# --- Report ------------------------------------------------------------------
foreach(_rel IN LISTS _seeded)
    if(_writes)
        message(STATUS "  seeded   ${_rel}")
    else()
        message(STATUS "  missing  ${_rel}")
    endif()
endforeach()
foreach(_rel IN LISTS _updated)
    if(MINIMAL_SEED_MODE STREQUAL "update")
        message(STATUS "  updated  ${_rel} (you had not changed it)")
    else()
        message(STATUS "  stale    ${_rel} (unchanged by you; update replaces it)")
    endif()
endforeach()
foreach(_rel IN LISTS _offered)
    if(MINIMAL_SEED_MODE STREQUAL "update")
        message(STATUS "  offered  ${_rel} -> ${_rel}${_new_suffix} (yours; merge by hand)")
    else()
        message(STATUS "  changed  ${_rel} (yours; update writes ${_rel}${_new_suffix})")
    endif()
endforeach()
foreach(_rel IN LISTS _dropped)
    message(STATUS "  retired  ${_rel} (no longer bundled; yours to keep or delete)")
endforeach()

list(LENGTH _seeded _n_seeded)
list(LENGTH _updated _n_updated)
list(LENGTH _offered _n_offered)
message(STATUS "minimal: ${_n_seeded} seeded, ${_kept} already present, "
               "${_n_updated} stale default(s), ${_n_offered} edited file(s) changed upstream")
if(NOT MINIMAL_SEED_MODE STREQUAL "update" AND (_n_updated GREATER 0 OR _n_offered GREATER 0))
    message(STATUS "  apply with: cmake --build <build dir> --target update-config")
endif()

if(NOT EXISTS "${_config_dir}/providers.json"
   OR NOT EXISTS "${_config_dir}/providers.example.json")
    return()
endif()
file(READ "${_config_dir}/providers.json" _providers)
string(REGEX REPLACE "[ \t\r\n]" "" _providers "${_providers}")
if(_providers STREQUAL "{\"providers\":{}}")
    # The one thing seeding cannot do for you. A base_url that is right on the
    # machine that built this is wrong on the next one, so there is no honest
    # default -- an empty list that plainly needs filling in beats a plausible
    # entry that silently points at nothing.
    message(STATUS "")
    message(STATUS "  minimal: no model configured yet. Edit")
    message(STATUS "    ${_config_dir}/providers.json")
    message(STATUS "  using providers.example.json beside it as a starting point.")
endif()
