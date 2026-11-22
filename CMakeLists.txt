cmake_minimum_required(VERSION 3.20)
project(minicraft_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL)

set(MINICRAFT_CORE_SOURCES
    src/common/Log.cpp
    src/common/Asm.cpp
    src/common/Config.cpp
    src/common/Json.cpp
    src/common/Crypto.cpp
    src/common/Uuid.cpp
    src/common/ThreadPool.cpp
    src/net/ByteBuffer.cpp
    src/net/Zlib.cpp
    src/net/Encryption.cpp
    src/net/Connection.cpp
    src/net/EventLoop.cpp
    src/net/TcpServer.cpp
    src/protocol/Chat.cpp
    src/protocol/Auth.cpp
    src/protocol/ItemStack.cpp
    src/world/Chunk.cpp
    src/world/World.cpp
    src/lang/Lexer.cpp
    src/lang/Compiler.cpp
    src/lang/Disassembler.cpp
    src/lang/Vm.cpp
    src/lang/ScriptEngine.cpp
    src/mod/ModManager.cpp
    src/server/PlayerManager.cpp
    src/server/CommandManager.cpp
    src/server/Server.cpp
)

set(MINICRAFT_NASM_ENABLED OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    find_program(NASM_EXECUTABLE nasm)
    if(NASM_EXECUTABLE)
        enable_language(ASM_NASM)
        set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
        list(APPEND MINICRAFT_CORE_SOURCES src/asm/fastmem.asm)
        set(MINICRAFT_NASM_ENABLED ON)
    endif()
endif()

add_library(minicraft_core STATIC ${MINICRAFT_CORE_SOURCES})

if(MINICRAFT_NASM_ENABLED)
    target_compile_definitions(minicraft_core PUBLIC MINICRAFT_HAVE_NASM=1)
    target_include_directories(minicraft_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/asm)
endif()

target_include_directories(minicraft_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(minicraft_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter>)
target_link_libraries(minicraft_core PUBLIC Threads::Threads ZLIB::ZLIB OpenSSL::Crypto ${CMAKE_DL_LIBS})

if(CURL_FOUND)
    target_compile_definitions(minicraft_core PUBLIC MINICRAFT_HAVE_CURL=1)
    if(TARGET CURL::libcurl)
        target_link_libraries(minicraft_core PUBLIC CURL::libcurl)
    else()
        target_link_libraries(minicraft_core PUBLIC ${CURL_LIBRARIES})
        target_include_directories(minicraft_core PUBLIC ${CURL_INCLUDE_DIRS})
    endif()
endif()

add_executable(minicraft src/main.cpp)
target_link_libraries(minicraft PRIVATE minicraft_core)

option(MINICRAFT_BUILD_EXAMPLE_MOD "Build the example C++ mod" ON)
if(MINICRAFT_BUILD_EXAMPLE_MOD)
    add_library(hello_mod MODULE mods/hello_mod.cpp)
    target_include_directories(hello_mod PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(hello_mod PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>)
    set_target_properties(hello_mod PROPERTIES
        PREFIX ""
        OUTPUT_NAME "hello_mod"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mods")
endif()

option(MINICRAFT_BUILD_PROBE "Build the protocol probe client" ON)
if(MINICRAFT_BUILD_PROBE)
    add_executable(probe tests/probe.cpp)
    target_link_libraries(probe PRIVATE minicraft_core)

    add_executable(core_test tests/core_test.cpp)
    target_link_libraries(core_test PRIVATE minicraft_core OpenSSL::Crypto)
endif()
