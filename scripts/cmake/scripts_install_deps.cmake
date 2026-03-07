if(NOT DEFINED SCRIPTS_DIR)
  message(FATAL_ERROR "SCRIPTS_DIR is required.")
endif()
if(NOT DEFINED PYTHON_EXECUTABLE)
  message(FATAL_ERROR "PYTHON_EXECUTABLE is required.")
endif()

# Use pip directly to install the scripts package in editable mode with all
# dev dependencies.  Avoid uv here: uv pip install --system (used by the CI
# pre-install step) creates gtopt_scripts.egg-info with metadata that
# subsequent uv invocations cannot update (permission / timestamp conflicts),
# causing the CTest fixture to fail for regular cmake users who happen to
# have uv on their PATH.  pip handles the editable-install metadata
# correctly in all environments without any special flags.
#
# Install directly from SCRIPTS_DIR (the real source tree) so that the
# editable install metadata always points to a directory that persists.
execute_process(
  COMMAND "${PYTHON_EXECUTABLE}" -m pip install -q -e "${SCRIPTS_DIR}[dev]"
  RESULT_VARIABLE _pip_rc
)

if(NOT _pip_rc EQUAL 0)
  message(FATAL_ERROR "pip install failed (exit code ${_pip_rc})")
endif()
