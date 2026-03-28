#[=======================================================================[.rst:
GitReports
----------

Provides ``add_git_commit_reports_target()`` — a convenience target that
commits generated report files to the current branch.

Usage::

  include(GitReports)
  add_git_commit_reports_target(<target-name> <reports-directory>)

#]=======================================================================]

include_guard(GLOBAL)

find_program(GIT_EXECUTABLE NAMES git)

function(add_git_commit_reports_target target_name reports_dir)
  if(NOT GIT_EXECUTABLE)
    message(STATUS "git not found — ${target_name} target will not be created")
    return()
  endif()

  add_custom_target(
    ${target_name}
    COMMAND ${GIT_EXECUTABLE} add -A "${reports_dir}"
    COMMAND ${GIT_EXECUTABLE} commit -m "chore: update coverage reports"
    WORKING_DIRECTORY "${reports_dir}/.."
    COMMENT "Committing reports in ${reports_dir}..."
    USES_TERMINAL
    VERBATIM
  )
endfunction()
