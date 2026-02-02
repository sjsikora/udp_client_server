cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(udp_client_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable warnings
if (CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Global include directory
include_directories(${PROJECT_SOURCE_DIR}/include)

# ---- UTCP library sources ----
set(UTCP_SOURCES
    src/net/utcp_api.c
)

# ---- Client executable ----
add_executable(client
    src/client.c
    ${UTCP_SOURCES}
)

# ---- Server executable ----
add_executable(server
    src/server.c
    ${UTCP_SOURCES}
)
