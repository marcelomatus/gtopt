cmake_minimum_required(VERSION 3.31.6)

# ---- Project ----

# Note: update this to your new project's name and version
project(
  gtopt
  VERSION 1.0
  LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_policy(SET CMP0167 NEW)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed.")
endif()

# ---- Add dependencies via CPM ----
# see https://github.com/TheLartians/CPM.cmake for more info Set cache directory
# (default is ${CMAKE_BINARY_DIR}/cmake) set(CPM_SOURCE_CACHE
# "${CMAKE_BINARY_DIR}/.cpm_cache")

# You can also set a global cache location for all projects
set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/cpm")

# Force cache usage (will fail if package not in cache)
set(CPM_USE_LOCAL_PACKAGES ON)

# Or make it fallback to download if not in cache
set(CPM_DOWNLOAD_ALL ON)

set(CPPCHECK_ARGS "--check-level=exhaustive" "--suppress=returnDanglingLifetime"
                  "--suppress=internalAstError"
)
#set(USE_STATIC_ANALYZER "cppcheck;clang-tidy")

set(USE_CCACHE YES)

include(cmake/CPM.cmake)
include(cmake/tools.cmake)

# PackageProject.cmake will be used to make our target installable
CPMAddPackage("gh:TheLartians/PackageProject.cmake@1.13.0")

CPMAddPackage("gh:TheLartians/Ccache.cmake@1.2.5")
option(USE_CCACHE "use ccache" YES)

CPMAddPackage("gh:TheLartians/Format.cmake@1.8.3")

CPMAddPackage("gh:ericniebler/range-v3#0.12.0")

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  VERSION 1.15.3
  OPTIONS "SPDLOG_FMT_EXTERNAL 1" "SPDLOG_INSTALL YES" # create an installable
  # target
  SYSTEM YES
)

CPMAddPackage(
  NAME fmt
  GITHUB_REPOSITORY fmtlib/fmt
  GIT_TAG 9.1.0 # Use a specific version compatible with spdlog
  SYSTEM YES
)

CPMAddPackage(
  NAME daw
  GITHUB_REPOSITORY beached/daw_json_link
  VERSION 3.31.0
  SYSTEM YES
)

CPMAddPackage(
  NAME strong_type
  GITHUB_REPOSITORY rollbear/strong_type
  VERSION 15
  SYSTEM YES
)

# ---- Add source files ----

# Note: globbing sources is considered bad practice as CMake's generators may not
# detect new files automatically. Keep that in mind when changing files, or
# explicitly mention them here.
file(GLOB_RECURSE headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/[a-zA-Z]*.hpp"
)
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
)

# ---- Create library ----

# Note: for header-only libraries change all PUBLIC flags to INTERFACE and create an
# interface target: add_library(${PROJECT_NAME} INTERFACE)
add_library(${PROJECT_NAME} ${headers} ${sources})

# Set experimental flag to enable `import std` support from CMake. This must be
# enabled before C++ language support.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    # This specific value changes as experimental support evolves. See
    # `Help/dev/experimental.rst` in the CMake source corresponding to
    # your CMake build for the exact value to use.
    "0e5b6991-d74f-4b3d-a41c-cf096e0b2508"
)

# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(
  ${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->"
)

# Link dependencies

target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
target_link_libraries(${PROJECT_NAME} PUBLIC fmt::fmt)
target_link_libraries(${PROJECT_NAME} PUBLIC daw::daw-json-link)
target_link_libraries(${PROJECT_NAME} PUBLIC strong_type::strong_type)
target_link_libraries(${PROJECT_NAME} PUBLIC range-v3::range-v3)

# find_package(range-v3 REQUIRED) target_link_libraries(${PROJECT_NAME} PRIVATE
# range-v3::range-v3)
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake_local ${PROJECT_SOURCE_DIR}/cmake)

option(COIN_USE_CLP "Use Clp" ON)
option(COIN_USE_CBC "Use Cbc" OFF)
option(COIN_USE_CPX "Use Cplex" OFF)

set(COIN_ROOT_DIR
    "/opt/coinor"
    CACHE PATH "Coinor root directory"
)
include(cmake_local/Solver.cmake)

target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC "${COIN_INCLUDE_DIRS}")
target_link_libraries(
  ${PROJECT_NAME} PUBLIC "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
)

find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
target_link_libraries(
  ${PROJECT_NAME} PRIVATE Arrow::arrow_shared Parquet::parquet_shared
)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include/${PROJECT_NAME}-${PROJECT_VERSION}>
)

find_package(Boost REQUIRED COMPONENTS container serialization)
target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC ${Boost_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PUBLIC Boost::container Boost::serialization)

# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`.

# the location where the project's version header will be placed should match the
# project's regular header paths
string(TOLOWER ${PROJECT_NAME}/version.h VERSION_HEADER_LOCATION)

packageProject(
  NAME ${PROJECT_NAME}
  VERSION ${PROJECT_VERSION}
  NAMESPACE ${PROJECT_NAME}
  BINARY_DIR ${PROJECT_BINARY_DIR}
  INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include
  INCLUDE_DESTINATION include/${PROJECT_NAME}-${PROJECT_VERSION}
  VERSION_HEADER "${VERSION_HEADER_LOCATION}"
  COMPATIBILITY SameMajorVersion
  DEPENDENCIES "range-v3 0.12.0" "spdlog 1.12.0" "fmt 9.1.0" "strong_type 15"
               "daw 3.30.2"
)
