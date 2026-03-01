if(NOT DEFINED SCRIPTS_DIR)
  message(FATAL_ERROR "SCRIPTS_DIR is required.")
endif()
if(NOT DEFINED PYTHON_EXECUTABLE)
  message(FATAL_ERROR "PYTHON_EXECUTABLE is required.")
endif()
if(NOT DEFINED STAGE_DIR)
  message(FATAL_ERROR "STAGE_DIR is required.")
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
file(COPY "${SCRIPTS_DIR}/" DESTINATION "${STAGE_DIR}"
  PATTERN "*.egg-info" EXCLUDE
  PATTERN "__pycache__" EXCLUDE
)

execute_process(
  COMMAND "${PYTHON_EXECUTABLE}" -m pip install --ignore-installed -e
    "${STAGE_DIR}[dev]"
  RESULT_VARIABLE _pip_rc
)

file(REMOVE_RECURSE "${STAGE_DIR}")

if(NOT _pip_rc EQUAL 0)
  message(FATAL_ERROR "pip install failed (exit code ${_pip_rc})")
endif()
