cmake_minimum_required(VERSION 3.15)
project(neimog)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(PD_BUILD_STATIC_OBJECTS ON)
set(POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(
    -Wno-deprecated # disables deprecated warnings
    -Wno-unused-parameter # disables unused parameter warnings
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
  set(CMAKE_SHARED_LINKER_FLAGS
      "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

# ──────────────────────────────────────
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
if(NOT EXISTS ${PDCMAKE_FILE})
  message(STATUS "Downloading pd.cmake")
  file(
    DOWNLOAD
    https://raw.githubusercontent.com/pure-data/pd.cmake/refs/tags/v0.2.5/pd.cmake
    ${PDCMAKE_FILE}
    SHOW_PROGRESS
    STATUS DOWNLOAD_STATUS)
endif()
include(${PDCMAKE_FILE})
include_directories(${PD_SOURCES_PATH})

# ╭──────────────────────────────────────╮
# │                FFTW3                 │
# ╰──────────────────────────────────────╯
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE)
set(BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
set(ENABLE_FLOAT
    ON
    CACHE BOOL "Build single-precision (fftw3f)" FORCE)
set(ENABLE_LONG_DOUBLE
    OFF
    CACHE BOOL "" FORCE)
set(ENABLE_QUAD_PRECISION
    OFF
    CACHE BOOL "" FORCE)
set(DISABLE_FORTRAN
    ON
    CACHE BOOL "" FORCE)

set(FFTW3_FILE ${CMAKE_BINARY_DIR}/fftw-3.3.10.tar.gz)
if(NOT EXISTS ${FFTW3_FILE})
  message(STATUS "Downloading FFTW3")
  file(
    DOWNLOAD https://www.fftw.org/fftw-3.3.10.tar.gz ${FFTW3_FILE}
    SHOW_PROGRESS
    STATUS DOWNLOAD_STATUS)
endif()

file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/fftw-3.3.10.tar.gz DESTINATION
     ${CMAKE_BINARY_DIR}/)
add_subdirectory(${CMAKE_BINARY_DIR}/fftw-3.3.10 EXCLUDE_FROM_ALL)
get_property(
  all_targets
  DIRECTORY ${CMAKE_BINARY_DIR}/fftw-3.3.10
  PROPERTY BUILDSYSTEM_TARGETS)

set_target_properties(fftw3f PROPERTIES LIBRARY_OUTPUT_DIRECTORY
                                        ${CMAKE_BINARY_DIR})
set_target_properties(fftw3f PROPERTIES POSITION_INDEPENDENT_CODE ON)
include_directories(${CMAKE_BINARY_DIR}/fftw-3.3.10/api)

# ╭──────────────────────────────────────╮
# │               Objects                │
# ╰──────────────────────────────────────╯
if(PD_FLOATSIZE STREQUAL 64)
  add_compile_definitions(PD_FLOATSIZE=64)
endif()

# divergence
file(GLOB statistics_src ${CMAKE_CURRENT_SOURCE_DIR}/Sources/statistics/*.cpp)
add_library(statistics STATIC ${statistics_src})
set_target_properties(statistics PROPERTIES POSITION_INDEPENDENT_CODE ON)

# array
file(GLOB arrays_src ${CMAKE_CURRENT_SOURCE_DIR}/Sources/arrays/*.cpp)
add_library(arrays STATIC ${arrays_src})
set_target_properties(arrays PROPERTIES POSITION_INDEPENDENT_CODE ON)

# manipulations
file(GLOB manipulations ${CMAKE_CURRENT_SOURCE_DIR}/Sources/manipulations/*.cpp)
add_library(manipulations STATIC ${manipulations})
set_target_properties(manipulations PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(manipulations PUBLIC fftw3f)

# mir
file(GLOB mir_source ${CMAKE_CURRENT_SOURCE_DIR}/Sources/mir/*.cpp)
add_library(mir STATIC ${mir_source})
set_target_properties(mir PROPERTIES POSITION_INDEPENDENT_CODE ON)

# onsetsds
file(GLOB ONSETSDS_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/Sources/mir/onsetsds~/*.c")

# utilities
list(APPEND utilities_src
     ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utilities/infinite.record.cpp)
list(APPEND utilities_src
     ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utilities/switch.control~.cpp)

add_library(utilities STATIC ${utilities_src})
set_target_properties(utilities PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ╭──────────────────────────────────────╮
# │           External Plugins           │
# ╰──────────────────────────────────────╯
add_library(
  patcherize STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/Sources/patcherize/patcherize-plugin/patcherize.c"
)
set_target_properties(patcherize PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ╭──────────────────────────────────────╮
# │              PY4PD, SAF              │
# ╰──────────────────────────────────────╯
set(PD_BUILD_STATIC_OBJECTS OFF)

# add_subdirectory(Sources/pd-saf)

set(PYVERSION "3.13")
add_subdirectory(Sources/py4pd)

# │              convolve~               │
pd_add_external(
  convolve~ ${CMAKE_CURRENT_SOURCE_DIR}/Sources/convolve_tilde/src/convolve~.c)
target_link_libraries(convolve_tilde PUBLIC fftw3f)
pd_add_datafile(
  convolver~
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/convolve_tilde/convolve~-help.pd)
pd_add_datafile(convolver~
                ${CMAKE_CURRENT_SOURCE_DIR}/Sources/convolve_tilde/audio)

# │                Reverb                │
list(
  APPEND
  multiconvolve_src
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/multiconvolve~.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/u_partition_convolve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/u_time_domain_convolve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/u_zero_latency_convolve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/u_output_channel_convolve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/u_multi_channel_convolve.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/m_memory.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/z_array.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/lib/z_fft.c)
pd_add_external(multiconvolve~ "${multiconvolve_src}")
pd_add_datafile(
  multiconvolve~
  "${CMAKE_CURRENT_SOURCE_DIR}/Sources/piro/multiconvolve~-help.pd")
target_compile_definitions(multiconvolve_tilde PRIVATE UNIX)

# ─────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

pd_add_external(pwout~ "${CMAKE_CURRENT_SOURCE_DIR}/Sources/utilities/pwout~.c")
target_link_libraries(pwout_tilde PUBLIC ${PIPEWIRE_LIBRARIES})
target_include_directories(pwout_tilde PUBLIC ${PIPEWIRE_INCLUDE_DIRS})

pd_add_external(pwin~ "${CMAKE_CURRENT_SOURCE_DIR}/Sources/utilities/pwin~.c")
target_link_libraries(pwin_tilde PUBLIC ${PIPEWIRE_LIBRARIES})
target_include_directories(pwin_tilde PUBLIC ${PIPEWIRE_INCLUDE_DIRS})

# ╭──────────────────────────────────────╮
# │             VST PLUGINS              │
# ╰──────────────────────────────────────╯

set(PD ON)
set(SC OFF)
set(TESTSUITE OFF)
set(VST2 OFF)
set(BRIDGE OFF)
set(VST3DIR
    "./Sources/vst3/Fire/JUCE/modules/juce_audio_processors/format_types/VST3_SDK/"
)
include_directories("${VST3DIR}")
add_subdirectory(Sources/vstplugin~/ EXCLUDE_FROM_ALL)

# vsts
add_subdirectory(Sources/vst3/Fire EXCLUDE_FROM_ALL)
include(Resources/cmake/dragonfly.cmake)
include(Resources/cmake/else.cmake)

# ╭──────────────────────────────────────╮
# │           Library Sources            │
# ╰──────────────────────────────────────╯
set(PD_BUILD_STATIC_OBJECTS OFF)
list(APPEND PD_NEIMOG "${CMAKE_CURRENT_SOURCE_DIR}/Sources/neimog.cpp")
file(GLOB NEIMOG_ABS "${CMAKE_CURRENT_SOURCE_DIR}/Abstractions/*")
file(GLOB NEIMOG_HELP "${CMAKE_CURRENT_SOURCE_DIR}/Help-Patches/*")
file(GLOB NEIMOG_LUA "${CMAKE_CURRENT_SOURCE_DIR}/Sources/lua/*.pd_lua")
file(GLOB NEIMOG_PY "${CMAKE_CURRENT_SOURCE_DIR}/Sources/python/*.pd_py")

pd_add_external(neimog "${PD_NEIMOG};${ONSETSDS_SOURCES}")

pd_add_datafile(neimog "${NEIMOG_ABS}")
pd_add_datafile(neimog "${NEIMOG_HELP}")
pd_add_datafile(neimog "${CMAKE_CURRENT_SOURCE_DIR}/Sources/lua")
pd_add_datafile(neimog "${NEIMOG_LUA}")
pd_add_datafile(neimog "${NEIMOG_PY}")
pd_add_datafile(
  neimog
  "${CMAKE_CURRENT_SOURCE_DIR}/Sources/patcherize/patcherize-plugin/patcherize-plugin.tcl"
)

target_link_libraries(neimog PUBLIC patcherize statistics arrays manipulations
                                    mir utilities)
set_target_properties(neimog PROPERTIES CXX_STANDARD 20)

# List of external dependencies
set(ADICIONAL_EXTERNALS py4pd Fire pwout_tilde pwin_tilde)

# Add dependencies for target 'neimog'
foreach(ext IN LISTS ADICIONAL_EXTERNALS)
  add_dependencies(neimog ${ext})
endforeach()

add_dependencies(neimog py4pd)
add_dependencies(neimog Fire_VST3)
add_dependencies(neimog pwout_tilde)
add_dependencies(neimog pwin_tilde)

# Install vst3
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(ARCHITECTURE "x86_64")
else()
  set(ARCHITECTURE "x86")
endif()

# Detect platform
if(UNIX AND NOT APPLE)
  set(PLATFORM "linux")
elseif(APPLE)
  set(PLATFORM "mac")
elseif(WIN32)
  set(PLATFORM "win")
endif()

install(
  TARGETS Fire_VST3
  LIBRARY
    DESTINATION
      "${PDLIBDIR}/${PROJECT_NAME}/vst3/Fire.vst3/Contents/${ARCHITECTURE}-${PLATFORM}"
)

install(
  TARGETS dragonfly-early-reflections-vst3
  LIBRARY
    DESTINATION
      "${PDLIBDIR}/${PROJECT_NAME}/vst3/dragonfly-early-reflections.vst3/Contents/${ARCHITECTURE}-${PLATFORM}"
)

install(
  TARGETS dragonfly-room-reverb-vst3
  LIBRARY
    DESTINATION
      "${PDLIBDIR}/${PROJECT_NAME}/vst3/dragonfly-room-reverb.vst3/Contents/${ARCHITECTURE}-${PLATFORM}"
)

install(TARGETS vstplugin_tilde
        LIBRARY DESTINATION "${PDLIBDIR}/${PROJECT_NAME}/")
