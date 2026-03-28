#[=======================================================================[.rst:
GtoptPlugin
-----------

Utility function to define a solver backend plugin with consistent
build settings, output location, RPATH, and install rules.

Usage::

  gtopt_add_solver_plugin(
    NAME       gtopt_solver_cplex
    SOURCES    cplex_solver_backend.cpp cplex_plugin.cpp
    INCLUDES   "${CPLEX_INCLUDE_DIRS}"
    LIBRARIES  ${CPLEX_LIBRARIES}
  )

All plugins are MODULE libraries placed in ``${CMAKE_BINARY_DIR}/plugins``
at build time and installed to ``lib/gtopt/plugins``.

#]=======================================================================]

include_guard(GLOBAL)
include(InstallRpath)

#[=======================================================================[
Helper: verify that a library comes from the same directory as a
reference directory (ABI consistency).  Sets ``<out_var>`` to TRUE/FALSE.

Usage::

  gtopt_check_coin_abi(
    REFERENCE_DIR  "/opt/coinor/lib"
    LIBRARY        "${COIN_OSICBC_LIBRARY}"
    LABEL          "OsiCbc"
    RESULT         _cbc_abi_ok
  )

#]=======================================================================]
function(gtopt_check_coin_abi)
  cmake_parse_arguments(PARSE_ARGV 0 _A "" "REFERENCE_DIR;LIBRARY;LABEL;RESULT" "")

  set(${_A_RESULT} TRUE PARENT_SCOPE)
  if(NOT _A_REFERENCE_DIR OR NOT _A_LIBRARY)
    return()
  endif()
  get_filename_component(_ref "${_A_REFERENCE_DIR}" REALPATH)
  get_filename_component(_lib_dir "${_A_LIBRARY}" DIRECTORY)
  get_filename_component(_lib_dir "${_lib_dir}" REALPATH)
  if(NOT _lib_dir STREQUAL _ref)
    message(WARNING
      "COIN-OR ABI mismatch: ${_A_LABEL} at ${_A_LIBRARY} "
      "but core Osi/CoinUtils at ${_A_REFERENCE_DIR}. "
      "Skipping ${_A_LABEL}. Fix: install all COIN-OR from the same source."
    )
    set(${_A_RESULT} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(gtopt_add_solver_plugin)
  cmake_parse_arguments(
    PARSE_ARGV 0 _P ""
    "NAME"
    "SOURCES;INCLUDES;LIBRARIES;DEFINITIONS"
  )

  if(NOT _P_NAME)
    message(FATAL_ERROR "gtopt_add_solver_plugin: NAME is required")
  endif()
  if(NOT _P_SOURCES)
    message(FATAL_ERROR "gtopt_add_solver_plugin(${_P_NAME}): SOURCES is required")
  endif()

  add_library(${_P_NAME} MODULE ${_P_SOURCES})

  # Private include path for plugin-local headers.
  target_include_directories(${_P_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  # External solver headers as SYSTEM to suppress warnings.
  if(_P_INCLUDES)
    target_include_directories(${_P_NAME} SYSTEM PRIVATE
      ${_P_INCLUDES}
    )
  endif()

  # Link gtopt core + solver libraries.
  target_link_libraries(${_P_NAME} PRIVATE
    gtopt::gtopt
    ${_P_LIBRARIES}
  )

  # Optional compile definitions.
  if(_P_DEFINITIONS)
    target_compile_definitions(${_P_NAME} PRIVATE ${_P_DEFINITIONS})
  endif()

  # Consistent output directory and naming across all plugins.
  set_target_properties(${_P_NAME} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    PREFIX "lib"
    INSTALL_RPATH_USE_LINK_PATH TRUE
  )

  # Preserve RPATH to non-system solver libraries.
  configure_install_rpath(${_P_NAME})

  # Install to the standard plugin directory.
  install(
    TARGETS ${_P_NAME}
    LIBRARY DESTINATION lib/gtopt/plugins
  )
endfunction()
