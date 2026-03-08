#[=======================================================================[.rst:
Coverage
--------

Provides the ``target_enable_coverage()`` function for instrumenting C++
targets with gcov/lcov-compatible coverage flags.

Usage::

  include(cmake_local/Coverage.cmake)

  option(ENABLE_TEST_COVERAGE "Enable coverage instrumentation" OFF)

  if(ENABLE_TEST_COVERAGE)
    target_enable_coverage(<target>)           # PUBLIC flags (executable)
    target_enable_coverage(<target> PRIVATE)   # PRIVATE flags (library)
  endif()

Function synopsis
^^^^^^^^^^^^^^^^^

.. code-block:: cmake

   target_enable_coverage(<target> [PRIVATE])

Applies the following flags to *target*:

* **Compile options**: ``-O0 -g -fprofile-arcs -ftest-coverage -fprofile-update=atomic``
* **Link options**:    ``-fprofile-arcs -ftest-coverage``

The ``PRIVATE`` keyword selects the ``PRIVATE`` visibility scope (default is
``PUBLIC``).  Use ``PRIVATE`` for library targets to avoid leaking coverage
flags into consumer compile commands.

.. note::
   These flags are GCC/Clang-specific.  The function is a no-op for MSVC.

#]=======================================================================]

include_guard(GLOBAL)

function(target_enable_coverage target)
  cmake_parse_arguments(PARSE_ARGV 1 _tec "PRIVATE" "" "")
  if(_tec_PRIVATE)
    set(_scope PRIVATE)
  else()
    set(_scope PUBLIC)
  endif()

  target_compile_options(
    ${target} ${_scope}
    -O0
    -g
    -fprofile-arcs
    -ftest-coverage
    -fprofile-update=atomic
  )
  target_link_options(${target} ${_scope} -fprofile-arcs -ftest-coverage)
endfunction()
