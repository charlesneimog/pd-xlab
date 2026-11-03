# CloudSeedCore ships a console example, rather than a CMake library. Build only
# its DSP sources, with local portability/correctness fixes in a build-tree copy.
CPMAddPackage(
    NAME CloudSeedCore
    GITHUB_REPOSITORY GhostNoteAudio/CloudSeedCore
    GIT_TAG deb21ded9eb7dad9b3ff94ce1ba96a963716594e
    DOWNLOAD_ONLY YES)

set(CLOUDSEED_SOURCE_DIR "${CMAKE_BINARY_DIR}/cloudseedcore")
set(CLOUDSEED_CORE_SOURCES DSP/Biquad.cpp DSP/RandomBuffer.cpp Parameters.cpp)
file(GLOB_RECURSE CLOUDSEED_HEADERS CONFIGURE_DEPENDS
     RELATIVE "${CloudSeedCore_SOURCE_DIR}" "${CloudSeedCore_SOURCE_DIR}/*.h")

foreach(CLOUDSEED_FILE IN LISTS CLOUDSEED_CORE_SOURCES CLOUDSEED_HEADERS)
    get_filename_component(CLOUDSEED_DESTINATION "${CLOUDSEED_SOURCE_DIR}/${CLOUDSEED_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${CLOUDSEED_DESTINATION}")
    configure_file("${CloudSeedCore_SOURCE_DIR}/${CLOUDSEED_FILE}"
                   "${CLOUDSEED_SOURCE_DIR}/${CLOUDSEED_FILE}" COPYONLY)
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/PatchCloudSeedCore.cmake")

add_library(cloudseedcore STATIC
            "${CLOUDSEED_SOURCE_DIR}/DSP/Biquad.cpp"
            "${CLOUDSEED_SOURCE_DIR}/DSP/RandomBuffer.cpp"
            "${CLOUDSEED_SOURCE_DIR}/Parameters.cpp")

set_target_properties(cloudseedcore PROPERTIES
                      POSITION_INDEPENDENT_CODE ON
                      CXX_STANDARD 14
                      CXX_STANDARD_REQUIRED ON
                      CXX_EXTENSIONS OFF)

target_compile_features(cloudseedcore PUBLIC cxx_std_14)
target_compile_definitions(cloudseedcore PUBLIC BUFFER_SIZE=1024 MAX_STR_SIZE=32)
target_include_directories(cloudseedcore PUBLIC "${CLOUDSEED_SOURCE_DIR}")

set(CLOUDSEED_LICENSE_FILE "${CMAKE_BINARY_DIR}/CloudSeedCore-LICENSE.txt")
configure_file("${CloudSeedCore_SOURCE_DIR}/LICENSE" "${CLOUDSEED_LICENSE_FILE}" COPYONLY)
