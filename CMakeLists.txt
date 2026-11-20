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

# Optional observational trace support; the numerical solver has no JSON logic.
add_library(placement_tracer STATIC src/tracer/placement_tracer.cpp
                                   src/tracer/placement_metrics.cpp)
target_include_directories(placement_tracer PUBLIC src)
target_link_libraries(placement_tracer PUBLIC opendb_core Eigen3::Eigen PRIVATE placement_json)
target_compile_options(placement_tracer PRIVATE -Wall -Wextra -Wpedantic)

# Algorithm targets belong here. Learning tools build separately in docs/plan/tools.
add_executable(odb_hello src/main.cpp)
target_include_directories(odb_hello PRIVATE src)
target_link_libraries(odb_hello PRIVATE opendb_core Eigen3::Eigen placement_json placement_tracer)
target_compile_options(odb_hello PRIVATE -Wall -Wextra -Wpedantic)

# Give standalone first-party headers the application's exact compiler flags.
# Keep the full compilation database unchanged for normal source-file tooling.
add_custom_target(clangd_database
  COMMAND "${CMAKE_COMMAND}"
    "-DGP_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DGP_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    -P "${CMAKE_SOURCE_DIR}/cmake/UpdateClangdDatabase.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/clangd/compile_commands.json"
  VERBATIM)
add_dependencies(odb_hello clangd_database)

include(CTest)
if(BUILD_TESTING)
  add_executable(placement_tracer_smoke tests/placement_tracer_smoke.cpp)
  target_link_libraries(placement_tracer_smoke PRIVATE placement_tracer placement_json)
  target_compile_options(placement_tracer_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME placement_tracer_smoke COMMAND placement_tracer_smoke)

  add_executable(json_smoke tests/json_smoke.cpp)
  target_link_libraries(json_smoke PRIVATE placement_json)
  target_compile_options(json_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME json_smoke COMMAND json_smoke)

  add_executable(eigen_smoke tests/eigen_smoke.cpp)
  target_link_libraries(eigen_smoke PRIVATE Eigen3::Eigen)
  target_compile_options(eigen_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME eigen_smoke COMMAND eigen_smoke)

  add_executable(gordian_builder_smoke tests/gordian_builder_smoke.cpp)
  target_include_directories(gordian_builder_smoke PRIVATE src)
  target_link_libraries(gordian_builder_smoke PRIVATE opendb_core Eigen3::Eigen placement_tracer)
  target_compile_options(gordian_builder_smoke PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME gordian_builder_smoke COMMAND gordian_builder_smoke)

  add_executable(module_registry_views tests/module_registry_views.cpp)
  target_include_directories(module_registry_views PRIVATE src)
  target_link_libraries(module_registry_views PRIVATE opendb_core)
  target_compile_options(module_registry_views PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME module_registry_views COMMAND module_registry_views)

endif()
