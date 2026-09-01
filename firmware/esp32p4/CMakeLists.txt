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

project(revlink_esp32p4)
