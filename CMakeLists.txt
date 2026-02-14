cmake_minimum_required(VERSION 3.31.6)

# ---- Project ----

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

set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/cpm")
set(CPM_USE_LOCAL_PACKAGES ON)
set(CPM_DOWNLOAD_ALL ON)

set(CPPCHECK_ARGS "--check-level=exhaustive" "--suppress=returnDanglingLifetime"
                  "--suppress=internalAstError"
)

option(USE_CCACHE "use ccache" YES)

include(cmake/CPM.cmake)
include(cmake/tools.cmake)

# PackageProject.cmake will be used to make our target installable
CPMAddPackage("gh:TheLartians/PackageProject.cmake@1.13.0")

CPMAddPackage("gh:TheLartians/Format.cmake@1.8.3")

# External libraries

CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  VERSION 1.17.0
  OPTIONS "SPDLOG_USE_STD_FORMAT 1" "SPDLOG_INSTALL YES" # create an installable
  # target
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

# ---- Solver configuration ----

set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake_local ${PROJECT_SOURCE_DIR}/cmake)

set(COIN_ROOT_DIR
    "/opt/coinor"
    CACHE PATH "Coinor root directory"
)
# Solver back-end: AUTO (default), CLP, CBC, or CPX (CPLEX)
set(COIN_SOLVER "AUTO" CACHE STRING "COIN-OR solver back-end")
include(cmake_local/Solver.cmake)

# ---- External packages ----

find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(Boost REQUIRED COMPONENTS container)



# ---- Add source files ----

file(GLOB_RECURSE headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/include/[a-zA-Z]*.hpp"
)
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
)

# ---- Create library ----

add_library(${PROJECT_NAME} ${headers} ${sources})

# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(
  ${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->"
)

target_include_directories(
  ${PROJECT_NAME}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include/${PROJECT_NAME}-${PROJECT_VERSION}>
)

target_include_directories(
  ${PROJECT_NAME} SYSTEM
  PUBLIC "${COIN_INCLUDE_DIRS}" ${Boost_INCLUDE_DIRS}
)

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC spdlog::spdlog
         daw::daw-json-link
         strong_type::strong_type
         "${COIN_OSI_LIBRARIES};${SOLVER_LIBRARIES}"
         Arrow::arrow_shared
         Parquet::parquet_shared
         Boost::container
)

# enable compiler warnings
include(cmake/CompilerWarnings.cmake)
target_set_warnings(${PROJECT_NAME})

# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`.
# Skip when included as a subproject (e.g. from standalone) to avoid installing
# headers and cmake package files that are not needed.

if(PROJECT_IS_TOP_LEVEL)
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
    DEPENDENCIES "spdlog 1.17.0" "strong_type 15" "daw 3.31.0"
  )
endif()
