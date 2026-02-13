#[=======================================================================[.rst:
FindGUROBI
----------

Find the Gurobi Optimizer.

Imported variables
^^^^^^^^^^^^^^^^^^

``GUROBI_FOUND``
  True if Gurobi was found.

``GUROBI_INCLUDE_DIRS``
  Include directories.

``GUROBI_LIBRARIES``
  Libraries to link against.

#]=======================================================================]

include_guard(GLOBAL)

find_path(
  GUROBI_INCLUDE_DIR
  NAMES gurobi_c++.h
  PATHS "$ENV{GUROBI_HOME}/include" "${GUROBI_ROOT_DIR}/linux64/include"
        "/usr/local/gurobi/linux64/include"
)

find_library(
  GUROBI_LIBRARY
  NAMES gurobi
        gurobi45
        gurobi46
        gurobi50
        gurobi51
        gurobi52
        gurobi55
        gurobi56
        gurobi60
  PATHS "$ENV{GUROBI_HOME}/lib" "/opt/gurobi/linux64/lib"
        "/usr/local/gurobi/linux64/lib"
)

find_library(
  GUROBI_CXX_LIBRARY
  NAMES gurobi_c++
  PATHS "$ENV{GUROBI_HOME}/lib" "/opt/gurobi/linux64/lib"
        "/usr/local/gurobi/linux64/lib"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  GUROBI
  REQUIRED_VARS GUROBI_LIBRARY GUROBI_CXX_LIBRARY GUROBI_INCLUDE_DIR
)

if(GUROBI_FOUND)
  set(GUROBI_INCLUDE_DIRS "${GUROBI_INCLUDE_DIR}")
  set(GUROBI_LIBRARIES "${GUROBI_LIBRARY};${GUROBI_CXX_LIBRARY}")
endif()

mark_as_advanced(GUROBI_INCLUDE_DIR GUROBI_LIBRARY GUROBI_CXX_LIBRARY)
