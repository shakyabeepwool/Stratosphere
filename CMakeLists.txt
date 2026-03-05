cmake_minimum_required(VERSION 3.20)
project(MyEngineRoot VERSION 0.1 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Debug: print how many entities each dirty-query-based ECS system processes.
option(ECS_DIRTY_DEBUG "Enable per-system ECS dirty-row logging" OFF)
if (ECS_DIRTY_DEBUG)
  add_compile_definitions(ECS_DIRTY_DEBUG=1)
endif()

# Default to Debug if not provided (useful for single-config)
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Production build flag (used to compile out console logging)
#  - Release / MinSizeRel => ENGINE_PRODUCTION=1
#  - everything else       => ENGINE_PRODUCTION=0
add_compile_definitions(
  ENGINE_PRODUCTION=$<IF:$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>,1,0>
)

# Make FetchContent available
include(FetchContent)

# Header-only glm (fetched once for all subprojects)
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# Add subprojects
add_subdirectory(Engine)
add_subdirectory(Sample)