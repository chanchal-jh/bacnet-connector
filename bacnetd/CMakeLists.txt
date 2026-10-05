cmake_minimum_required(VERSION 3.16)
project(bacnetd C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------
# bacnet-stack — use the submodule's own CMake build system.
# It defines PUBLIC include dirs (src/) and all compile definitions.
# We disable the apps and use BACnet/IP on Linux.
# -----------------------------------------------------------------------
set(BACNET_STACK_DIR "${CMAKE_SOURCE_DIR}/../bacnet-stack")

set(BACNET_STACK_BUILD_APPS    OFF CACHE BOOL "" FORCE)
set(BACDL_BIP                  ON  CACHE BOOL "" FORCE)
set(BACDL_MSTP                 OFF CACHE BOOL "" FORCE)
set(BACDL_ETHERNET             OFF CACHE BOOL "" FORCE)

add_subdirectory(${BACNET_STACK_DIR} bacnet-stack-build EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------
# bacnetd executable
# -----------------------------------------------------------------------
add_executable(bacnetd
    main.c
    ipc.c
    discovery.c
    readprop.c
    writeprop.c
    readpropmultiple.c
)

# Link against the target defined by bacnet-stack's CMake.
# This pulls in all PUBLIC include directories and compile definitions
# automatically — no manual path listing needed.
target_link_libraries(bacnetd PRIVATE bacnet-stack)

target_include_directories(bacnetd PRIVATE "${CMAKE_SOURCE_DIR}")

target_compile_options(bacnetd PRIVATE -Wall -Wextra -g)
