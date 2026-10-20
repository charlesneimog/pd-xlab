# Upstream's CMake build always builds both Lua and LuaJIT. Use only its
# regular-Lua wrapper, which includes the bundled interpreter and Lua headers.
set(PDLUA_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/pdlua-nojit")
file(MAKE_DIRECTORY "${PDLUA_BUILD_DIR}/luas")
configure_file("${pdlua_SOURCE_DIR}/luas/lua.c" "${PDLUA_BUILD_DIR}/luas/lua.c" COPYONLY)

# The shared implementation calls the second loader unconditionally. Disable
# those two setup blocks in a generated copy, leaving the checkout untouched.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pdlua_SOURCE_DIR}/pdlua.c")
file(READ "${pdlua_SOURCE_DIR}/pdlua.c" PDLUA_SOURCE)
string(REPLACE "#ifndef LUA_USE_JIT\n#ifdef PLUGDATA"
               "#if 0 /* xlab: LuaJIT loader disabled */\n#ifdef PLUGDATA" PDLUA_SOURCE "${PDLUA_SOURCE}")
string(REPLACE "#ifndef LUA_USE_JIT\n    void pdluajit_instance_setup();"
               "#if 0 /* xlab: LuaJIT instance disabled */\n    void pdluajit_instance_setup();"
               PDLUA_SOURCE "${PDLUA_SOURCE}")
file(GENERATE OUTPUT "${PDLUA_BUILD_DIR}/pdlua.c" CONTENT "${PDLUA_SOURCE}")

execute_process(
    COMMAND git describe --tags
    WORKING_DIRECTORY "${pdlua_SOURCE_DIR}"
    OUTPUT_VARIABLE PDLUA_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT PDLUA_VERSION)
    set(PDLUA_VERSION "unknown")
endif()

# Keep the external named 'lua': xlab loads it through sys_load_lib at runtime.
pd_add_external(lua "${PDLUA_BUILD_DIR}/luas/lua.c")
target_include_directories(lua PRIVATE "${pdlua_SOURCE_DIR}" "${pdlua_SOURCE_DIR}/luas")
target_compile_definitions(lua PRIVATE MAKE_LIB PDLUA_VERSION="${PDLUA_VERSION}")
if(APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif(WIN32)
    target_compile_definitions(lua PRIVATE LUA_USE_WINDOWS)
elseif(UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
endif()
if(DEFINED PD_MULTICHANNEL)
    target_compile_definitions(lua PRIVATE PD_MULTICHANNEL=${PD_MULTICHANNEL})
endif()
target_link_libraries(lua PRIVATE ${CMAKE_DL_LIBS})
pd_add_datafile(lua "${pdlua_SOURCE_DIR}/pd.lua")
