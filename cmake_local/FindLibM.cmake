#[=======================================================================[.rst:
FindLibM
--------

Find the C math library (``libm``).

Imported variables
^^^^^^^^^^^^^^^^^^

``LibM_FOUND``
  True if libm was found.

``LibM_INCLUDES``
  Path to the ``math.h`` header.

``LibM_LIBRARIES``
  Libraries to link against.

``LibM_LIBRARY_DIR``
  Directory containing the library.

#]=======================================================================]

include_guard(GLOBAL)

find_path(
  LibM_INCLUDES
  NAMES math.h
  PATHS ${include_locations} ${lib_locations}
)

find_library(
  LibM_LIBRARY
  NAMES m
  PATHS ${LibM_LIBRARY_DIR}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  LibM
  REQUIRED_VARS LibM_INCLUDES LibM_LIBRARY
)

if(LibM_FOUND)
  set(LibM_LIBRARIES "${LibM_LIBRARY}")
  get_filename_component(LibM_LIBRARY_DIR "${LibM_LIBRARY}" DIRECTORY)
endif()

mark_as_advanced(LibM_INCLUDES LibM_LIBRARIES LibM_LIBRARY LibM_LIBRARY_DIR)
