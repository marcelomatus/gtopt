#[=======================================================================[.rst:
FindSCIP
--------

Find the SCIP constraint-integer-programming / MIP solver (``libscip``).

This is the FALLBACK module used when SCIP's own CMake config
(``find_package(SCIP CONFIG)``) is unavailable or fails (e.g. its
``find_dependency(GMP)`` / ``find_dependency(MPFR)`` can't locate the GMP/MPFR
*development headers*, which are not required to LINK against ``libscip.so`` —
its runtime deps are resolved by the dynamic linker).  It links ``libscip.so``
directly via its public C API headers (``scip/scip.h``), which do NOT include
``gmp.h``.

Search order for the include dir and library:

1. ``SCIP_ROOT_DIR`` CMake variable
2. ``$ENV{SCIP_ROOT_DIR}`` / ``$ENV{SCIPOPTDIR}``
3. Common install prefixes (system, ``/usr/local``, ``/opt/scip``).

Result variables::

  ``SCIP_FOUND``        — TRUE if SCIP was found
  ``SCIP_INCLUDE_DIRS`` — Header include directory (contains scip/)
  ``SCIP_LIBRARIES``    — Libraries to link against (libscip.so)
  ``SCIP_VERSION``      — Version string when discoverable

Disable explicitly with ``-DGTOPT_DISABLE_SCIP=ON``.

#]=======================================================================]

include_guard(GLOBAL)

if(GTOPT_DISABLE_SCIP)
  set(SCIP_FOUND FALSE)
  return()
endif()

set(_SCIP_SEARCH_PATHS)
if(SCIP_ROOT_DIR)
  list(APPEND _SCIP_SEARCH_PATHS "${SCIP_ROOT_DIR}")
endif()
if(DEFINED ENV{SCIP_ROOT_DIR})
  list(APPEND _SCIP_SEARCH_PATHS "$ENV{SCIP_ROOT_DIR}")
endif()
if(DEFINED ENV{SCIPOPTDIR})
  list(APPEND _SCIP_SEARCH_PATHS "$ENV{SCIPOPTDIR}")
endif()
list(APPEND _SCIP_SEARCH_PATHS "/usr" "/usr/local" "/opt/scip")

find_path(SCIP_INCLUDE_DIR
  NAMES scip/scip.h
  HINTS ${_SCIP_SEARCH_PATHS}
  PATH_SUFFIXES include
)

find_library(SCIP_LIBRARY
  NAMES scip
  HINTS ${_SCIP_SEARCH_PATHS}
  PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu
)

# Best-effort version string from scip/def.h.
set(SCIP_VERSION "")
if(SCIP_INCLUDE_DIR AND EXISTS "${SCIP_INCLUDE_DIR}/scip/def.h")
  file(STRINGS "${SCIP_INCLUDE_DIR}/scip/def.h" _scip_ver_major
       REGEX "^#define[ \t]+SCIP_VERSION_MAJOR[ \t]+[0-9]+")
  file(STRINGS "${SCIP_INCLUDE_DIR}/scip/def.h" _scip_ver_minor
       REGEX "^#define[ \t]+SCIP_VERSION_MINOR[ \t]+[0-9]+")
  file(STRINGS "${SCIP_INCLUDE_DIR}/scip/def.h" _scip_ver_patch
       REGEX "^#define[ \t]+SCIP_VERSION_PATCH[ \t]+[0-9]+")
  string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _scip_vmaj "${_scip_ver_major}")
  string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _scip_vmin "${_scip_ver_minor}")
  string(REGEX REPLACE ".*[ \t]([0-9]+).*" "\\1" _scip_vpat "${_scip_ver_patch}")
  if(_scip_vmaj AND _scip_vmin)
    set(SCIP_VERSION "${_scip_vmaj}.${_scip_vmin}.${_scip_vpat}")
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SCIP
  REQUIRED_VARS SCIP_LIBRARY SCIP_INCLUDE_DIR
  VERSION_VAR   SCIP_VERSION
)

if(SCIP_FOUND)
  set(SCIP_INCLUDE_DIRS "${SCIP_INCLUDE_DIR}")
  set(SCIP_LIBRARIES    "${SCIP_LIBRARY}")
  mark_as_advanced(SCIP_INCLUDE_DIR SCIP_LIBRARY)
endif()
