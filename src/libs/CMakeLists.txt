# These build files belong to our standalone project. Vendored OpenROAD source
# remains unchanged, including its original build metadata and licenses.
if(NOT IS_DIRECTORY "${GP_DEPENDENCY_CACHE}")
  message(FATAL_ERROR
    "Set GP_DEPENDENCY_CACHE to the existing OpenROAD bazel-OpenROAD/external "
    "directory. See docs/plan/odb-hello-world.md for the offline build command.")
endif()

foreach(dependency IN ITEMS abseil-cpp fmt spdlog)
  if(NOT EXISTS "${GP_DEPENDENCY_CACHE}/${dependency}+/CMakeLists.txt")
    message(FATAL_ERROR "Missing cached source: ${dependency}+ in ${GP_DEPENDENCY_CACHE}")
  endif()
endforeach()

set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory("${GP_DEPENDENCY_CACHE}/abseil-cpp+" "${CMAKE_CURRENT_BINARY_DIR}/abseil" EXCLUDE_FROM_ALL)
add_subdirectory("${GP_DEPENDENCY_CACHE}/fmt+" "${CMAKE_CURRENT_BINARY_DIR}/fmt" EXCLUDE_FROM_ALL)
add_subdirectory("${GP_DEPENDENCY_CACHE}/spdlog+" "${CMAKE_CURRENT_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL)

# The cache holds Boost as separate modules. This checkout uses header-only
# Boost components (including its current regex implementation).
file(GLOB boost_modules LIST_DIRECTORIES true "${GP_DEPENDENCY_CACHE}/boost.*+")
set(boost_includes "")
foreach(module IN LISTS boost_modules)
  if(IS_DIRECTORY "${module}/include")
    list(APPEND boost_includes "${module}/include")
  endif()
endforeach()
if(NOT EXISTS "${GP_DEPENDENCY_CACHE}/boost.config+/include/boost/version.hpp")
  message(FATAL_ERROR "Boost module headers are missing from GP_DEPENDENCY_CACHE")
endif()
add_library(placement_boost_headers INTERFACE)
target_include_directories(placement_boost_headers SYSTEM INTERFACE ${boost_includes})

# Reuse cached Boost.JSON sources; no download or shared library is needed.
if(NOT EXISTS "${GP_DEPENDENCY_CACHE}/boost.json+/src/src.cpp")
  message(FATAL_ERROR "Boost.JSON sources are missing from GP_DEPENDENCY_CACHE")
endif()
add_library(placement_json STATIC "${GP_DEPENDENCY_CACHE}/boost.json+/src/src.cpp")
target_link_libraries(placement_json PUBLIC placement_boost_headers)

find_package(Threads REQUIRED)

# Only the utility implementation needed by the database/logger is compiled.
# No Tcl/Python bindings, compression helpers, or unrelated OpenROAD modules.
add_library(placement_utl STATIC
  utl/src/Logger.cpp
  utl/src/Metrics.cpp
  utl/src/Progress.cpp
  utl/src/CommandLineProgress.cpp
  utl/src/prometheus/metrics_server.cpp
)
target_include_directories(placement_utl SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/utl/include")
target_include_directories(placement_utl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/utl/src")
target_link_libraries(placement_utl PUBLIC
  absl::synchronization spdlog::spdlog Threads::Threads placement_boost_headers
)

# Mirrors the database-core source glob used by upstream Bazel, without
# importing the OpenROAD-wide CMake/SWIG configuration.
file(GLOB odb_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/odb/src/db/*.cpp")
add_library(opendb_core STATIC ${odb_sources})
target_include_directories(opendb_core SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/odb/include")
target_link_libraries(opendb_core PUBLIC placement_utl)
