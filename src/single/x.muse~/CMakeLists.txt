cmake_minimum_required(VERSION 3.19)
project(pd_musesampler LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This IPC implementation currently supports Linux only")
endif()

function(download_file url destination)
    get_filename_component(destination_dir "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${destination_dir}")
    file(
        DOWNLOAD "${url}" "${destination}.tmp"
        STATUS status
        TLS_VERIFY ON)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE "${destination}.tmp")
        list(GET status 1 error)
        message(FATAL_ERROR "Could not download ${url}: ${error}")
    endif()
    file(RENAME "${destination}.tmp" "${destination}")
endfunction()

# API is taken from current muse_framework (GPLv3).
set(MUSESAMPLER_API_COMMIT
    "main"
    CACHE STRING "muse_framework Git ref, preferably a pinned commit")
set(MUSESAMPLER_APITYPES "${CMAKE_CURRENT_BINARY_DIR}/generated/apitypes.h")
if(NOT EXISTS "${MUSESAMPLER_APITYPES}")
    download_file(
        "https://raw.githubusercontent.com/musescore/muse_framework/${MUSESAMPLER_API_COMMIT}/framework/musesampler/internal/apitypes.h"
        "${MUSESAMPLER_APITYPES}")
endif()

add_executable(mscore mscore.cpp)
set_target_properties(mscore PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories(mscore PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")

pd_add_external(x.muse~ "x.muse~.cpp" TARGET musesampler_tilde)
pd_add_datafile(musesampler_tilde "${CMAKE_CURRENT_BINARY_DIR}/mscore")

set_target_properties(musesampler_tilde PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_include_directories(musesampler_tilde PRIVATE include)
if(PD_HEADER_PATH)
    target_include_directories(musesampler_tilde PRIVATE "${PD_HEADER_PATH}")
endif()
target_link_libraries(musesampler_tilde PRIVATE ${CMAKE_DL_LIBS} rt)
add_dependencies(musesampler_tilde mscore)
add_custom_command(
    TARGET musesampler_tilde
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:mscore>"
            "$<TARGET_FILE_DIR:musesampler_tilde>/mscore"
    COMMENT "Copying MuseSampler helper next to Pd external")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/musesampler~-help.pd")
    pd_add_datafile(musesampler~ "${CMAKE_CURRENT_SOURCE_DIR}/musesampler~-help.pd")
endif()
