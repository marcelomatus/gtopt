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
include(FastLinker)
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

include(ClangFormat)

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
# NOTE: the install RPATH (incl. `$ORIGIN/../lib` so bin/gtopt resolves the
# shipped libspdlog.so.1.15 in <prefix>/lib) is set by configure_install_rpath()
# below — do NOT set INSTALL_RPATH here, it would be overwritten.

target_link_libraries(
  ${PROJECT_NAME}
  gtopt::gtopt
)

# ---- jemalloc (optional) ----
# Replace glibc malloc with jemalloc on multi-threaded SDDP runs.  IMPORTANT:
# simply *linking* jemalloc is not enough — with its stock defaults
# (background_thread:false, dirty_decay_ms:10000) peak RSS stays at glibc-like
# levels (measured 35.6 GB on the 2-year Maule/Laja CPLEX parity case, vs
# ~31 GB for glibc).  The win comes from `background_thread:true`, which
# main.cpp bakes into the binary via the `malloc_conf` symbol (guarded by the
# GTOPT_HAVE_JEMALLOC define set below): a dedicated purge thread returns pages
# to the OS off the hot path — measured peak RSS 35.6 -> 25.5 GB (-28%) and
# wall 19:47 -> 16:43 (-15%) on that case.  Narenas/percpu/decay tuning added
# nothing.  Override at runtime with the MALLOC_CONF env var.
# Opt-in via -DGTOPT_USE_JEMALLOC=ON (default ON when jemalloc is found).
option(GTOPT_USE_JEMALLOC "Link jemalloc as the malloc replacement" ON)
if(GTOPT_USE_JEMALLOC)
  # Prefer the `-dev` package (libjemalloc.so symlink).  Fall back to the
  # runtime SONAME `libjemalloc.so.2` so that systems which only ship the
  # runtime (e.g. distroless containers, or boxes where libjemalloc-dev
  # was not installed) still get the benefit.
  find_library(JEMALLOC_LIB NAMES jemalloc jemalloc.so.2 libjemalloc.so.2)
  if(JEMALLOC_LIB)
    target_link_libraries(${PROJECT_NAME} ${JEMALLOC_LIB})
    # Enables the `malloc_conf = "background_thread:true"` symbol in main.cpp.
    target_compile_definitions(${PROJECT_NAME} PRIVATE GTOPT_HAVE_JEMALLOC=1)
    message(STATUS "gtopt: linking jemalloc: ${JEMALLOC_LIB} "
                   "(malloc_conf=background_thread:true)")
  else()
    message(WARNING
      "gtopt: jemalloc not found (install libjemalloc-dev); "
      "falling back to glibc malloc — expect ~2x higher peak RSS on "
      "multi-threaded SDDP runs due to per-thread arena fragmentation")
  endif()
endif()

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

# ---- Plugin build dependency ----
# Ensure solver plugins are built alongside the standalone binary so that
# the binary can discover them at runtime (in the build tree or after install).
foreach(plugin IN ITEMS osi cplex highs)
  if(TARGET gtopt_solver_${plugin})
    add_dependencies(${PROJECT_NAME} gtopt_solver_${plugin})
  endif()
endforeach()
