# ClangFormat.cmake – format targets restricted to C/C++ source files.
#
# Provides the following custom targets:
#   format            – reformat C/C++ files in-place
#   check-format      – dry-run check (fails on any diff)
#   fix-format        – alias for format
#
# The file list is obtained with `git ls-files` so only tracked files are
# considered, and the set of extensions is limited to C/C++ to avoid running
# clang-format on languages it does not fully support (JS, TS, JSON, …).

if(TARGET format)
  return()
endif()

find_program(CLANG_FORMAT_PROGRAM clang-format)
find_program(GIT_PROGRAM git)

if(CLANG_FORMAT_PROGRAM AND GIT_PROGRAM)
  # Determine the git repository root for correct working directory
  execute_process(
    COMMAND ${GIT_PROGRAM} rev-parse --show-toplevel
    OUTPUT_VARIABLE _CF_GIT_ROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT _CF_GIT_ROOT)
    set(_CF_GIT_ROOT ${CMAKE_SOURCE_DIR})
  endif()

  # C/C++ extensions recognised by clang-format
  set(_CF_GLOBS
      "*.h" "*.c"
      "*.hpp" "*.cpp"
      "*.hxx" "*.cxx"
      "*.hh" "*.cc"
      "*.ipp" "*.inc" "*.inl"
  )

  # Directories to exclude from formatting
  set(_CF_EXCLUDE_DIRS "cmake/")

  # Build the git ls-files argument list
  set(_GIT_LS_CMD ${GIT_PROGRAM} ls-files -z)
  foreach(_g IN LISTS _CF_GLOBS)
    list(APPEND _GIT_LS_CMD "${_g}")
  endforeach()
  foreach(_d IN LISTS _CF_EXCLUDE_DIRS)
    list(APPEND _GIT_LS_CMD ":(exclude)${_d}")
  endforeach()

  # We build a small shell one-liner so that the file list comes from
  # git ls-files at *build* time (not at configure time).
  # Each argument is single-quoted to protect pathspecs like :(exclude).
  set(_GIT_LS_CMD_STR "")
  foreach(_a IN LISTS _GIT_LS_CMD)
    if(_GIT_LS_CMD_STR)
      string(APPEND _GIT_LS_CMD_STR " ")
    endif()
    string(APPEND _GIT_LS_CMD_STR "'${_a}'")
  endforeach()

  add_custom_target(
    format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} -i"
    COMMENT "clang-format: formatting C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${_CF_GIT_ROOT}
  )

  add_custom_target(
    check-format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} --dry-run -Werror"
    COMMENT "clang-format: checking C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${_CF_GIT_ROOT}
  )

  add_custom_target(
    fix-format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} -i"
    COMMENT "clang-format: fixing C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${_CF_GIT_ROOT}
  )
else()
  set(_NOT_FOUND_MSG "ClangFormat.cmake: clang-format and/or git not found")

  add_custom_target(format       COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
  add_custom_target(check-format COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
  add_custom_target(fix-format   COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
endif()
