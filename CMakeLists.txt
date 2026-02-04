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
# ---- UTCP core ----
set(UTCP_SOURCES
    src/net/utcp_api.c
    src/net/utils/socket_wrappers.c
    src/net/utils/utcp_utils.c
)

# ---- Shared utilities ----
set(UTIL_SOURCES
    src/util/util.c
)

# ---- Client ----
add_executable(client
    src/client.c
    ${UTCP_SOURCES}
    ${UTIL_SOURCES}
)

# ---- Server ----
add_executable(server
    src/server.c
    ${UTCP_SOURCES}
    ${UTIL_SOURCES}
)
