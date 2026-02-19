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

find_program(CLANG_FORMAT_PROGRAM clang-format)
find_program(GIT_PROGRAM git)

if(CLANG_FORMAT_PROGRAM AND GIT_PROGRAM)
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
  string(JOIN " " _GIT_LS_CMD_STR ${_GIT_LS_CMD})

  add_custom_target(
    format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} -i"
    COMMENT "clang-format: formatting C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )

  add_custom_target(
    check-format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} --dry-run -Werror"
    COMMENT "clang-format: checking C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )

  add_custom_target(
    fix-format
    COMMAND bash -c "${_GIT_LS_CMD_STR} | xargs -0 ${CLANG_FORMAT_PROGRAM} -i"
    COMMENT "clang-format: fixing C/C++ files"
    VERBATIM
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
else()
  set(_NOT_FOUND_MSG "ClangFormat.cmake: clang-format and/or git not found")

  add_custom_target(format       COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
  add_custom_target(check-format COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
  add_custom_target(fix-format   COMMAND ${CMAKE_COMMAND} -E echo "${_NOT_FOUND_MSG}" COMMAND ${CMAKE_COMMAND} -E false)
endif()
