cmake_minimum_required(VERSION 3.25)
project(x.pv~)

# ╭──────────────────────────────────────╮
# │               PDCMAKE                │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
set(PDCMAKE_VERSION "v0.2.11")
if(NOT EXISTS "${PDCMAKE_FILE}")
    file(DOWNLOAD https://github.com/pure-data/pd.cmake/releases/download/v2.0.11/pd.cmake ${PDCMAKE_FILE})
endif()
include(${PDCMAKE_FILE})

# ╭──────────────────────────────────────╮
# │                 CPM                  │
# ╰──────────────────────────────────────╯
set(CPM_FILE ${CMAKE_BINARY_DIR}/CPM.cmake)
set(CPM_VERSION "0.42.0")
if(NOT EXISTS "${CPM_FILE}")
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake" ${CPM_FILE})
endif()
include(${CPM_FILE})

# ╭──────────────────────────────────────╮
# │               Library                │
# ╰──────────────────────────────────────╯
cpmaddpackage("gh:Signalsmith-Audio/signalsmith-stretch#main")

# ╭──────────────────────────────────────╮
# │               External               │
# ╰──────────────────────────────────────╯
pd_add_external(x.pv~ x.pv~.cpp LINK_LIBRARIES signalsmith-stretch)
target_compile_options(x.pv_tilde PRIVATE -include cstring)
pd_add_datafile(x.pv~ "${CMAKE_CURRENT_SOURCE_DIR}/x.pv~-help.pd")
