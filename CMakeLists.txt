cmake_minimum_required(VERSION 3.24)
project(global_placement LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Application libraries are static; the operating-system C/C++ runtime may be
# shared. Dependency sources can be reused from the existing OpenROAD cache.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static application libraries" FORCE)
set(GP_DEPENDENCY_CACHE "" CACHE PATH
    "Existing OpenROAD Bazel external directory containing dependency sources")
add_subdirectory(src/libs)

# Algorithm targets belong here. Learning tools build separately in docs/plan/tools.
add_executable(odb_hello src/main.cpp)
target_link_libraries(odb_hello PRIVATE opendb_core)
target_compile_options(odb_hello PRIVATE -Wall -Wextra -Wpedantic)
