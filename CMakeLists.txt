cmake_minimum_required(VERSION 3.15)
project(neimog)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(PD_BUILD_STATIC_OBJECTS ON)
set(POSITION_INDEPENDENT_CODE ON)

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
    https://raw.githubusercontent.com/pure-data/pd.cmake/refs/tags/v0.2.4/pd.cmake
    ${PDCMAKE_FILE}
    SHOW_PROGRESS
    STATUS DOWNLOAD_STATUS)
endif()
include(${PDCMAKE_FILE})

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
file(GLOB utilities_src ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utilities/*.cpp)
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
# │             PureData Saf             │
# ╰──────────────────────────────────────╯
add_subdirectory(Sources/pd-saf)

# ╭──────────────────────────────────────╮
# │                py4pd                 │
# ╰──────────────────────────────────────╯
set(PD_BUILD_STATIC_OBJECTS OFF)
set(PYVERSION "3.13")
add_subdirectory(Sources/py4pd)
set(PD_BUILD_STATIC_OBJECTS ON)

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

target_link_libraries(
  neimog
  PUBLIC patcherize
         statistics
         arrays
         manipulations
         mir
         utilities
         saf.encoder_tilde
         saf.decoder_tilde
         saf.binaural_tilde
         saf.roomsim_tilde
         saf.pitchshifter_tilde
         saf.binauraliser_tilde)

set_target_properties(saf.encoder_tilde PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(saf.decoder_tilde PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(saf.binaural_tilde PROPERTIES POSITION_INDEPENDENT_CODE
                                                    ON)
set_target_properties(saf.roomsim_tilde PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(saf.pitchshifter_tilde
                      PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(saf.binauraliser_tilde
                      PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_dependencies(neimog py4pd)
set_target_properties(neimog PROPERTIES CXX_STANDARD 20)
