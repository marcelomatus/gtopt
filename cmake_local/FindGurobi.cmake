#[=======================================================================[.rst:
FindGurobi
----------

Find the Gurobi optimization solver.

Searches for the Gurobi C API header (``gurobi_c.h``) and shared library
(``libgurobi<ver>.so``, e.g. ``libgurobi130.so``).

Search order:

1. ``GUROBI_HOME`` environment variable
2. ``GUROBI_ROOT_DIR`` CMake variable
3. Common install locations:
   - ``/opt/gurobi<version>/linux64/``
   - ``$HOME/opt/gurobi<version>/linux64/``
   - ``$HOME/gurobi<version>/linux64/``

Result variables:

  ``GUROBI_FOUND``        — TRUE if Gurobi was found
  ``GUROBI_INCLUDE_DIRS`` — Header include directory
  ``GUROBI_LIBRARIES``    — Libraries to link against

#]=======================================================================]

# Build list of candidate paths
set(_GUROBI_SEARCH_PATHS)

if(DEFINED ENV{GUROBI_HOME})
  list(APPEND _GUROBI_SEARCH_PATHS "$ENV{GUROBI_HOME}")
endif()

if(GUROBI_ROOT_DIR)
  list(APPEND _GUROBI_SEARCH_PATHS "${GUROBI_ROOT_DIR}")
endif()

# Scan versioned directories under common prefixes.
# Gurobi ships as e.g. ``gurobi1301/linux64`` (no extra version level).
foreach(_prefix
    "/opt"
    "$ENV{HOME}/opt"
    "$ENV{HOME}")
  if(IS_DIRECTORY "${_prefix}")
    file(GLOB _versions "${_prefix}/gurobi*")
    foreach(_vdir ${_versions})
      if(IS_DIRECTORY "${_vdir}/linux64")
        list(APPEND _GUROBI_SEARCH_PATHS "${_vdir}/linux64")
      endif()
    endforeach()
  endif()
endforeach()

find_path(GUROBI_INCLUDE_DIR
  NAMES gurobi_c.h
  HINTS ${_GUROBI_SEARCH_PATHS}
  PATH_SUFFIXES include
)

# Gurobi libraries are versioned (libgurobi130.so, libgurobi120.so, etc.).
# Prefer newer versions first; fall back to the unversioned symlink if
# present.
find_library(GUROBI_LIBRARY
  NAMES gurobi140 gurobi130 gurobi120 gurobi110 gurobi100 gurobi
  HINTS ${_GUROBI_SEARCH_PATHS}
  PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Gurobi
  REQUIRED_VARS GUROBI_LIBRARY GUROBI_INCLUDE_DIR
)

if(GUROBI_FOUND)
  set(GUROBI_INCLUDE_DIRS "${GUROBI_INCLUDE_DIR}")
  set(GUROBI_LIBRARIES    "${GUROBI_LIBRARY}")
  mark_as_advanced(GUROBI_INCLUDE_DIR GUROBI_LIBRARY)
endif()
