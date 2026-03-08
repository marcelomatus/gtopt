#[=======================================================================[.rst:
UninstallTarget
---------------

Adds an ``uninstall`` custom target that removes every file recorded in
``build/install_manifest.txt`` by the most recent ``cmake --install`` run.

Usage::

  include(cmake_local/UninstallTarget.cmake)

Effect
^^^^^^

Adds the target::

  cmake --build <build> --target uninstall

The target is guarded — if another module or sub-project has already
defined an ``uninstall`` target, this module is a no-op.

The implementation delegates to ``cmake_uninstall.cmake.in``, located in
the ``cmake/`` directory relative to this file.

#]=======================================================================]

include_guard(GLOBAL)

if(TARGET uninstall)
  return()
endif()

# Locate cmake_uninstall.cmake.in relative to this file's directory
# (cmake_local/ → ../cmake/cmake_uninstall.cmake.in)
get_filename_component(_uninstall_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
set(_uninstall_template "${_uninstall_cmake_dir}/cmake/cmake_uninstall.cmake.in")

if(NOT EXISTS "${_uninstall_template}")
  message(WARNING "UninstallTarget: cmake_uninstall.cmake.in not found at ${_uninstall_template}")
  return()
endif()

configure_file(
  "${_uninstall_template}"
  "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
  @ONLY
)

add_custom_target(uninstall
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
  COMMENT "Uninstalling files listed in ${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt"
  VERBATIM
)

unset(_uninstall_cmake_dir)
unset(_uninstall_template)
