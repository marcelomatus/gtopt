#[=======================================================================[.rst:
TestParallelism
---------------

Configures CTest to run tests in parallel, using all available CPU cores.

Usage::

  include(cmake_local/TestParallelism.cmake)

Effect
^^^^^^

* Appends ``--parallel <N>`` to ``CMAKE_CTEST_ARGUMENTS``, where ``N``
  is the number of logical processors reported by CMake's
  ``ProcessorCount`` module (minimum 1).

This module is safe to include multiple times — ``include_guard(GLOBAL)``
ensures the ``CMAKE_CTEST_ARGUMENTS`` entry is only appended once.

#]=======================================================================]

include(ProcessorCount)
ProcessorCount(_test_parallelism_nproc)
if(_test_parallelism_nproc EQUAL 0)
  set(_test_parallelism_nproc 1)
endif()
if(NOT "--parallel" IN_LIST CMAKE_CTEST_ARGUMENTS)
  list(APPEND CMAKE_CTEST_ARGUMENTS "--parallel" "${_test_parallelism_nproc}")
endif()
unset(_test_parallelism_nproc)
