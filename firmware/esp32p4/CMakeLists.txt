cmake_minimum_required(VERSION 3.16)

set(
    EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# CONFIG_COMPILER_HIDE_PATHS_MACROS rewrites __FILE__ only for paths under the
# project directory and IDF_PATH. The shared components live one level above
# this project, so without this their absolute build paths — including the
# building user's home directory — end up in assert strings inside published
# binaries. Map the firmware root as well so a release image carries relative
# paths and nothing about the machine that built it.
# -fmacro-prefix-map matches the prefix literally, so the path must be
# normalised: ".../esp32p4/.." never textually matches ".../components/...".
get_filename_component(
    REVLINK_FIRMWARE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE
)
idf_build_set_property(
    COMPILE_OPTIONS "-fmacro-prefix-map=${REVLINK_FIRMWARE_ROOT}=."
    APPEND
)

# Take the firmware version from version.txt rather than letting ESP-IDF fall
# back to `git describe`. That fallback reported a release as
# "v0.1.0-10-gd7c5a13" — honest about the commit, but describing it against a
# stale tag and disagreeing with every other place the version appears. It
# also makes the version depend on git state, so a build from a tarball or a
# shallow clone reports something different again.
# The commit this image was built from. A UI change alters the portal that is
# compiled into the firmware without necessarily changing the version, so the
# version alone cannot identify a build. Reported alongside it in the portal.
#
# Resolved at configure time, so a rebuild after committing without
# reconfiguring keeps the previous value. Release builds run set-target, which
# reconfigures, and a dirty tree is marked as such.
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" rev-parse --short=8 HEAD
    OUTPUT_VARIABLE REVLINK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT REVLINK_GIT_COMMIT)
    set(REVLINK_GIT_COMMIT "unknown")
else()
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}" status --porcelain
        OUTPUT_VARIABLE REVLINK_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT REVLINK_GIT_DIRTY STREQUAL "")
        set(REVLINK_GIT_COMMIT "${REVLINK_GIT_COMMIT}-dirty")
    endif()
endif()
idf_build_set_property(
    COMPILE_DEFINITIONS "REVLINK_GIT_COMMIT=\"${REVLINK_GIT_COMMIT}\""
    APPEND
)
message(STATUS "RevLink build commit: ${REVLINK_GIT_COMMIT}")

project(revlink_esp32p4)
