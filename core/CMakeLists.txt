cmake_minimum_required(VERSION 3.14)
project(AtomicCIM LANGUAGES CXX)

# Produce compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default build type for single-config generators
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the build type." FORCE)
endif()

# Project layout variables (adjust if your tree differs)
set(HEADERS_DIR "${CMAKE_SOURCE_DIR}/headers")
set(SRC_DIR     "${CMAKE_SOURCE_DIR}/src")
set(BINDINGS_DIR "${CMAKE_SOURCE_DIR}/bindings")

# Optionally point to a local pybind11 checkout
option(PYBIND11_DIR "" "Path to a local pybind11 checkout (optional)")

# Acquire pybind11 (local or FetchContent)
if(PYBIND11_DIR)
    if(EXISTS "${PYBIND11_DIR}/CMakeLists.txt")
        message(STATUS "Using local pybind11 at: ${PYBIND11_DIR}")
        add_subdirectory("${PYBIND11_DIR}" EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "PYBIND11_DIR set but not found: ${PYBIND11_DIR}")
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(
      pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG        v3.0.1
    )
    FetchContent_MakeAvailable(pybind11)
endif()

# ---------- Optional libnuma ----------
find_library(NUMALIB NAMES numa libnuma)
if(NUMALIB)
    message(STATUS "Found libnuma: ${NUMALIB}")
    set(HAVE_LIBNUMA 1)
else()
    message(STATUS "libnuma not found; building without NUMA support (optional).")
endif()

# ---------- Core static library ----------
file(GLOB CORE_SOURCES "${SRC_DIR}/*.cpp")
if(NOT CORE_SOURCES)
    message(FATAL_ERROR "No source files found in ${SRC_DIR}. Expecting at least one .cpp (e.g. src/atomiccim_core.cpp or main.cpp).")
endif()

add_library(atomiccim_core STATIC ${CORE_SOURCES})
target_include_directories(atomiccim_core PUBLIC ${HEADERS_DIR})
target_compile_features(atomiccim_core PUBLIC cxx_std_20)

# compiler warnings for core
if(MSVC)
    target_compile_options(atomiccim_core PRIVATE /W4 /WX)
else()
    target_compile_options(atomiccim_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# link libnuma if found
if(NUMALIB)
    target_compile_definitions(atomiccim_core PRIVATE HAVE_LIBNUMA=1)
    target_link_libraries(atomiccim_core PRIVATE ${NUMALIB})
endif()

# ---------- Optional CLI executable ----------
# If you have a main.cpp (CLI), put it in src/ and this will create an executable that links the core lib.
set(MAIN_SRC "${CMAKE_SOURCE_DIR}/main.cpp")
if(EXISTS ${MAIN_SRC})
    add_executable(AtomicCIM ${MAIN_SRC})
    target_link_libraries(AtomicCIM PRIVATE atomiccim_core)
    target_include_directories(AtomicCIM PRIVATE ${HEADERS_DIR})
    if(MSVC)
        target_compile_options(AtomicCIM PRIVATE /W4 /WX)
    else()
        target_compile_options(AtomicCIM PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endif()

# ---------- pybind11 Python extension (optional) ----------
# Look for binding sources in bindings/
file(GLOB BINDING_SRCS "${BINDINGS_DIR}/*.cpp")
if(BINDING_SRCS)
    # build a module per binding file or (prefer single module) choose a single entry
    # We'll expect a single module entry file named py_atomiccim.cpp or PackedCellCoreBindings.cpp
    set(PREFERRED_BINDING "${BINDINGS_DIR}/PackedCellCoreBindings.cpp")
    if(EXISTS ${PREFERRED_BINDING})
        set(MODULE_SRC ${PREFERRED_BINDING})
    else()
        # fallback: choose first .cpp binding found
        list(GET BINDING_SRCS 0 MODULE_SRC)
        message(STATUS "Using binding source: ${MODULE_SRC}")
    endif()

    pybind11_add_module(atomiccim_bind ${MODULE_SRC})
    target_link_libraries(atomiccim_bind PRIVATE atomiccim_core)
    target_include_directories(atomiccim_bind PRIVATE ${HEADERS_DIR} ${BINDINGS_DIR})
    # Ensure python extension is placed in top-level build dir for fast import during dev
    set_target_properties(atomiccim_bind PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
    foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
        set_target_properties(atomiccim_bind PROPERTIES
            "LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE}" ${CMAKE_BINARY_DIR}/${CONFIG_TYPE}
            "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE}" ${CMAKE_BINARY_DIR}/${CONFIG_TYPE}
        )
    endforeach()
else()
    message(STATUS "No binding sources (bindings/*.cpp) found — Python module will not be built.")
endif()

# Convenience targets
add_custom_target(build_python_module
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target atomiccim_bind --config $<CONFIG>
    COMMENT "Build python extension module (atomiccim_bind)"
)

add_custom_target(build_core
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target atomiccim_core --config $<CONFIG>
    COMMENT "Build atomiccim_core"
)
