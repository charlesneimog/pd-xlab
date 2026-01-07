cmake_minimum_required(VERSION 3.15)
project(xlab)

# ──────────────────────────────────────
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(PD_BUILD_STATIC_OBJECTS OFF)
set(POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version
set(PYVERSION "3.13" CACHE STRING "Version of Python used in py4pd")

# ╭──────────────────────────────────────╮
# │                FLAGS                 │
# ╰──────────────────────────────────────╯
if(LINUX)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")
endif()

# ╭──────────────────────────────────────╮
# │               pd.cmake               │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
set(PDCMAKE_VERSION "v0.2.11")
if(NOT EXISTS ${PDCMAKE_FILE})
    message(STATUS "Downloading pd.cmake")
    file(DOWNLOAD 
    https://github.com/pure-data/pd.cmake/releases/download/v2.0.11/pd.cmake ${PDCMAKE_FILE} 
    STATUS DOWNLOAD_STATUS)
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
    file(DOWNLOAD https://www.fftw.org/fftw-3.3.10.tar.gz ${FFTW3_FILE} STATUS DOWNLOAD_STATUS)
endif()

file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/fftw-3.3.10.tar.gz DESTINATION ${CMAKE_BINARY_DIR}/)
add_subdirectory(${CMAKE_BINARY_DIR}/fftw-3.3.10 EXCLUDE_FROM_ALL)
get_property(
    all_targets
    DIRECTORY ${CMAKE_BINARY_DIR}/fftw-3.3.10
    PROPERTY BUILDSYSTEM_TARGETS)

set_target_properties(fftw3f PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set_target_properties(fftw3f PROPERTIES POSITION_INDEPENDENT_CODE ON)
include_directories(${CMAKE_BINARY_DIR}/fftw-3.3.10/api)

# ╭──────────────────────────────────────╮
# │               Objects                │
# ╰──────────────────────────────────────╯
file(GLOB statistics_src "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/*.cpp")
add_library(statistics STATIC "${statistics_src}")
set_target_properties(statistics PROPERTIES POSITION_INDEPENDENT_CODE ON)

# array
file(GLOB arrays_src "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/*.cpp")
add_library(arrays STATIC "${arrays_src}")
set_target_properties(arrays PROPERTIES POSITION_INDEPENDENT_CODE ON)

# manipulations
file(GLOB manipulations "${CMAKE_CURRENT_SOURCE_DIR}/src/manipulations/*.cpp")
add_library(manipulations STATIC "${manipulations}")
set_target_properties(manipulations PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(manipulations PUBLIC fftw3f)

# mir
file(GLOB mir_source "${CMAKE_CURRENT_SOURCE_DIR}/src/mir/*.cpp")
add_library(mir STATIC "${mir_source}")
set_target_properties(mir PROPERTIES POSITION_INDEPENDENT_CODE ON)

# utilities
file(GLOB utilities_src "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/*.cpp")
add_library(utilities STATIC "${utilities_src}")
set_target_properties(utilities PROPERTIES POSITION_INDEPENDENT_CODE ON)

# main object
pd_add_external(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/xlab.cpp")
file(GLOB XLAB_FILES "${CMAKE_BINARY_DIR}/${PROJECT_NAME}/*")
pd_add_datafile(xlab "${XLAB_FILES}")
target_link_libraries(xlab PRIVATE utilities manipulations arrays statistics)


# ╭──────────────────────────────────────╮
# │          Externals Objects           │
# ╰──────────────────────────────────────╯
# ELSE
unset(PD_OUTPUT_PATH)
cpmaddpackage(URI "gh:jimmy-park/openssl-cmake#3.6.0" OPTIONS "OPENSSL_USE_STATIC_LIBS ON" "BUILD_SHARED_LIBS OFF")
cpmaddpackage("gh:porres/pd-else#master")

if(pd-else_ADDED)
    # abstraction
    pd_add_datafile(xlab "${pd-else_SOURCE_DIR}/Abstractions/Control/count.pd")

    # help files
    pd_add_datafile(xlab "${pd-else_SOURCE_DIR}/Documentation/Help-files/giga.rev~-help.pd")
    pd_add_datafile(xlab "${pd-else_SOURCE_DIR}/Documentation/Help-files/plaits~-help.pd")

    pd_add_datafile(xlab "${pd-else_SOURCE_DIR}/Documentation/Help-files/count-help.pd")
endif()

# ╭──────────────────────────────────────╮
# │         Externals Libraries          │
# ╰──────────────────────────────────────╯
set(PD_OUTPUT_PATH "${CMAKE_BINARY_DIR}/xlab")
cpmaddpackage("gh:charlesneimog/py4pd#master")
cpmaddpackage("gh:charlesneimog/pd-ambi#main")
cpmaddpackage("gh:charlesneimog/pv_tilde#main")

# lua
cpmaddpackage("gh:charlesneimog/pd-upic#main")
cpmaddpackage("gh:charlesneimog/pd-orchidea#main")
cpmaddpackage("gh:EL-LOCUS-SOLUS/pd-bhack#main")

# single objects
cpmaddpackage("gh:charlesneimog/pd-onnx#main")
cpmaddpackage("gh:charlesneimog/onsetsds_tilde#v0.1.0")

# saf
cpmaddpackage("gh:EL-LOCUS-SOLUS/pd-saf#main")

# ╭──────────────────────────────────────╮
# │          Externals Objects           │
# ╰──────────────────────────────────────╯
set(PDELSE_OBJECTS giga.rev_tilde plaits_tilde play.file_tilde)
foreach(obj ${PDELSE_OBJECTS})
    if(TARGET ${obj})
        set_target_properties(${obj} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
        set_target_properties(
            ${obj}
            PROPERTIES LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/${PROJECT_NAME}"
                       LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/${PROJECT_NAME}"
                       LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/${PROJECT_NAME}"
                       LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/${PROJECT_NAME}")
    else()
        message(WARNING "Target ${obj} not found")
    endif()
endforeach()

# ╭──────────────────────────────────────╮
# │             xlab library             │
# ╰──────────────────────────────────────╯
add_dependencies(
    xlab
    py4pd
    # lua
    upic
    orchidea
    bhack
    # extra
    pdonnx
    pdlua
    pv_tilde
    # o.scofo_tilde
    onsetsds_tilde
    saf.encoder_tilde
    saf.decoder_tilde
    saf.binaural_tilde
    saf.roomsim_tilde
    saf.panner_tilde
    # else
    giga.rev_tilde
    plaits_tilde
    play.file_tilde)
