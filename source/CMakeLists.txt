cmake_minimum_required(VERSION 3.10)
project(OllamaCppClient CXX)

# Use C++17 or higher for modern features (inline variables, std::filesystem, ...)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Release build unless the user asked for something else.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# 1. Header-only dependencies (cpp-httplib and nlohmann/json)
#
#    These live outside the repo. By default we look for them as sibling
#    checkouts next to the project (../../cpp-httplib and ../../json), but the
#    locations can be overridden on the command line, e.g.
#        cmake -DHTTPLIB_INCLUDE_DIR=/path -DJSON_INCLUDE_DIR=/path ...
#    and we fall back to any system-installed copy (find_path) if present.
# ---------------------------------------------------------------------------
find_path(HTTPLIB_INCLUDE_DIR
    NAMES httplib.h
    HINTS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-httplib"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-httplib/include"
)

find_path(JSON_INCLUDE_DIR
    NAMES nlohmann/json.hpp
    HINTS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../json/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../json/single_include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../json"
)

if(NOT HTTPLIB_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find httplib.h. Clone https://github.com/yhirose/cpp-httplib "
        "next to this project (../../cpp-httplib) or pass "
        "-DHTTPLIB_INCLUDE_DIR=/path/to/dir")
endif()

if(NOT JSON_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find nlohmann/json.hpp. Clone https://github.com/nlohmann/json "
        "next to this project (../../json) or pass "
        "-DJSON_INCLUDE_DIR=/path/to/include")
endif()

message(STATUS "Using cpp-httplib headers from: ${HTTPLIB_INCLUDE_DIR}")
message(STATUS "Using nlohmann/json headers from: ${JSON_INCLUDE_DIR}")

# 2. Handle threading and CURL
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)

# whisper.cpp: offline speech-to-text engine for Voca (see voca.hpp).
# Vendored via FetchContent so there's nothing to check in - only the build
# step needs network access, runtime stays fully offline. GGML_CUDA=ON lets
# it run on a local NVIDIA GPU when present (falls back to CPU automatically
# otherwise - see whisper_context_params::use_gpu in voca.cpp).
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(CMAKE_CUDA_ARCHITECTURES "61" CACHE STRING "" FORCE)
FetchContent_Declare(
    whisper_cpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.9.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(whisper_cpp)

# 3. Define the executable
add_executable(olli
    ../source/main.cpp
    ../source/olla.cpp
    ../source/helper_olli.cpp
    ../source/tools_helper.cpp
    ../source/stringthings.cpp
    ../source/audio_control.cpp
    ../source/tts.cpp
    ../source/voca.cpp
    ../source/sidetrack.cpp
    ../source/threading.cpp
    ../source/fled_time.cpp
)

# 4. Include directories.
#    Marking the third-party headers as SYSTEM is critical when building with
#    -Werror: it prevents warnings inside those headers from failing the build.
target_include_directories(olli SYSTEM PRIVATE
    ${HTTPLIB_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
    ${whisper_cpp_SOURCE_DIR}/include
    ${whisper_cpp_SOURCE_DIR}/ggml/include
)

# 5. Compiler-specific strict warning flags
if(MSVC)
    target_compile_options(olli PRIVATE /W4 /WX)
else()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(olli PRIVATE
            -Weverything
            -Werror
            -Wno-volatile
        )
    else()
        # GCC does not recognize -Weverything, so build a strict set by hand.
        target_compile_options(olli PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wmisleading-indentation
            -Werror
            -Wno-volatile
        )
    endif()
endif()

# 6. Link libraries
target_link_libraries(olli PRIVATE Threads::Threads CURL::libcurl whisper PkgConfig::PORTAUDIO)
