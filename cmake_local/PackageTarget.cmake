#[=======================================================================[.rst:
PackageTarget
-------------

Create a namespaced alias and version header for a library target, and
optionally install it via ``packageProject()``.

Usage::

  include(PackageTarget)
  package_target(
    NAME <name>
    VERSION <version>
    NAMESPACE <namespace>
    BINARY_DIR <binary_dir>
    INCLUDE_DIR <include_dir>
    INCLUDE_DESTINATION <include_destination>
    VERSION_HEADER <version_header>
    COMPATIBILITY <compatibility>
    [DEPENDENCIES <dep> ...]
  )

The function always:

* creates an ``ALIAS`` target ``<namespace>::<name>``
* generates a version header at ``<version_header>`` and adds the
  containing directory to the target's ``BUILD_INTERFACE`` includes

When ``PROJECT_IS_TOP_LEVEL`` is true the function also forwards all
arguments to ``packageProject()`` so that ``cmake --install`` sets up
the library for downstream ``find_package()`` consumption.

#]=======================================================================]

include_guard(GLOBAL)

function(package_target)
  cmake_parse_arguments(
    PT
    ""
    "NAME;VERSION;NAMESPACE;BINARY_DIR;INCLUDE_DIR;INCLUDE_DESTINATION;VERSION_HEADER;COMPATIBILITY"
    "DEPENDENCIES"
    ${ARGN}
  )

  # ---- Namespaced alias ----
  if(NOT TARGET ${PT_NAMESPACE}::${PT_NAME})
    add_library(${PT_NAMESPACE}::${PT_NAME} ALIAS ${PT_NAME})
  endif()

  # ---- Version header ----
  set(_version_include_dir "${PT_BINARY_DIR}/PackageProjectInclude")

  string(TOUPPER ${PT_NAME} _upper_name)
  string(REGEX REPLACE [^a-zA-Z0-9] _ _upper_name ${_upper_name})
  set(UPPERCASE_PROJECT_NAME ${_upper_name})

  # Parse the version string passed via VERSION into its components so that
  # the generated header always contains correct numeric values, regardless
  # of whether the calling project() specified all four components.
  string(REPLACE "." ";" _version_parts "${PT_VERSION}")
  list(LENGTH _version_parts _version_len)
  list(GET _version_parts 0 PROJECT_VERSION_MAJOR)
  if(_version_len GREATER 1)
    list(GET _version_parts 1 PROJECT_VERSION_MINOR)
  else()
    set(PROJECT_VERSION_MINOR 0)
  endif()
  if(_version_len GREATER 2)
    list(GET _version_parts 2 PROJECT_VERSION_PATCH)
  else()
    set(PROJECT_VERSION_PATCH 0)
  endif()
  if(_version_len GREATER 3)
    list(GET _version_parts 3 PROJECT_VERSION_TWEAK)
  else()
    set(PROJECT_VERSION_TWEAK 0)
  endif()
  set(PROJECT_VERSION "${PT_VERSION}")

  configure_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/version.hpp.in"
    "${_version_include_dir}/${PT_VERSION_HEADER}" @ONLY
  )
  target_include_directories(
    ${PT_NAME} PUBLIC "$<BUILD_INTERFACE:${_version_include_dir}>"
  )

  # ---- Install (top-level only) ----
  if(PROJECT_IS_TOP_LEVEL)
    packageProject(
      NAME ${PT_NAME}
      VERSION ${PT_VERSION}
      NAMESPACE ${PT_NAMESPACE}
      BINARY_DIR ${PT_BINARY_DIR}
      INCLUDE_DIR ${PT_INCLUDE_DIR}
      INCLUDE_DESTINATION ${PT_INCLUDE_DESTINATION}
      VERSION_HEADER "${PT_VERSION_HEADER}"
      COMPATIBILITY ${PT_COMPATIBILITY}
      DEPENDENCIES ${PT_DEPENDENCIES}
    )
  endif()
endfunction()
