cmake_minimum_required(VERSION 3.10)
project(OllamaCppClient)

# Use C++17 or higher for modern features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Define paths to your dependencies
set(HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../cpp-httplib")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../json/include")
    set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../json/include")
else()
    set(JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../json")
endif()

# 2. Add include directories
# Using SYSTEM is critical when using -Werror; it prevents warnings 
# in third-party headers from stopping your build.
include_directories(SYSTEM ${HTTPLIB_DIR})
include_directories(SYSTEM ${JSON_DIR})

# 3. Handle threading and CURL
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# 4. Define the executable
add_executable(olli 
    ../source/main.cpp
    ../source/olla.cpp
    ../source/helper_olli.cpp
    ../source/tools.cpp
    ../source/tools_helper.cpp
    ../source/stringthings.cpp
)

# 5. Compiler-Specific Strict Warning Flags
if(MSVC)
    # Windows/Visual Studio flags
    target_compile_options(olli PRIVATE /W4 /WX)
else()
    # Check if the compiler is Clang or GCC
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Clang supports -Weverything
        target_compile_options(olli PRIVATE 
            -Weverything 
            -Werror 
            -Wno-volatile
        )
    else()
        # GCC does not recognize -Weverything. 
        # We manually build a strict set of flags.
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
target_link_libraries(olli PRIVATE Threads::Threads CURL::libcurl)