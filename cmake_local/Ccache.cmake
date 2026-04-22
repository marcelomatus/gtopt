#[=======================================================================[.rst:
Ccache
------

Detects and enables `ccache <https://ccache.dev/>`_ as the C/C++ compiler
launcher for the current CMake project.

Usage::

  option(USE_CCACHE "Enable ccache compiler caching" YES)
  include(cmake_local/Ccache.cmake)

or simply::

  include(cmake_local/Ccache.cmake)   # USE_CCACHE defaults to YES

Behaviour
^^^^^^^^^

* If ``USE_CCACHE`` is ``NO`` (or ``OFF``) nothing happens.
* If ``USE_CCACHE`` is ``YES`` (the default) and ``ccache`` is found on
  ``PATH``, the variables ``CMAKE_C_COMPILER_LAUNCHER`` and
  ``CMAKE_CXX_COMPILER_LAUNCHER`` are set to the ccache executable — but
  **only when the caller has not already set them**.  This means an
  externally supplied launcher (e.g. ``-DCMAKE_CXX_COMPILER_LAUNCHER=ccache``
  on the CI command line) is always honoured as-is.
* If ccache is not found, a status message is printed and the build
  continues without a launcher.

The native ``CMAKE_<LANG>_COMPILER_LAUNCHER`` mechanism is used instead of
the ``TheLartians/Ccache.cmake`` CPM package so that:

* No internet download is required.
* The module works in offline / air-gapped environments.
* No CPM dependency is introduced when this file is included from
  ``all/CMakeLists.txt`` (which manages its own CPM bootstrap).

.. note::
   ``CMAKE_<LANG>_COMPILER_LAUNCHER`` is baked into the build system at
   **configure time**.  Install ccache **before** running ``cmake``.  If
   ccache was absent when you last configured, delete the build directory
   and reconfigure.

#]=======================================================================]

include_guard(GLOBAL)

option(USE_CCACHE "Enable ccache compiler caching" YES)

if(NOT USE_CCACHE)
  return()
endif()

find_program(CCACHE_PROGRAM ccache)

if(CCACHE_PROGRAM)
  if(NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER
        "${CCACHE_PROGRAM}"
        CACHE STRING "C compiler launcher" FORCE
    )
  endif()
  if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${CCACHE_PROGRAM}"
        CACHE STRING "CXX compiler launcher" FORCE
    )
  endif()
  # Configure ccache for PCH + coverage cacheability.  Without these
  # settings ccache marks compilations that use precompiled headers or
  # coverage instrumentation as "uncacheable".
  #   pch_defines, time_macros: ignore __DATE__/__TIME__ differences in
  #     PCH and source files — required for TUs using the project PCH
  #     (cmake_local/PrecompiledHeaders.cmake).
  #   gcno_file_location: don't hash the .gcno path — required for
  #     -fprofile-arcs / -ftest-coverage (coverage-instrumented TUs).
  # Written to ccache.conf so settings persist at build time.  The
  # CCACHE_SLOPPINESS env var would override this file if set, so CI
  # workflows must NOT export CCACHE_SLOPPINESS — this conf is the single
  # source of truth (see ubuntu/coverage/profile workflows).
  set(_ccache_conf_dir "$ENV{HOME}/.config/ccache")
  set(_ccache_conf "${_ccache_conf_dir}/ccache.conf")
  set(_ccache_marker "${_ccache_conf}.gtopt_marker_v2")
  if(NOT EXISTS "${_ccache_marker}")
    file(MAKE_DIRECTORY "${_ccache_conf_dir}")
    # Strip any stanza previously written by an earlier gtopt CMake so we
    # can bump sloppiness cleanly (v1 wrote only pch_defines,time_macros).
    if(EXISTS "${_ccache_conf}")
      file(READ "${_ccache_conf}" _existing_conf)
      string(REGEX REPLACE
        "# Added by gtopt CMake[^\n]*\nsloppiness *=[^\n]*\npch_external_checksum *=[^\n]*\n"
        ""
        _cleaned_conf "${_existing_conf}")
      file(WRITE "${_ccache_conf}" "${_cleaned_conf}")
    endif()
    file(APPEND "${_ccache_conf}"
      "# Added by gtopt CMake for PCH + coverage cacheability\n"
      "sloppiness = gcno_file_location,pch_defines,time_macros\n"
      "pch_external_checksum = true\n"
    )
    if(EXISTS "${_ccache_conf}.gtopt_marker")
      file(REMOVE "${_ccache_conf}.gtopt_marker")
    endif()
    file(TOUCH "${_ccache_marker}")
    message(STATUS "ccache: wrote PCH + coverage sloppiness to ${_ccache_conf}")
  endif()
  message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
else()
  message(STATUS "USE_CCACHE=YES but ccache not found in PATH — building without cache")
endif()
