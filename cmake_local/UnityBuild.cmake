#[=======================================================================[.rst:
UnityBuild
----------

Enables CMake unity (jumbo) builds for faster compilation.

Unity builds combine multiple ``.cpp`` files into batches that are compiled
as a single translation unit, dramatically reducing redundant header parsing
and template instantiation.

Usage::

  include(UnityBuild)
  target_enable_unity_build(<target> [BATCH_SIZE <n>])

When no BATCH_SIZE is given, the batch size is computed automatically from
the number of CPU cores so that there are enough batches to keep all cores
busy during parallel builds.

Unity builds can be disabled globally by setting
``-DGTOPT_UNITY_BUILD=OFF`` on the cmake command line.

#]=======================================================================]

option(GTOPT_UNITY_BUILD "Enable unity (jumbo) builds for faster compilation" ON)

# Detect available CPU cores once
include(ProcessorCount)
ProcessorCount(GTOPT_NPROC)
if(GTOPT_NPROC EQUAL 0)
  set(GTOPT_NPROC 4)
endif()

function(target_enable_unity_build target)
  cmake_parse_arguments(ARG "" "BATCH_SIZE" "" ${ARGN})

  if(NOT GTOPT_UNITY_BUILD)
    return()
  endif()

  if(NOT ARG_BATCH_SIZE)
    # Compute batch size: enough batches to keep all cores busy.
    # Get the number of source files in the target.
    get_target_property(_sources ${target} SOURCES)
    list(LENGTH _sources _n_sources)
    if(_n_sources GREATER 0 AND GTOPT_NPROC GREATER 0)
      math(EXPR ARG_BATCH_SIZE "${_n_sources} / ${GTOPT_NPROC}")
    else()
      set(ARG_BATCH_SIZE 4)
    endif()
    # Clamp: at least 2 (otherwise unity is pointless)
    if(ARG_BATCH_SIZE LESS 2)
      set(ARG_BATCH_SIZE 2)
    endif()
  endif()

  set_target_properties(${target} PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE ${ARG_BATCH_SIZE}
  )
  message(STATUS "Unity build enabled for ${target} (batch size ${ARG_BATCH_SIZE}, ${GTOPT_NPROC} cores)")
endfunction()
