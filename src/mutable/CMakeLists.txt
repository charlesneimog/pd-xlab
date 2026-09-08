cmake_minimum_required(VERSION 3.25)
project(
    mutables~
    VERSION 0.1.0
    DESCRIPTION "Mutables implement the mutables instruments for Pure Data")

# ╭──────────────────────────────────────╮
# │             CPM Package              │
# ╰──────────────────────────────────────╯
set(CPM_FILE ${CMAKE_BINARY_DIR}/CPM.cmake)
set(CPM_VERSION "0.42.0")
if(NOT EXISTS "${CPM_FILE}")
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake" ${CPM_FILE})
endif()
include(${CPM_FILE})

# ╭──────────────────────────────────────╮
# │               PD.CMAKE               │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
set(PDCMAKE_VERSION "v0.2.15")
if(NOT EXISTS "${PDCMAKE_FILE}")
    file(DOWNLOAD
         # "https://github.com/pure-data/pd.cmake/releases/download/v2.0.11/pd.cmake"
         "https://raw.githubusercontent.com/charlesneimog/pd.cmake/refs/heads/flags-test/pd.cmake" ${PDCMAKE_FILE})
endif()
include(${PDCMAKE_FILE})

cpmaddpackage("gh:pichenettes/eurorack#master")

# ╭──────────────────────────────────────╮
# │               ELEMENTS               │
# ╰──────────────────────────────────────╯
set(ELEMENTS_SOURCES
    ${eurorack_SOURCE_DIR}/stmlib/dsp/units.cc
    ${eurorack_SOURCE_DIR}/stmlib/utils/random.cc
    ${eurorack_SOURCE_DIR}/elements/resources.cc
    # dsp
    ${eurorack_SOURCE_DIR}/elements/dsp/ominous_voice.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/exciter.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/multistage_envelope.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/part.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/string.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/resonator.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/tube.cc
    ${eurorack_SOURCE_DIR}/elements/dsp/voice.cc)

# ---- Executable ----
add_library(elements STATIC ${ELEMENTS_SOURCES})
target_compile_definitions(elements PUBLIC TEST=1 _USE_MATH_DEFINES=1)
set_target_properties(elements PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Headers use paths relative to the repository root. Adding elements/dsp would
# shadow the standard <string.h> with elements/dsp/string.h on macOS.
target_include_directories(elements PRIVATE ${eurorack_SOURCE_DIR})

# ---- Compile definitions ----
target_compile_definitions(elements PRIVATE TEST)
pd_add_external(elements~ "${CMAKE_CURRENT_SOURCE_DIR}/elements~.cpp")
target_link_libraries(elements_tilde PRIVATE elements)
target_include_directories(elements_tilde SYSTEM PUBLIC ${eurorack_SOURCE_DIR})

# ╭──────────────────────────────────────╮
# │                RINGS                 │
# ╰──────────────────────────────────────╯
set(RINGS_SOURCES
    ${eurorack_SOURCE_DIR}/stmlib/dsp/units.cc
    ${eurorack_SOURCE_DIR}/stmlib/utils/random.cc
    ${eurorack_SOURCE_DIR}/rings/resources.cc
    # dsp
    ${eurorack_SOURCE_DIR}/rings/dsp/fm_voice.cc
    ${eurorack_SOURCE_DIR}/rings/dsp/part.cc
    ${eurorack_SOURCE_DIR}/rings/dsp/resonator.cc
    ${eurorack_SOURCE_DIR}/rings/dsp/string.cc
    ${eurorack_SOURCE_DIR}/rings/dsp/string_synth_part.cc
    #
)

add_library(rings STATIC ${RINGS_SOURCES})
target_include_directories(rings SYSTEM PUBLIC ${eurorack_SOURCE_DIR})
target_compile_definitions(rings PUBLIC TEST=1 _USE_MATH_DEFINES=1)
set_target_properties(rings PROPERTIES POSITION_INDEPENDENT_CODE ON)
pd_add_external(rings~ "${CMAKE_CURRENT_SOURCE_DIR}/rings~.cpp")
target_link_libraries(rings_tilde PRIVATE rings)
target_include_directories(rings_tilde SYSTEM PUBLIC ${eurorack_SOURCE_DIR})
