cmake_minimum_required(VERSION 3.25)
project(x.dragonfly~)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 11)

# ╭──────────────────────────────────────╮
# │               PDCMAKE                │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
if(NOT EXISTS "${PDCMAKE_FILE}")
    file(DOWNLOAD https://raw.githubusercontent.com/pure-data/pd.cmake/refs/heads/main/pd.cmake ${PDCMAKE_FILE})
endif()
include(${PDCMAKE_FILE})

# ╭──────────────────────────────────────╮
# │               Freeverb               │
# ╰──────────────────────────────────────╯
include(FetchContent)
FetchContent_Declare(
    dragonfly_reverb
    GIT_REPOSITORY https://github.com/michaelwillis/dragonfly-reverb.git
    GIT_TAG master
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(dragonfly_reverb)
set(FREEVERB_DIR "${dragonfly_reverb_SOURCE_DIR}/common/freeverb")

set(FREEVERB_SOURCES
    ${FREEVERB_DIR}/allpass.cpp
    ${FREEVERB_DIR}/biquad.cpp
    ${FREEVERB_DIR}/comb.cpp
    ${FREEVERB_DIR}/delay.cpp
    ${FREEVERB_DIR}/delayline.cpp
    ${FREEVERB_DIR}/earlyref.cpp
    ${FREEVERB_DIR}/efilter.cpp
    ${FREEVERB_DIR}/nrev.cpp
    ${FREEVERB_DIR}/nrevb.cpp
    ${FREEVERB_DIR}/progenitor.cpp
    ${FREEVERB_DIR}/progenitor2.cpp
    ${FREEVERB_DIR}/revbase.cpp
    ${FREEVERB_DIR}/slot.cpp
    ${FREEVERB_DIR}/strev.cpp
    ${FREEVERB_DIR}/utils.cpp
    ${FREEVERB_DIR}/zrev.cpp
    ${FREEVERB_DIR}/zrev2.cpp)

add_library(freeverb STATIC ${FREEVERB_SOURCES})
set_target_properties(freeverb PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(freeverb PUBLIC "${FREEVERB_DIR}/../")
target_compile_definitions(freeverb PUBLIC LIBFV3_FLOAT)
target_compile_options(freeverb PRIVATE -Wno-deprecated -Wno-deprecated-declarations)
target_link_libraries(freeverb PUBLIC m)

pd_add_external(x.dragonfly~ ${CMAKE_CURRENT_SOURCE_DIR}/x.dragonfly~.cpp LINK_LIBRARIES freeverb)
