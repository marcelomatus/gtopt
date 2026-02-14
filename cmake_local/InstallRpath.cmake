#[=======================================================================[.rst:
InstallRpath
------------

Configure ``INSTALL_RPATH`` for a target so that non-system shared libraries
it links against (directly or transitively) can be found at runtime after
``cmake --install``.

Usage::

  include(InstallRpath)
  configure_install_rpath(<target> [<lib_target> ...])

``<target>``
  The executable (or shared-library) target whose ``INSTALL_RPATH`` will be
  set.

``<lib_target>``
  One or more library targets whose ``LINK_LIBRARIES`` property is inspected
  to discover non-system shared-library directories.  If omitted, only the
  direct link libraries of ``<target>`` are considered.

The function walks every item in the ``LINK_LIBRARIES`` property of each
*lib_target* (falling back to *target* itself), resolves imported-target
locations, filters out well-known system directories, and sets the result
as the target's ``INSTALL_RPATH``.

#]=======================================================================]

include_guard(GLOBAL)

function(configure_install_rpath _target)
  set(_system_lib_dirs "^(/usr/lib|/usr/lib64|/lib|/lib64|/usr/lib/x86_64-linux-gnu)$")
  set(_install_rpaths "")

  # Determine which targets to inspect for link libraries
  set(_lib_targets ${ARGN})
  if(NOT _lib_targets)
    set(_lib_targets ${_target})
  endif()

  foreach(_lib_target ${_lib_targets})
    if(NOT TARGET ${_lib_target})
      continue()
    endif()
    get_target_property(_libs ${_lib_target} LINK_LIBRARIES)
    if(NOT _libs)
      continue()
    endif()

    foreach(_item ${_libs})
      if(TARGET ${_item})
        get_target_property(_type ${_item} TYPE)
        if(_type MATCHES "SHARED_LIBRARY|UNKNOWN_LIBRARY")
          get_target_property(_loc ${_item} IMPORTED_LOCATION)
          if(NOT _loc)
            get_target_property(_configs ${_item} IMPORTED_CONFIGURATIONS)
            if(_configs)
              list(GET _configs 0 _first_cfg)
              string(TOUPPER "${_first_cfg}" _uc)
              get_target_property(_loc ${_item} IMPORTED_LOCATION_${_uc})
            endif()
          endif()
          if(_loc AND EXISTS "${_loc}")
            get_filename_component(_dir "${_loc}" DIRECTORY)
            if(NOT _dir MATCHES "${_system_lib_dirs}")
              list(APPEND _install_rpaths "${_dir}")
            endif()
          endif()
        endif()
      elseif(_item AND EXISTS "${_item}")
        # Plain library path (e.g. /opt/coinor/lib/libOsiClp.so)
        get_filename_component(_dir "${_item}" DIRECTORY)
        if(NOT _dir MATCHES "${_system_lib_dirs}")
          list(APPEND _install_rpaths "${_dir}")
        endif()
      endif()
    endforeach()
  endforeach()

  list(REMOVE_DUPLICATES _install_rpaths)

  if(_install_rpaths)
    set_target_properties(${_target} PROPERTIES INSTALL_RPATH "${_install_rpaths}")
  endif()
endfunction()
