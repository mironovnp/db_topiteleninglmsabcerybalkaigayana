cmake_minimum_required(VERSION 3.14)
project(databasetopit VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# ── cpp-httplib ──
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
)
FetchContent_MakeAvailable(httplib)

# ── nlohmann/json ──
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# ── Engine (internal static lib) ──
add_library(dbengine STATIC
    src/engine/storage.cpp
    src/engine/parser.cpp
    src/engine/executor.cpp
    src/engine/buffer_pool.cpp
    src/engine/btree.cpp
    src/engine/wal.cpp
    src/engine/cell_value.cpp
    src/engine/row_codec.cpp
)
target_include_directories(dbengine PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dbengine PUBLIC nlohmann_json::nlohmann_json)

# ── Server executable ──
add_executable(dbserver
    src/server/main.cpp
    src/server/server.cpp
)
target_link_libraries(dbserver PRIVATE dbengine httplib::httplib)
target_include_directories(dbserver PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ── Client library (static) ──
add_library(dbclient STATIC
    src/client_lib/db_client.cpp
)
target_include_directories(dbclient PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dbclient PUBLIC httplib::httplib nlohmann_json::nlohmann_json)

# ── CLI executable ──
add_executable(dbcli
    src/cli/main.cpp
)
target_link_libraries(dbcli PRIVATE dbclient)
target_include_directories(dbcli PRIVATE ${CMAKE_SOURCE_DIR}/include)

# ── Tests executable ──
add_executable(db_tests
    src/engine/executor_tests.cpp
)
target_link_libraries(db_tests PRIVATE dbengine)
target_include_directories(db_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
