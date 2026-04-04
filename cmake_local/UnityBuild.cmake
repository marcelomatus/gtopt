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

The default batch size is 8.  Unity builds can be disabled globally by
setting ``-DGTOPT_UNITY_BUILD=OFF`` on the cmake command line.

#]=======================================================================]

option(GTOPT_UNITY_BUILD "Enable unity (jumbo) builds for faster compilation" ON)

function(target_enable_unity_build target)
  cmake_parse_arguments(ARG "" "BATCH_SIZE" "" ${ARGN})
  if(NOT ARG_BATCH_SIZE)
    set(ARG_BATCH_SIZE 8)
  endif()

  if(GTOPT_UNITY_BUILD)
    set_target_properties(${target} PROPERTIES
      UNITY_BUILD ON
      UNITY_BUILD_BATCH_SIZE ${ARG_BATCH_SIZE}
    )
    message(STATUS "Unity build enabled for ${target} (batch size ${ARG_BATCH_SIZE})")
  endif()
endfunction()
