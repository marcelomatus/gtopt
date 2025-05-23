cmake_minimum_required(VERSION 3.22)

set(CMAKE_CONFIGURATION_TYPES
    "Debug"
    CACHE STRING "" FORCE
)

project(gtoptStandalone LANGUAGES CXX)

# --- Options ----

option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first" ON)

cmake_policy(SET CMP0167 NEW)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(NAME gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Boost REQUIRED COMPONENTS program_options)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

# ---- Create standalone executable ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)

add_executable(${PROJECT_NAME} ${sources})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "gtopt")
# target_compile_options("gtopt" PUBLIC -O0 -g)

target_link_libraries(${PROJECT_NAME} gtopt::gtopt Boost::program_options)
