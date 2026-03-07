# this file contains a list of tools that can be activated and downloaded on-demand
# each tool is enabled during configuration by passing an additional
# `-DUSE_<TOOL>=<VALUE>` argument to CMake

# only activate tools for top level project
if(NOT PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  return()
endif()

include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

# enables sanitizers support using the the `USE_SANITIZER` flag available values are:
# Address, Memory, MemoryWithOrigins, Undefined, Thread, Leak, 'Address;Undefined'
if(USE_SANITIZER OR USE_STATIC_ANALYZER)
  CPMAddPackage("gh:StableCoder/cmake-scripts#24.08.1")

  if(USE_SANITIZER)
    include(${cmake-scripts_SOURCE_DIR}/sanitizers.cmake)
  endif()

  if(USE_STATIC_ANALYZER)
    if("clang-tidy" IN_LIST USE_STATIC_ANALYZER)
      set(CLANG_TIDY
          ON
          CACHE INTERNAL ""
      )
    else()
      set(CLANG_TIDY
          OFF
          CACHE INTERNAL ""
      )
    endif()
    if("iwyu" IN_LIST USE_STATIC_ANALYZER)
      set(IWYU
          ON
          CACHE INTERNAL ""
      )
    else()
      set(IWYU
          OFF
          CACHE INTERNAL ""
      )
    endif()
    if("cppcheck" IN_LIST USE_STATIC_ANALYZER)
      set(CPPCHECK
          ON
          CACHE INTERNAL ""
      )
    else()
      set(CPPCHECK
          OFF
          CACHE INTERNAL ""
      )
    endif()

    include(${cmake-scripts_SOURCE_DIR}/tools.cmake)

    if(${CLANG_TIDY})
      clang_tidy(${CLANG_TIDY_ARGS})
    endif()

    if(${IWYU})
      include_what_you_use(${IWYU_ARGS})
    endif()

    if(${CPPCHECK})
      cppcheck(${CPPCHECK_ARGS})
    endif()
  endif()
endif()

# enables CCACHE support through the USE_CCACHE flag possible values are: YES, NO or
# equivalent.  Uses the native CMake compiler-launcher mechanism so that no CPM
# download is required and externally supplied CMAKE_<LANG>_COMPILER_LAUNCHER
# values (e.g. from the CI command line) are always honoured.
if(USE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    if(NOT CMAKE_C_COMPILER_LAUNCHER)
      set(CMAKE_C_COMPILER_LAUNCHER
          "${CCACHE_PROGRAM}"
          CACHE STRING "C compiler launcher" FORCE
      )
    endif()
    if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
      set(CMAKE_CXX_COMPILER_LAUNCHER
          "${CCACHE_PROGRAM}"
          CACHE STRING "CXX compiler launcher" FORCE
      )
    endif()
    message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
  else()
    message(STATUS "USE_CCACHE=YES but ccache not found in PATH — building without cache")
  endif()
endif()
