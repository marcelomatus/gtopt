#[=======================================================================[.rst:
BuildTypeDefault
----------------

Sets ``CMAKE_BUILD_TYPE`` to ``Debug`` when no build type has been
provided on the command line and the generator does not support multiple
configurations (i.e. all single-config generators such as Makefiles and
Ninja).

Usage::

  include(cmake_local/BuildTypeDefault.cmake)

This module is a no-op for multi-config generators (Visual Studio, Xcode,
Ninja Multi-Config) where ``CMAKE_CONFIGURATION_TYPES`` is set by the
generator and ``CMAKE_BUILD_TYPE`` is ignored.

Including this module in every sub-project ``CMakeLists.txt`` that can be
built stand-alone ensures a predictable default when a developer runs
``cmake -S <subdir> -B build`` without specifying ``-DCMAKE_BUILD_TYPE``.

When the sub-project is consumed via ``add_subdirectory()`` from a
parent (e.g. ``all/CMakeLists.txt``), the parent has already set
``CMAKE_BUILD_TYPE`` in the cache, so the guard
``if(NOT CMAKE_BUILD_TYPE ...)`` is false and this module is a no-op.

#]=======================================================================]

include_guard(GLOBAL)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE
  )
  set_property(
    CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
  )
  message(STATUS "Build type not set — defaulting to Debug")
endif()
