cmake_minimum_required(VERSION 3.24)
project(coalesce LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(COALESCE_SANITIZER "" CACHE STRING "thread, address or empty")
if(COALESCE_SANITIZER)
    add_compile_options(-fsanitize=${COALESCE_SANITIZER} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${COALESCE_SANITIZER})
endif()

find_package(Threads REQUIRED)

# use installed libraries when they exist, otherwise download pinned versions
include(FetchContent)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

find_package(SQLiteCpp QUIET)
if(NOT SQLiteCpp_FOUND)
    FetchContent_Declare(sqlitecpp GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git GIT_TAG 3.3.3 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(sqlitecpp)
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.12.0 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(json)
endif()

find_path(HTTPLIB_INCLUDE httplib.h)
if(NOT HTTPLIB_INCLUDE)
    FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.65.0 GIT_SHALLOW ON SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(httplib)
    set(HTTPLIB_INCLUDE ${httplib_SOURCE_DIR})
endif()

set(web_header ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.hpp)
add_custom_command(
    OUTPUT ${web_header}
    COMMAND ${CMAKE_COMMAND} -DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web -DOUT_FILE=${web_header}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS web/index.html web/style.css web/app.js web/favicon.svg cmake/embed.cmake
    VERBATIM)

add_library(coalesce_core STATIC
    src/store.cpp src/strategy.cpp src/flusher.cpp src/metrics.cpp src/bench.cpp src/server.cpp ${web_header})
target_include_directories(coalesce_core PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(coalesce_core SYSTEM PUBLIC ${HTTPLIB_INCLUDE})
target_link_libraries(coalesce_core PUBLIC SQLiteCpp nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(coalesce_core PRIVATE -Wall -Wextra)

add_executable(coalesce src/main.cpp)
target_link_libraries(coalesce PRIVATE coalesce_core)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0 GIT_SHALLOW ON)
        FetchContent_MakeAvailable(googletest)
    endif()
    include(GoogleTest)
    add_executable(coalesce_tests tests/test_strategy.cpp tests/test_flusher.cpp tests/test_metrics.cpp tests/test_bench.cpp tests/test_server.cpp)
    target_link_libraries(coalesce_tests PRIVATE coalesce_core GTest::gtest_main)
    gtest_discover_tests(coalesce_tests)
endif()
