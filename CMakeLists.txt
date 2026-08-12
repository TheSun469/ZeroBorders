cmake_minimum_required(VERSION 3.20)
project(ZeroBorders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_DEBUG_POSTFIX d)

# Match the MSVC DLL runtime across all targets (required by shared third-party libs)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# All third-party sources/build intermediates live under build/ThirdParty (read-only source tree)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/ThirdParty" CACHE PATH "Third-party base dir" FORCE)
set(FETCHCONTENT_QUIET OFF)

include(FetchContent)

# nlohmann/json (header-only) - used by UDP discovery and config
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Qt 6 (Widgets + Network) - GUI framework
# Qt is installed locally (not fetched); point CMAKE_PREFIX_PATH at its CMake config.
set(CMAKE_PREFIX_PATH "C:/Qt/Qt6.11.1/6.11.1/msvc2022_64" CACHE PATH "Qt6 prefix" FORCE)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
# Run moc/rcc automatically on Q_OBJECT classes and .qrc files.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )
endif()

add_subdirectory(src)
