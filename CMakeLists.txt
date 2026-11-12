cmake_minimum_required(VERSION 3.20)

# Static CRT: tldserver.exe runs on a bare Windows without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(tldserver LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The mod (polar-link) is the submodule `mod`: its network library is built from it here, so the server always speaks
# exactly the protocol of the mod commit it points at.
set(MOD_DIR ${CMAKE_SOURCE_DIR}/mod)
if(NOT EXISTS ${MOD_DIR}/src/net/session.cpp)
  message(FATAL_ERROR "mod/ is empty: git submodule update --init mod")
endif()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE SERVER_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY ${MOD_DIR}
                OUTPUT_VARIABLE MOD_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SERVER_BUILD)
  set(SERVER_BUILD "local")
endif()
if(NOT MOD_BUILD)
  set(MOD_BUILD "?")
endif()
# (two plain words: a value with spaces and brackets reached the compiler empty)
add_compile_definitions(TLDSERVER_BUILD="${SERVER_BUILD}" TLDSERVER_MOD="${MOD_BUILD}")
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /Zi)
  add_link_options(/DEBUG /OPT:REF /OPT:ICF)
endif()

add_library(tldnet STATIC ${MOD_DIR}/src/net/session.cpp ${MOD_DIR}/src/net/udp_transport.cpp
                          ${MOD_DIR}/src/net/steam_transport.cpp)
target_include_directories(tldnet PUBLIC ${MOD_DIR}/src)
target_link_libraries(tldnet PUBLIC ws2_32)

# The animals' arbiter: plain code (the mod's protocol.h and wildlife_own.h only), so it has a test without a network.
add_library(tldserver_wildlife STATIC server/wildlife.cpp)
target_include_directories(tldserver_wildlife PUBLIC ${CMAKE_SOURCE_DIR} ${MOD_DIR}/src)

add_library(tldserver_core STATIC server/server.cpp server/world.cpp)
target_include_directories(tldserver_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(tldserver_core PUBLIC tldnet tldserver_wildlife)

add_executable(tldserver server/main.cpp)
target_link_libraries(tldserver PRIVATE tldserver_core)

add_executable(server_test tests/server_test.cpp)
target_link_libraries(server_test PRIVATE tldserver_core)

add_executable(world_test tests/world_test.cpp)
target_link_libraries(world_test PRIVATE tldserver_core)

add_executable(shared_test tests/shared_test.cpp)
target_link_libraries(shared_test PRIVATE tldserver_core)

add_executable(wildlife_test tests/wildlife_test.cpp)
target_link_libraries(wildlife_test PRIVATE tldserver_wildlife)
