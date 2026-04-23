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

When no BATCH_SIZE is given, the batch size is computed automatically
**per source directory** so that each directory produces enough batches
to keep all cores busy.  Files in different directories are never mixed
in the same unity batch (via ``UNITY_GROUP``).

Unity builds can be disabled globally by setting
``-DGTOPT_UNITY_BUILD=OFF`` on the cmake command line.

Priority scheduling
^^^^^^^^^^^^^^^^^^^

Files marked with ``SKIP_UNITY_BUILD_INCLUSION ON`` compile as standalone
translation units alongside the batches.  To minimise critical-path build
time, callers should ``list(PREPEND)`` the slowest standalone files to
the front of the source list **before** creating the target — Ninja uses
source-list order as a tiebreaker when multiple edges are ready at the
same graph depth.

Requires ``CMAKE_CXX_SCAN_FOR_MODULES OFF`` (set by ``CxxStandard.cmake``)
so that the Ninja generator produces unity batch files instead of
individual per-file compilations.

#]=======================================================================]

option(GTOPT_UNITY_BUILD "Enable unity (jumbo) builds for faster compilation" OFF)

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

  get_target_property(_all_sources ${target} SOURCES)

  # Filter to only compilable sources (.cpp/.cxx/.cc) — headers don't
  # participate in unity batching but skew the file count.
  set(_sources)
  foreach(_src IN LISTS _all_sources)
    get_filename_component(_ext "${_src}" LAST_EXT)
    if(_ext MATCHES "^\\.(cpp|cxx|cc|c)$")
      list(APPEND _sources "${_src}")
    endif()
  endforeach()
  list(LENGTH _sources _n_sources)

  # ── Per-directory grouping and batch-size computation ──
  # Always assign UNITY_GROUP per directory so files from different
  # directories are never mixed in the same unity source (even when an
  # explicit BATCH_SIZE is provided).
  # 1. Discover unique directories among the target's sources.
  set(_dirs)
  foreach(_src IN LISTS _sources)
    get_filename_component(_dir "${_src}" DIRECTORY)
    # Normalise to a short label so the log is readable.
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_dir}")
    if("${_rel}" STREQUAL "")
      set(_rel ".")
    endif()
    list(APPEND _dirs "${_rel}")
  endforeach()
  list(REMOVE_DUPLICATES _dirs)

  # 2. For each directory, count files and assign UNITY_GROUP.
  set(_max_batch 2)
  foreach(_dir IN LISTS _dirs)
    # Collect sources in this directory.
    set(_dir_sources)
    foreach(_src IN LISTS _sources)
      get_filename_component(_abs_dir "${_src}" DIRECTORY)
      file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_abs_dir}")
      if("${_rel}" STREQUAL "")
        set(_rel ".")
      endif()
      if("${_rel}" STREQUAL "${_dir}")
        list(APPEND _dir_sources "${_src}")
      endif()
    endforeach()

    list(LENGTH _dir_sources _n_dir)
    if(_n_dir EQUAL 0)
      continue()
    endif()

    if(ARG_BATCH_SIZE)
      # Explicit batch size — use as-is but clamp to [1, n_dir].
      set(_batch ${ARG_BATCH_SIZE})
      if(_batch LESS 1)
        set(_batch 1)
      endif()
    else()
      # Batch size: enough batches to saturate cores, capped at 8.
      math(EXPR _batch "(${_n_dir} + ${GTOPT_NPROC} - 1) / ${GTOPT_NPROC}")
      if(_batch LESS 2)
        set(_batch 2)
      endif()
      if(_batch GREATER 8)
        set(_batch 8)
      endif()
    endif()
    if(_batch GREATER _n_dir)
      set(_batch ${_n_dir})
    endif()

    # Track the maximum batch size across directories — CMake uses a single
    # UNITY_BUILD_BATCH_SIZE per target, so we use the largest one and rely
    # on UNITY_GROUP to keep directories separate.
    if(_batch GREATER _max_batch)
      set(_max_batch ${_batch})
    endif()

    # Assign a per-directory unity group so files from different
    # directories are never mixed in the same unity source.
    string(REPLACE "/" "_" _group "dir_${_dir}")
    foreach(_src IN LISTS _dir_sources)
      set_source_files_properties("${_src}" PROPERTIES UNITY_GROUP "${_group}")
    endforeach()

    message(STATUS "  unity group '${_dir}': ${_n_dir} files, batch ${_batch}")
  endforeach()

  set(_batch ${_max_batch})

  set_target_properties(${target} PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE ${_batch}
  )

  # ── Log SKIP_UNITY priority sources ──
  # Files with SKIP_UNITY_BUILD_INCLUSION compile as standalone TUs.
  # Combined with list(PREPEND) in the calling CMakeLists.txt, they appear
  # first in the source list — Ninja uses this as a tiebreaker when multiple
  # edges are ready at the same depth.
  get_target_property(_all_srcs ${target} SOURCES)
  set(_skip_srcs)
  foreach(_src IN LISTS _all_srcs)
    get_filename_component(_ext "${_src}" LAST_EXT)
    if(NOT _ext MATCHES "^\\.(cpp|cxx|cc|c)$")
      continue()
    endif()
    get_source_file_property(_skip "${_src}" SKIP_UNITY_BUILD_INCLUSION)
    if(_skip)
      list(APPEND _skip_srcs "${_src}")
    endif()
  endforeach()
  if(_skip_srcs)
    list(LENGTH _skip_srcs _n_skip)
    message(STATUS "  priority sources (${_n_skip}, SKIP_UNITY): ${_skip_srcs}")
  endif()

  message(STATUS "Unity build enabled for ${target} (batch size ${_batch}, ${GTOPT_NPROC} cores)")
endfunction()
