cmake_minimum_required(VERSION 3.22)

project(gtopt LANGUAGES CXX)

# --- Options ----

option(CPM_USE_LOCAL_PACKAGES "Try `find_package` first"  ON)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

CPMAddPackage(NAME Gtopt SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Boost REQUIRED COMPONENTS program_options)
include_directories(SYSTEM ${Boost_INCLUDE_DIRS})

# ---- Create standalone executable ----

file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)

add_executable(${PROJECT_NAME} ${sources})

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23 OUTPUT_NAME "Gtopt")

target_link_libraries(${PROJECT_NAME} Gtopt::Gtopt cxxopts Boost::program_options)
