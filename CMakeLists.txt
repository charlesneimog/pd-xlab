cmake_minimum_required(VERSION 3.15)
project(xlab LANGUAGES CXX C)

# ──────────────────────────────────────
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(PD_BUILD_STATIC_OBJECTS OFF)
set(POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IAMCHARLES "Use local checkouts for packages that are not stored in pd-xlab" OFF)

# ╭──────────────────────────────────────╮
# │                FLAGS                 │
# ╰──────────────────────────────────────╯
if(LINUX OR MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

# ╭──────────────────────────────────────╮
# │               pd.cmake               │
# ╰──────────────────────────────────────╯
set(PDCMAKE_FILE ${CMAKE_BINARY_DIR}/pd.cmake)
set(PDCMAKE_VERSION "v0.2.19")
if(NOT EXISTS ${PDCMAKE_FILE})
    message(STATUS "Downloading pd.cmake")
    file(DOWNLOAD https://raw.githubusercontent.com/pure-data/pd.cmake/refs/heads/main/pd.cmake ${PDCMAKE_FILE}
         STATUS DOWNLOAD_STATUS)

endif()
include(${PDCMAKE_FILE})
include_directories("${PD_SOURCES_PATH}")

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

file(
    ARCHIVE_EXTRACT
    INPUT
    ${CMAKE_BINARY_DIR}/fftw-3.3.10.tar.gz
    DESTINATION
    ${CMAKE_BINARY_DIR}/)

add_subdirectory(${CMAKE_BINARY_DIR}/fftw-3.3.10 ${CMAKE_BINARY_DIR}/fftw-3.3.10-build EXCLUDE_FROM_ALL)
get_property(
    all_targets
    DIRECTORY ${CMAKE_BINARY_DIR}/fftw-3.3.10
    PROPERTY BUILDSYSTEM_TARGETS)

set_target_properties(fftw3f PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set_target_properties(fftw3f PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ╭──────────────────────────────────────╮
# │               Objects                │
# ╰──────────────────────────────────────╯
pd_add_external(entropy "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/entropy.cpp")
pd_add_external(euclidean "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/euclidean.cpp")
pd_add_external(kalman "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/kalman.cpp")
pd_add_external(kl "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/kullback-leibler.cpp")
pd_add_external(
    xconv~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/xconvolve~.cpp"
    LINK_LIBRARIES
    fftw3f)

# array
pd_add_external(a.rotate "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/a.rotate.cpp")
pd_add_external(a.invert "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/a.invert.cpp")
pd_add_external(a.sum "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/a.sum.cpp")

# manipulations
pd_add_external(infinite.record~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/infinite.record.cpp")
pd_add_external(fdn~ "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/fdn~.cpp")

# instruments
find_package(Boost REQUIRED)
target_link_libraries(OpenScofo PRIVATE Boost::headers)
pd_add_external(besseldrum~ "${CMAKE_CURRENT_SOURCE_DIR}/src/instruments/besseldrum~.cpp")
set_target_properties(
    besseldrum_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)
target_compile_definitions(besseldrum_tilde PRIVATE USE_BOOST_BESSEL)

# ╭──────────────────────────────────────╮
# │                 stk                  │
# ╰──────────────────────────────────────╯
set(BUILD_SHARED
    OFF
    CACHE BOOL "" FORCE)
set(BUILD_STATIC
    ON
    CACHE BOOL "" FORCE)
set(COMPILE_PROJECTS
    OFF
    CACHE BOOL "" FORCE)
set(REALTIME
    OFF
    CACHE BOOL "" FORCE)
set(INSTALL_HEADERS
    OFF
    CACHE BOOL "" FORCE)

cpmaddpackage("gh:thestk/stk#master")

if(TARGET stk)
    set_target_properties(stk PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(stk PRIVATE __OS_LINUX__)
    elseif(APPLE)
        target_compile_definitions(stk PRIVATE __OS_MACOSX__)
    elseif(WIN32)
        target_compile_definitions(stk PRIVATE __OS_WINDOWS__)
    endif()
endif()

pd_add_external(
    clarinet~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/instruments/clarinet~.cpp"
    LINK_LIBRARIES
    stk)

target_include_directories(clarinet_tilde PRIVATE "${stk_SOURCE_DIR}/include")

# ╭──────────────────────────────────────╮
# │                Reverb                │
# ╰──────────────────────────────────────╯
cpmaddpackage("gh:PelleJuul/zita-rev1#master")
add_library(zitarev1 STATIC "${zita-rev1_SOURCE_DIR}/source/pareq.cpp;${zita-rev1_SOURCE_DIR}/source/reverb.cpp")
set_target_properties(zitarev1 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(zitarev1 PUBLIC "${zita-rev1_SOURCE_DIR}/source")
pd_add_external(
    zita.rev~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/zita.rev~.cpp"
    LINK_LIBRARIES
    zitarev1)

set(PYTHON OFF)
cpmaddpackage("gh:charlesneimog/loris#master")
set_target_properties(
    loris
    PROPERTIES POSITION_INDEPENDENT_CODE ON
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)
target_include_directories(loris PRIVATE ${CMAKE_BINARY_DIR}/fftw-3.3.10/api)

# freeze
pd_add_external(
    lfreeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/lfreeze~.cpp"
    LINK_LIBRARIES
    loris)

# main object
pd_add_external(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/xlab.cpp")

pd_add_external(
    xfreeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/xfreeze~.cpp"
    LINK_LIBRARIES
    fftw3f)

pd_add_external(
    gfreeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/gfreeze~.cpp"
    LINK_LIBRARIES
    fftw3f)

target_include_directories(xfreeze_tilde PRIVATE ${CMAKE_BINARY_DIR}/fftw-3.3.10/api)
target_include_directories(gfreeze_tilde PRIVATE ${CMAKE_BINARY_DIR}/fftw-3.3.10/api)
# include_directories(${CMAKE_BINARY_DIR}/)
pd_add_external(xclick~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/xclick~.cpp")
pd_add_external(xcurve~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/xcurve~.cpp")

set(NW_GVERB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/nw.gverb_tilde.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/reverb_bb.cpp")
pd_add_external(
    nw.gverb~
    "${NW_GVERB_SOURCES}"
    TARGET
    nw_gverb_tilde)

# Synth
cpmaddpackage("gh:schellingb/TinySoundFont#main")
pd_add_external(tsf~ "${CMAKE_CURRENT_SOURCE_DIR}/src/synth/tsf~.cpp")
target_include_directories(tsf_tilde PRIVATE ${TinySoundFont_SOURCE_DIR}/)

# Single objects maintained as part of xlab.
if(NOT EMSCRIPTEN)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/ambi~" "${CMAKE_BINARY_DIR}/single/ambi")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/pv~" "${CMAKE_BINARY_DIR}/single/pv")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/mar~" "${CMAKE_BINARY_DIR}/single/mar")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/dragonfly~" "${CMAKE_BINARY_DIR}/single/dragonfly")

# Collections maintained as part of xlab.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/mutable" "${CMAKE_BINARY_DIR}/mutable")

if(NOT TARGET onsetsds_tilde)
    pd_add_external(onsetsds~
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/mir/onsetsds.c;${CMAKE_CURRENT_SOURCE_DIR}/src/mir/onsetsds~.c")
endif()

# ╭──────────────────────────────────────╮
# │             DDDD Package             │
# ╰──────────────────────────────────────╯
include(FetchContent)
FetchContent_Declare(
    dddd
    GIT_REPOSITORY https://github.com/EL-LOCUS-SOLUS/pd-dddd
    GIT_TAG main)
FetchContent_MakeAvailable(dddd)
FetchContent_GetProperties(dddd)
set(DDDD_PATH ${dddd_SOURCE_DIR})
set(DDDD_LUA ${DDDD_PATH}/dddd.lua)
message(STATUS "dddd file: ${DDDD_LUA}")

# ╭──────────────────────────────────────╮
# │         Externals Libraries          │
# ╰──────────────────────────────────────╯

if(IAMCHARLES)
    add_subdirectory(../pd-orchidea ${CMAKE_BINARY_DIR}/pd-orchidea)
    add_subdirectory(../pd-upic ${CMAKE_BINARY_DIR}/pd-upic)
    add_subdirectory(../pd-bhack ${CMAKE_BINARY_DIR}/pd-bhack)

    if(NOT EMSCRIPTEN)
        add_subdirectory(../py4pd ${CMAKE_BINARY_DIR}/py4pd)
        add_subdirectory(../pd-lua ${CMAKE_BINARY_DIR}/pd-lua)
    endif()

else()
    # Temporary package fallbacks until these sources are moved into pd-xlab.
    cpmaddpackage("gh:charlesneimog/pd-orchidea#main")
    cpmaddpackage("gh:charlesneimog/pd-upic#main")
    cpmaddpackage("gh:EL-LOCUS-SOLUS/pd-bhack#main")

    # Packages intentionally kept outside pd-xlab.
    if(NOT EMSCRIPTEN)
        cpmaddpackage("gh:charlesneimog/py4pd#master")
        cpmaddpackage("gh:EL-LOCUS-SOLUS/pd-lua#main")
    endif()
endif()

target_link_libraries(xlab PRIVATE lua)

# ╭──────────────────────────────────────╮
# │ Lua, Python Objects + Audio and Data │
# ╰──────────────────────────────────────╯
file(GLOB LUA_OBJECTS "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/*.pd_lua")
pd_add_datafile(xlab "${LUA_OBJECTS}")
file(GLOB JI_HELP_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/ji*-help.pd")
pd_add_datafile(xlab "${JI_HELP_FILES}")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/l.gain~-help.pd")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/rhythm-help.pd")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/sieve.lua")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/om_ji.lua")

file(GLOB PYTHON_OBJECTS "${CMAKE_CURRENT_SOURCE_DIR}/src/python/*.pd_py")
pd_add_datafile(xlab "${PYTHON_OBJECTS}")

file(GLOB AUDIO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/audios/*.wav")
pd_add_datafile(xlab "${AUDIO_FILES}")

file(GLOB ABS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/abs/*.pd")
pd_add_datafile(xlab "${ABS_FILES}")
pd_add_datafile(xlab "${DDDD_LUA}")

# ╭──────────────────────────────────────╮
# │             xlab library             │
# ╰──────────────────────────────────────╯
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(
    edit_files
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/xlab"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/resources/edit_patches.py" "${CMAKE_BINARY_DIR}/xlab"
    COMMENT "Editing generated files with Python")

add_dependencies(
    edit_files # target
    # fft
    xfreeze_tilde
    gfreeze_tilde
    # lua
    upic
    orchidea
    bhack
    # single
    pv_tilde
    mar_tilde
    dragonfly_tilde
    besseldrum_tilde
    # mutable
    elements_tilde
    rings_tilde
    # synth
    tsf_tilde)

if(NOT EMSCRIPTEN)
    add_dependencies(edit_files ambi_tilde py4pd)
endif()

add_dependencies(xlab edit_files)
