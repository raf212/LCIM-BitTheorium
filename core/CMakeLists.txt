cmake_minimum_required(VERSION 3.10)
project(AtomicCIM LANGUAGES CXX)

# Produce compile_commands.json (useful for many IntelliSense tools)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Source layout
set(HEADERS_DIR ${CMAKE_SOURCE_DIR}/headers)
set(SRC_DIR     ${CMAKE_SOURCE_DIR}/programs)

# Prefer explicit sources; add your .cpp files
set(SRC_FILES
    ${HEADERS_DIR}/AllocNW.hpp
    ${HEADERS_DIR}/PackedCell.hpp
    ${HEADERS_DIR}/AtomicPCArray.hpp
    ${HEADERS_DIR}/AtomicDataSignalArray.hpp
    ${CMAKE_SOURCE_DIR}/main.cpp

)

# Add executable
add_executable(AtomicCIM ${SRC_FILES})

# Include headers specifically for the target
target_include_directories(AtomicCIM PRIVATE ${HEADERS_DIR})

# Compiler warnings - target-specific
if (MSVC)
    target_compile_options(AtomicCIM PRIVATE /W4 /WX)
else()
    target_compile_options(AtomicCIM PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
