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

# Helper: try to resolve the library path for an imported target.
# Checks IMPORTED_LOCATION, then every IMPORTED_LOCATION_<CONFIG>, and
# finally falls back to the generic LOCATION property.
function(_resolve_imported_location _target _out_var)
  set(_loc "")

  # 1. IMPORTED_LOCATION (generic)
  get_target_property(_loc ${_target} IMPORTED_LOCATION)

  # 2. IMPORTED_LOCATION_<CONFIG> for each known configuration
  if(NOT _loc)
    get_target_property(_configs ${_target} IMPORTED_CONFIGURATIONS)
    if(_configs)
      foreach(_cfg ${_configs})
        string(TOUPPER "${_cfg}" _uc)
        get_target_property(_loc ${_target} IMPORTED_LOCATION_${_uc})
        if(_loc)
          break()
        endif()
      endforeach()
    endif()
  endif()

  # 3. LOCATION fallback (works for most generator expressions at
  #    configure time and is the property CMake itself resolves)
  if(NOT _loc)
    get_property(_loc TARGET ${_target} PROPERTY LOCATION)
  endif()

  set(${_out_var} "${_loc}" PARENT_SCOPE)
endfunction()

function(configure_install_rpath _target)
  set(_system_lib_dirs "^(/usr/lib|/usr/lib64|/lib|/lib64|/usr/lib/x86_64-linux-gnu)$")
  set(_install_rpaths "")

  # Determine which targets to inspect for link libraries
  set(_lib_targets ${ARGN})
  if(NOT _lib_targets)
    set(_lib_targets ${_target})
  endif()

  # Collect all items to inspect (direct + transitive INTERFACE_LINK_LIBRARIES)
  set(_all_items "")
  foreach(_lib_target ${_lib_targets})
    if(NOT TARGET ${_lib_target})
      continue()
    endif()
    get_target_property(_libs ${_lib_target} LINK_LIBRARIES)
    if(_libs)
      list(APPEND _all_items ${_libs})
    endif()
    get_target_property(_ilibs ${_lib_target} INTERFACE_LINK_LIBRARIES)
    if(_ilibs)
      list(APPEND _all_items ${_ilibs})
    endif()
  endforeach()

  # Walk collected items and resolve shared-library directories
  foreach(_item ${_all_items})
    if(TARGET ${_item})
      get_target_property(_type ${_item} TYPE)
      if(_type MATCHES "SHARED_LIBRARY|UNKNOWN_LIBRARY")
        _resolve_imported_location(${_item} _loc)
        if(_loc AND EXISTS "${_loc}")
          get_filename_component(_dir "${_loc}" DIRECTORY)
          if(NOT _dir MATCHES "${_system_lib_dirs}")
            list(APPEND _install_rpaths "${_dir}")
          endif()
        endif()
      elseif(_type STREQUAL "INTERFACE_LIBRARY")
        # Interface libraries may pull in shared libraries transitively
        get_target_property(_ilibs ${_item} INTERFACE_LINK_LIBRARIES)
        if(_ilibs)
          foreach(_ilib ${_ilibs})
            if(TARGET ${_ilib})
              get_target_property(_itype ${_ilib} TYPE)
              if(_itype MATCHES "SHARED_LIBRARY|UNKNOWN_LIBRARY")
                _resolve_imported_location(${_ilib} _iloc)
                if(_iloc AND EXISTS "${_iloc}")
                  get_filename_component(_idir "${_iloc}" DIRECTORY)
                  if(NOT _idir MATCHES "${_system_lib_dirs}")
                    list(APPEND _install_rpaths "${_idir}")
                  endif()
                endif()
              endif()
            endif()
          endforeach()
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

  list(REMOVE_DUPLICATES _install_rpaths)

  if(_install_rpaths)
    set_target_properties(${_target} PROPERTIES INSTALL_RPATH "${_install_rpaths}")
  endif()
endfunction()
