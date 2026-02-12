# Taken from
# http://www.openflipper.org/svnrepo/CoMISo/trunk/CoMISo/cmake/FindGUROBI.cmake

# Try to find GUROBI. Once done this will define:
#   GUROBI_FOUND          - System has Gurobi
#   GUROBI_INCLUDE_DIRS   - The Gurobi include directories
#   GUROBI_LIBRARIES      - The libraries needed to use Gurobi

if(GUROBI_INCLUDE_DIR)
  # in cache already
  set(GUROBI_FOUND TRUE)
  set(GUROBI_INCLUDE_DIRS "${GUROBI_INCLUDE_DIR}")
  set(GUROBI_LIBRARIES "${GUROBI_LIBRARY};${GUROBI_CXX_LIBRARY}")
else()

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

  set(GUROBI_INCLUDE_DIRS "${GUROBI_INCLUDE_DIR}")
  set(GUROBI_LIBRARIES "${GUROBI_LIBRARY};${GUROBI_CXX_LIBRARY}")

  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(
    GUROBI DEFAULT_MSG GUROBI_LIBRARY GUROBI_CXX_LIBRARY GUROBI_INCLUDE_DIR
  )

endif()
