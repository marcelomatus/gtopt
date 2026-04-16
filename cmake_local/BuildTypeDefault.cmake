#[=======================================================================[.rst:
BuildTypeDefault
----------------

Sets ``CMAKE_BUILD_TYPE`` to ``Debug`` when no build type has been
provided on the command line and the generator does not support multiple
configurations (i.e. all single-config generators such as Makefiles and
Ninja).

Also defines the custom ``CIFast`` build type — a fast-compile / fast-link
configuration intended for CI and for agent-driven local iteration where
the only needed signal is "does it compile + do the tests pass?".

  * ``CIFast``  — ``-O0 -g1`` (+ ``-fno-standalone-debug`` on Clang),
                  ``-ffunction-sections -fdata-sections`` +
                  ``-Wl,--gc-sections``.  No optimisation, minimal line-only
                  debug info (backtraces still point at a source line), small
                  binaries.  Use this for every-commit CI and for the default
                  sandbox build.  When deeper debugging is needed, reconfigure
                  with ``-DCMAKE_BUILD_TYPE=Debug``.
  * ``Debug``   — full symbols, unchanged.
  * ``Release`` / ``RelWithDebInfo`` / ``MinSizeRel`` — CMake defaults.

Usage::

  include(cmake_local/BuildTypeDefault.cmake)

This module is a no-op for multi-config generators (Visual Studio, Xcode,
Ninja Multi-Config) where ``CMAKE_CONFIGURATION_TYPES`` is set by the
generator and ``CMAKE_BUILD_TYPE`` is ignored.

Including this module in every sub-project ``CMakeLists.txt`` that can be
built stand-alone ensures a predictable default when a developer runs
``cmake -S <subdir> -B build`` without specifying ``-DCMAKE_BUILD_TYPE``.

When the sub-project is consumed via ``add_subdirectory()`` from a
parent (e.g. ``all/CMakeLists.txt``), the parent has already set
``CMAKE_BUILD_TYPE`` in the cache, so the guard
``if(NOT CMAKE_BUILD_TYPE ...)`` is false and this module is a no-op.

#]=======================================================================]

include_guard(GLOBAL)

# ---- CIFast build type ------------------------------------------------------
# Define the flag sets unconditionally so CMAKE_BUILD_TYPE=CIFast works on
# every configure path (all/, root, test/, standalone/, integration_test/).
# These are cache variables, so a user can override them from the command
# line if needed.

set(_gtopt_cifast_cxx_extra "")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Clang-only: skip emitting type info for inline functions.  Meaningful
  # size/time savings for heavy C++26 template instantiations.
  set(_gtopt_cifast_cxx_extra " -fno-standalone-debug")
endif()

# FORCE is required: enable_language(CXX) inside project() pre-creates
# CMAKE_CXX_FLAGS_<CONFIG> cache entries as empty strings (from the missing
# _INIT variables) for whatever CMAKE_BUILD_TYPE is active.  Without FORCE,
# our set(... CACHE ...) is then a no-op and the CIFast flags disappear.
set(CMAKE_C_FLAGS_CIFAST
    "-O0 -g1 -ffunction-sections -fdata-sections"
    CACHE STRING "C flags for the CIFast build (fast compile, backtrace-only debug info)"
    FORCE
)
set(CMAKE_CXX_FLAGS_CIFAST
    "-O0 -g1 -ffunction-sections -fdata-sections${_gtopt_cifast_cxx_extra}"
    CACHE STRING "C++ flags for the CIFast build (fast compile, backtrace-only debug info)"
    FORCE
)
set(CMAKE_EXE_LINKER_FLAGS_CIFAST
    "-Wl,--gc-sections"
    CACHE STRING "Exe linker flags for the CIFast build"
    FORCE
)
set(CMAKE_SHARED_LINKER_FLAGS_CIFAST
    "-Wl,--gc-sections"
    CACHE STRING "Shared linker flags for the CIFast build"
    FORCE
)
set(CMAKE_MODULE_LINKER_FLAGS_CIFAST
    "-Wl,--gc-sections"
    CACHE STRING "Module linker flags for the CIFast build"
    FORCE
)
mark_as_advanced(
  CMAKE_C_FLAGS_CIFAST
  CMAKE_CXX_FLAGS_CIFAST
  CMAKE_EXE_LINKER_FLAGS_CIFAST
  CMAKE_SHARED_LINKER_FLAGS_CIFAST
  CMAKE_MODULE_LINKER_FLAGS_CIFAST
)
unset(_gtopt_cifast_cxx_extra)

# ---- Default build type -----------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel, CIFast)" FORCE
  )
  message(STATUS "Build type not set — defaulting to Debug")
endif()

# Always refresh the drop-down list so CIFast shows up even when
# CMAKE_BUILD_TYPE was provided on the command line.
if(NOT CMAKE_CONFIGURATION_TYPES)
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel CIFast
  )
endif()
