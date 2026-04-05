#[=======================================================================[.rst:
FindMindOpt
-----------

Find the MindOpt optimization solver (Alibaba DAMO Academy).

Searches for the MindOpt C API header (``Mindopt.h``) and shared library
(``libmindopt.so``).

Search order:

1. ``MINDOPT_HOME`` environment variable
2. ``MINDOPT_ROOT_DIR`` CMake variable
3. Common install locations:
   - ``/opt/mindopt/<version>/linux64-x86/``
   - ``$HOME/opt/mindopt/<version>/linux64-x86/``
   - ``$HOME/mindopt/<version>/linux64-x86/``

Result variables:

  ``MINDOPT_FOUND``        — TRUE if MindOpt was found
  ``MINDOPT_INCLUDE_DIRS`` — Header include directory
  ``MINDOPT_LIBRARIES``    — Libraries to link against

#]=======================================================================]

# Build list of candidate paths
set(_MINDOPT_SEARCH_PATHS)

if(DEFINED ENV{MINDOPT_HOME})
  list(APPEND _MINDOPT_SEARCH_PATHS "$ENV{MINDOPT_HOME}")
endif()

if(MINDOPT_ROOT_DIR)
  list(APPEND _MINDOPT_SEARCH_PATHS "${MINDOPT_ROOT_DIR}")
endif()

# Scan versioned directories under common prefixes
foreach(_prefix
    "/opt/mindopt"
    "$ENV{HOME}/opt/mindopt"
    "$ENV{HOME}/mindopt")
  if(IS_DIRECTORY "${_prefix}")
    file(GLOB _versions "${_prefix}/*")
    foreach(_vdir ${_versions})
      if(IS_DIRECTORY "${_vdir}/linux64-x86")
        list(APPEND _MINDOPT_SEARCH_PATHS "${_vdir}/linux64-x86")
      endif()
      if(IS_DIRECTORY "${_vdir}")
        list(APPEND _MINDOPT_SEARCH_PATHS "${_vdir}")
      endif()
    endforeach()
  endif()
endforeach()

find_path(MINDOPT_INCLUDE_DIR
  NAMES Mindopt.h
  HINTS ${_MINDOPT_SEARCH_PATHS}
  PATH_SUFFIXES include
)

find_library(MINDOPT_LIBRARY
  NAMES mindopt
  HINTS ${_MINDOPT_SEARCH_PATHS}
  PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MindOpt
  REQUIRED_VARS MINDOPT_LIBRARY MINDOPT_INCLUDE_DIR
)

if(MINDOPT_FOUND)
  set(MINDOPT_INCLUDE_DIRS "${MINDOPT_INCLUDE_DIR}")
  set(MINDOPT_LIBRARIES    "${MINDOPT_LIBRARY}")
  mark_as_advanced(MINDOPT_INCLUDE_DIR MINDOPT_LIBRARY)
endif()
