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
    OFF
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

set_target_properties(fftw3 PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set_target_properties(fftw3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
include_directories("${CMAKE_BINARY_DIR}/fftw-3.3.10/api/")

# ╭──────────────────────────────────────╮
# │               Objects                │
# ╰──────────────────────────────────────╯
pd_add_external(x.entropy "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/x.entropy.cpp")
pd_add_external(x.euclidean "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/x.euclidean.cpp")
pd_add_external(x.kalman "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/x.kalman.cpp")
pd_add_external(x.kl "${CMAKE_CURRENT_SOURCE_DIR}/src/statistics/x.kl.cpp")
pd_add_external(
    x.conv~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/x.conv~.cpp"
    LINK_LIBRARIES
    fftw3)

# array
pd_add_external(x.array.rotate "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/x.array.rotate.cpp")
pd_add_external(x.array.invert "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/x.array.invert.cpp")
pd_add_external(x.array.sum "${CMAKE_CURRENT_SOURCE_DIR}/src/arrays/x.array.sum.cpp")

# manipulations
pd_add_external(x.infinite.record~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/x.infinite.record~.cpp")
pd_add_external(x.fdn~ "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/x.fdn~.cpp")
set_target_properties(
    x.fdn_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

# instruments
pd_add_external(x.besseldrum~ "${CMAKE_CURRENT_SOURCE_DIR}/src/instruments/x.besseldrum~.cpp")
set_target_properties(
    x.besseldrum_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)
if(EMSCRIPTEN)
    target_compile_options(x.besseldrum_tilde PRIVATE "-sUSE_BOOST_HEADERS=1")
    target_link_options(x.besseldrum_tilde PRIVATE "-sUSE_BOOST_HEADERS=1")
else()
    find_package(Boost CONFIG REQUIRED)
    target_link_libraries(x.besseldrum_tilde PRIVATE Boost::headers)
endif()

# Memoria da Chuva
pd_add_external(x.rain~ "${CMAKE_CURRENT_SOURCE_DIR}/src/others/x.rain~.cpp")
pd_add_external(x.rperlin "${CMAKE_CURRENT_SOURCE_DIR}/src/others/x.rperlin.cpp")

set_target_properties(x.rperlin PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

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
    x.clarinet~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/instruments/x.clarinet~.cpp"
    LINK_LIBRARIES
    stk)

target_include_directories(x.clarinet_tilde PRIVATE "${stk_SOURCE_DIR}/include")

# ╭──────────────────────────────────────╮
# │                Utils                 │
# ╰──────────────────────────────────────╯
pd_add_external(x.darray "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/x.darray.cpp")
pd_add_external(x.cputime "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/x.cputime.cpp")

# ╭──────────────────────────────────────╮
# │                Reverb                │
# ╰──────────────────────────────────────╯
cpmaddpackage("gh:PelleJuul/zita-rev1#master")
add_library(zitarev1 STATIC "${zita-rev1_SOURCE_DIR}/source/pareq.cpp;${zita-rev1_SOURCE_DIR}/source/reverb.cpp")
set_target_properties(zitarev1 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(zitarev1 PUBLIC "${zita-rev1_SOURCE_DIR}/source")
pd_add_external(
    x.zita.rev~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/x.zita.rev~.cpp"
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
    x.lfreeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/x.lfreeze~.cpp"
    LINK_LIBRARIES
    loris)

# main object
pd_add_external(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/xlab.cpp")

pd_add_external(
    x.freeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/x.freeze~.cpp"
    LINK_LIBRARIES
    fftw3)

pd_add_external(
    x.gfreeze~
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fft/x.gfreeze~.cpp"
    LINK_LIBRARIES
    fftw3)

target_include_directories(x.freeze_tilde PRIVATE ${CMAKE_BINARY_DIR}/fftw-3.3.10/api)
target_include_directories(x.gfreeze_tilde PRIVATE ${CMAKE_BINARY_DIR}/fftw-3.3.10/api)
# include_directories(${CMAKE_BINARY_DIR}/)
pd_add_external(x.click~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/x.click~.cpp")
pd_add_external(x.curve~ "${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/x.curve~.cpp")
set_target_properties(
    x.curve_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

set(NW_GVERB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/x.nw.gverb~.cpp"
                     "${CMAKE_CURRENT_SOURCE_DIR}/src/reverb/reverb_bb.cpp")
pd_add_external(
    x.nw.gverb~
    "${NW_GVERB_SOURCES}"
    TARGET
    x.nw.gverb_tilde)

# Synth
cpmaddpackage("gh:schellingb/TinySoundFont#main")
pd_add_external(x.tsf~ "${CMAKE_CURRENT_SOURCE_DIR}/src/synth/x.tsf~.cpp")
target_include_directories(x.tsf_tilde PRIVATE ${TinySoundFont_SOURCE_DIR}/)
set_target_properties(
    x.tsf_tilde
    PROPERTIES CXX_STANDARD 17
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF)

# Single objects maintained as part of xlab.
if(NOT EMSCRIPTEN)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/x.ambi~" "${CMAKE_BINARY_DIR}/single/ambi")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/x.pv~" "${CMAKE_BINARY_DIR}/single/pv")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/x.mar~" "${CMAKE_BINARY_DIR}/single/mar")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/single/x.dragonfly~" "${CMAKE_BINARY_DIR}/single/dragonfly")

# Collections maintained as part of xlab.
if(NOT APPLE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/src/mutable" "${CMAKE_BINARY_DIR}/mutable")
endif()

if(NOT TARGET x.onsetsds_tilde)
    pd_add_external(x.onsetsds~
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/mir/onsetsds.c;${CMAKE_CURRENT_SOURCE_DIR}/src/mir/x.onsetsds~.c")
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
file(
    GLOB
    LUA_OBJECTS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/*.pd_lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/granulador/*.pd_lua")
pd_add_datafile(xlab "${LUA_OBJECTS}")
file(
    GLOB_RECURSE
    XLAB_HELP_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/x.*-help.pd"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/x.*-help.pd")
pd_add_datafile(xlab "${XLAB_HELP_FILES}")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/sieve.lua")
pd_add_datafile(xlab "${CMAKE_CURRENT_SOURCE_DIR}/src/lua/om_ji.lua")

file(
    GLOB
    PYTHON_OBJECTS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/python/*.pd_py"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/ai/*.pd_py")
pd_add_datafile(xlab "${PYTHON_OBJECTS}")

file(GLOB AUDIO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/audios/*.wav")
pd_add_datafile(xlab "${AUDIO_FILES}")

file(
    GLOB
    ABS_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/abs/*.pd"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/x.cphasevoc~.pd"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/granulador/x.granulator.pd")
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
    x.freeze_tilde
    x.gfreeze_tilde
    # lua
    upic
    orchidea
    bhack
    # single
    x.pv_tilde
    x.mar_tilde
    x.dragonfly_tilde
    x.besseldrum_tilde
    # # mutable x.elements_tilde x.rings_tilde synth
    x.tsf_tilde)

if(NOT EMSCRIPTEN)
    add_dependencies(
        edit_files
        x.ambi_tilde
        py4pd
        lua)
endif()

add_dependencies(xlab edit_files)
