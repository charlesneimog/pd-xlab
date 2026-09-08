cmake_minimum_required(VERSION 3.15)

project(ambi~)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTS OFF)
set(BUILD_STATIC_LIBS ON)

# ╭──────────────────────────────────────╮
# │               PDCMAKE                │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
set(PDCMAKE_VERSION "v0.2.15")
if(NOT EXISTS "${PDCMAKE_FILE}")
    file(DOWNLOAD https://raw.githubusercontent.com/pure-data/pd.cmake/refs/tags/${PDCMAKE_VERSION}/pd.cmake
         ${PDCMAKE_FILE})
endif()
include(${PDCMAKE_FILE})

# ╭──────────────────────────────────────╮
# │                 CPM                  │
# ╰──────────────────────────────────────╯
set(CPM_FILE ${CMAKE_BINARY_DIR}/CPM.cmake)
set(CPM_VERSION "0.42.1")
if(NOT EXISTS "${CPM_FILE}")
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake" ${CPM_FILE})
endif()
include(${CPM_FILE})

# ╭──────────────────────────────────────╮
# │           spatialaudioLib            │
# ╰──────────────────────────────────────╯
cpmaddpackage("gh:videolan/libspatialaudio#0.4.0")
cpmaddpackage("gh:hoene/libmysofa#v1.3.4")

# ╭──────────────────────────────────────╮
# │               LibSofa                │
# ╰──────────────────────────────────────╯
cmake_policy(SET CMP0077 NEW)
set(MYSOFA_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/libmysofa/src/hrtf")
set(MYSOFA_LIBRARIES mysofa-static)
include_directories(${MYSOFA_INCLUDE_DIRS})

# ╭──────────────────────────────────────╮
# │             Pd External              │
# ╰──────────────────────────────────────╯
pd_add_external(
    ambi~
    "${CMAKE_CURRENT_SOURCE_DIR}/ambi~.cpp"
    TARGET
    ambi_tilde)

add_definitions(-DHAVE_MYSOFA)

target_link_libraries(ambi_tilde PRIVATE spatialaudio)
target_include_directories(ambi_tilde PRIVATE "${libspatialaudio_SOURCE_DIR}/include")
target_include_directories(ambi_tilde PRIVATE "${libspatialaudio_BINARY_DIR}/include")

pd_add_datafile(ambi~ "${CMAKE_CURRENT_SOURCE_DIR}/ambi~-help.pd")
pd_add_datafile(ambi~ "${CMAKE_CURRENT_SOURCE_DIR}/nh906.sofa")

set_target_properties(
    ambi_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)