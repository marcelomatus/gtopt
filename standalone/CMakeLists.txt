cmake_minimum_required(VERSION 3.31.6)

project(gtoptStandalone LANGUAGES CXX)

# ---- Shared cmake module path ----
list(APPEND CMAKE_MODULE_PATH
     "${CMAKE_CURRENT_LIST_DIR}/../cmake_local"
     "${CMAKE_CURRENT_LIST_DIR}/../cmake"
)

# ---- Build defaults ----
include(DefaultInstallPrefix)
include(BuildTypeDefault)
include(CxxStandard)
include(Ccache)

# ---- Options ----
option(ENABLE_TEST_COVERAGE "Enable coverage instrumentation for the gtopt binary" OFF)

# ---- Compiler checks and tools ----
include(CompilerCheck)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/tools.cmake")

# ---- Dependencies ----
option(CPM_USE_LOCAL_PACKAGES "Try find_package first" ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake")

if(NOT TARGET gtopt::gtopt)
  CPMAddPackage(NAME gtopt SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ClangFormat.cmake")

# ---- RPATH configuration ----
# INSTALL_RPATH_USE_LINK_PATH adds all linker search paths to the installed
# binary's RPATH so that non-system shared libraries (Arrow, Parquet, …)
# are found at runtime after installation.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# ---- Executable ----
file(GLOB sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/source/[a-zA-Z]*.cpp"
)

add_executable(${PROJECT_NAME} ${sources})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "gtopt")

target_link_libraries(
  ${PROJECT_NAME}
  gtopt::gtopt
)

# ---- Coverage ----
include(Coverage)
if(ENABLE_TEST_COVERAGE)
  target_enable_coverage(${PROJECT_NAME})
endif()

# ---- RPATH for installed binary ----
include(InstallRpath)
configure_install_rpath(${PROJECT_NAME} gtopt)

# ---- Install ----
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
