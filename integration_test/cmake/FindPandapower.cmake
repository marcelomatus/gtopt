#[=======================================================================[.rst:
FindPandapower
--------------

Locates the ``gtopt_compare`` tool and/or a Python interpreter with
``pandapower`` installed, for use in pandapower comparison tests.

Include this module from ``integration_test/CMakeLists.txt``::

  include(${CMAKE_SCRIPTS_DIR}/FindPandapower.cmake)

After inclusion the following cache variables are set (each may already
have been set by the user via ``-D``):

``COMPARE_PANDAPOWER_PROGRAM``
  Path to the installed ``gtopt-compare`` entry-point (from
  ``pip install -e scripts/``).

``COMPARE_PANDAPOWER_PYTHON``
  Python interpreter used to run ``gtopt_compare`` as a module directly
  from the source tree (``python -m gtopt_compare …``).

``COMPARE_PANDAPOWER_SCRIPTS_DIR``
  Path to the ``scripts/`` source directory that contains the
  ``gtopt_compare`` package (set automatically alongside
  ``COMPARE_PANDAPOWER_PYTHON``).

``PANDAPOWER_PYTHON``
  Fallback Python interpreter that has ``pandapower`` installed but no
  source-tree ``gtopt_compare`` package.

Detection priority (first match wins)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

1. ``-DCOMPARE_PANDAPOWER_PROGRAM=…`` – user-supplied installed binary.
2. Source-tree module: ``scripts/gtopt_compare/__init__.py`` exists and
   ``pandapower`` is importable.  Sets ``COMPARE_PANDAPOWER_PYTHON`` and
   ``COMPARE_PANDAPOWER_SCRIPTS_DIR``.
3. ``find_program(gtopt-compare)`` – installed binary found on PATH.
4. Fallback: any Python3 interpreter with ``pandapower`` importable.

#]=======================================================================]

# ---- Cache variables (user-overridable) ----

set(COMPARE_PANDAPOWER_PROGRAM "" CACHE FILEPATH
  "Path to the gtopt_compare command (from 'pip install -e scripts/')"
)
set(COMPARE_PANDAPOWER_PYTHON "" CACHE FILEPATH
  "Python interpreter used to run gtopt_compare from the source tree \
(set automatically when scripts/gtopt_compare is found; \
override with -DCOMPARE_PANDAPOWER_PYTHON=/path/to/python3)"
)
set(COMPARE_PANDAPOWER_SCRIPTS_DIR "" CACHE PATH
  "Path to the scripts/ source directory containing the gtopt_compare \
package (set automatically; override with \
-DCOMPARE_PANDAPOWER_SCRIPTS_DIR=/path/to/scripts)"
)

# Locate the scripts/ source directory (one level above integration_test/).
cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH _pp_integration_dir)
cmake_path(GET _pp_integration_dir PARENT_PATH _pp_repo_root)
cmake_path(APPEND _pp_repo_root scripts OUTPUT_VARIABLE _pp_scripts_src_dir)

# ---- Priority 2: source-tree gtopt_compare module ----
if(NOT COMPARE_PANDAPOWER_PROGRAM AND NOT COMPARE_PANDAPOWER_PYTHON)
  if(EXISTS "${_pp_scripts_src_dir}/gtopt_compare/__init__.py")
    if(NOT COMPARE_PANDAPOWER_PYTHON)
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_FOUND)
        execute_process(
          COMMAND "${Python3_EXECUTABLE}" -c "import pandapower"
          RESULT_VARIABLE _pp_import_result
          OUTPUT_QUIET
          ERROR_QUIET
        )
        if(_pp_import_result EQUAL 0)
          # Resolve real interpreter path (avoids pyenv/shim issues).
          execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c
              "import sys; print(sys.executable, end='')"
            OUTPUT_VARIABLE _pp_real_exec
            OUTPUT_STRIP_TRAILING_WHITESPACE
          )
          set(COMPARE_PANDAPOWER_PYTHON "${_pp_real_exec}")
          set(COMPARE_PANDAPOWER_SCRIPTS_DIR "${_pp_scripts_src_dir}")
          message(STATUS "Found gtopt_compare in source tree: ${_pp_scripts_src_dir}")
        else()
          message(STATUS
            "gtopt_compare source found but pandapower not installed — "
            "install pandapower or use 'pip install -e scripts/' to enable "
            "pandapower comparison tests."
          )
        endif()
      endif()
    else()
      # User supplied COMPARE_PANDAPOWER_PYTHON; auto-set the scripts dir.
      if(NOT COMPARE_PANDAPOWER_SCRIPTS_DIR)
        set(COMPARE_PANDAPOWER_SCRIPTS_DIR "${_pp_scripts_src_dir}")
      endif()
      message(STATUS
        "Using gtopt_compare from source tree: ${COMPARE_PANDAPOWER_SCRIPTS_DIR}"
      )
    endif()
  endif()
endif()

# ---- Priority 3: installed gtopt-compare binary ----
if(NOT COMPARE_PANDAPOWER_PROGRAM AND NOT COMPARE_PANDAPOWER_PYTHON)
  find_program(_pp_found_binary gtopt-compare)
  if(_pp_found_binary)
    set(COMPARE_PANDAPOWER_PROGRAM "${_pp_found_binary}")
    message(STATUS "Found gtopt-compare command: ${COMPARE_PANDAPOWER_PROGRAM}")
  endif()
  unset(_pp_found_binary)
endif()

# ---- Priority 4: any Python3 with pandapower ----
if(NOT COMPARE_PANDAPOWER_PROGRAM AND NOT COMPARE_PANDAPOWER_PYTHON)
  set(PANDAPOWER_PYTHON "" CACHE FILEPATH
    "Python interpreter with pandapower (used when gtopt_compare is not installed)"
  )
  if(NOT PANDAPOWER_PYTHON)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c "import pandapower"
        RESULT_VARIABLE _pp_import_result
        OUTPUT_QUIET
        ERROR_QUIET
      )
      if(_pp_import_result EQUAL 0)
        execute_process(
          COMMAND "${Python3_EXECUTABLE}" -c
            "import sys; print(sys.executable, end='')"
          OUTPUT_VARIABLE _pp_real_exec
          OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(PANDAPOWER_PYTHON "${_pp_real_exec}")
        message(STATUS "Found Python with pandapower: ${PANDAPOWER_PYTHON}")
      else()
        message(STATUS
          "Python3 found but pandapower is not installed — "
          "skipping pandapower comparison tests. "
          "Install scripts with 'pip install -e scripts/' (preferred) or "
          "set -DPANDAPOWER_PYTHON=/path/to/python3 with pandapower available."
        )
      endif()
    endif()
  endif()
endif()

unset(_pp_integration_dir)
unset(_pp_repo_root)
unset(_pp_scripts_src_dir)
unset(_pp_import_result)
unset(_pp_real_exec)
