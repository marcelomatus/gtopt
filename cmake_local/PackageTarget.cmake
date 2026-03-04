#[=======================================================================[.rst:
PackageTarget
-------------

Create a namespaced alias and version header for a library target.

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

The function:

* creates an ``ALIAS`` target ``<namespace>::<name>``
* generates a version header at ``<version_header>`` and adds the
  containing directory to the target's ``BUILD_INTERFACE`` includes

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

  # ---- Install ----
  # GTOPT_INSTALL_LIBRARY (default OFF)
  #   Install the library binary (libgtopt.a/.so) and the CMake package config
  #   files so that external projects can consume gtopt via find_package().
  #   Within a cmake -S all -B build, no installation is required: all
  #   component subprojects share the in-tree build target gtopt::gtopt
  #   through the if(NOT TARGET gtopt::gtopt) guard.
  #
  # GTOPT_INSTALL_HEADERS (default OFF)
  #   Also install the public C++ header tree under ${CMAKE_INSTALL_INCLUDEDIR}.
  #   Keep this OFF when you only need to link against a pre-built libgtopt for
  #   a co-located build; turn it ON when shipping a development package that
  #   other projects will build against.
  #
  # Usage after installation:
  #   find_package(gtopt REQUIRED)
  #   target_link_libraries(my_app PRIVATE gtopt::gtopt)

  option(GTOPT_INSTALL_LIBRARY "Install the gtopt library and CMake package files" OFF)
  option(GTOPT_INSTALL_HEADERS "Install the gtopt public C++ headers (requires GTOPT_INSTALL_LIBRARY)" OFF)

  if(GTOPT_INSTALL_LIBRARY)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(_cmake_install_dir
        "${CMAKE_INSTALL_LIBDIR}/cmake/${PT_NAME}-${PT_VERSION}"
    )

    # ---- Library binary ----
    # IMPORTANT: no EXPORT clause here.  Using install(EXPORT) with targets
    # whose PUBLIC link deps are built from CPM source (not find_package
    # IMPORTED targets) causes CMake to error:
    #   "install(EXPORT) includes target 'gtopt' which requires target
    #    'spdlog' that is not in any export set."
    # Instead we write a manual IMPORTED target in gtoptConfig.cmake.in.
    install(
      TARGETS ${PT_NAME}
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    # ---- Public headers (optional) ----
    if(GTOPT_INSTALL_HEADERS)
      install(
        DIRECTORY "${PT_INCLUDE_DIR}/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING
          PATTERN "*.hpp"
          PATTERN "*.h"
      )

      # Generated version header
      install(
        FILES "${_version_include_dir}/${PT_VERSION_HEADER}"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${PT_NAME}"
      )
    endif() # GTOPT_INSTALL_HEADERS

    # ---- Bundled COIN-OR Find modules ----
    # Consumers who call find_package(gtopt) need to locate COIN-OR libraries
    # and headers.  The custom Find*.cmake modules and Solver.cmake from
    # cmake_local/ are installed alongside the Config file so that
    # gtoptConfig.cmake can make them available without requiring consumers to
    # copy them manually.
    install(
      DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/"
      DESTINATION "${_cmake_install_dir}/cmake_local"
      FILES_MATCHING
        PATTERN "Find*.cmake"
        PATTERN "Solver.cmake"
        PATTERN "CompilerWarnings.cmake" EXCLUDE
        PATTERN "PackageTarget.cmake" EXCLUDE
        PATTERN "DefaultInstallPrefix.cmake" EXCLUDE
        PATTERN "InstallRpath.cmake" EXCLUDE
        PATTERN "version.hpp.in" EXCLUDE
        PATTERN "gtoptConfig.cmake.in" EXCLUDE
    )

    # ---- Config and ConfigVersion files ----
    # gtoptConfig.cmake.in creates a manual IMPORTED target (gtopt::gtopt)
    # pointing to the installed library.  PATH_VARS makes the lib and include
    # directories relocatable relative to the Config file's location.
    configure_package_config_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${PT_NAME}Config.cmake.in"
      "${PT_BINARY_DIR}/${PT_NAME}Config.cmake"
      INSTALL_DESTINATION "${_cmake_install_dir}"
      PATH_VARS CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR
    )

    write_basic_package_version_file(
      "${PT_BINARY_DIR}/${PT_NAME}ConfigVersion.cmake"
      VERSION "${PT_VERSION}"
      COMPATIBILITY "${PT_COMPATIBILITY}"
    )

    install(
      FILES
        "${PT_BINARY_DIR}/${PT_NAME}Config.cmake"
        "${PT_BINARY_DIR}/${PT_NAME}ConfigVersion.cmake"
      DESTINATION "${_cmake_install_dir}"
    )

  endif() # GTOPT_INSTALL_LIBRARY
endfunction()
