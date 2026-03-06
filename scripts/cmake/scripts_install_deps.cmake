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

# Prefer uv when available: it is 10–100× faster than pip on cold installs
# (Rust-based resolver + parallel downloads) and is pre-installed on
# ubuntu-latest GitHub Actions runners.
# Fall back to pip when uv is not found (local dev, other CI environments).
find_program(UV_EXECUTABLE uv)
if(UV_EXECUTABLE)
  # Detect whether the target Python lives inside a virtual environment.
  # uv requires --system to install into a non-venv Python interpreter;
  # without it the install would fail when running outside a venv.
  execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" -c
      "import sys; sys.exit(0 if (hasattr(sys,'real_prefix') or getattr(sys,'base_prefix',sys.prefix)!=sys.prefix) else 1)"
    RESULT_VARIABLE _in_venv
    OUTPUT_QUIET ERROR_QUIET
  )
  if(_in_venv EQUAL 0)
    set(_UV_SYSTEM_FLAG "")
  else()
    set(_UV_SYSTEM_FLAG "--system")
  endif()
  execute_process(
    COMMAND "${UV_EXECUTABLE}" pip install -q
      --python "${PYTHON_EXECUTABLE}"
      ${_UV_SYSTEM_FLAG}
      -e "${STAGE_DIR}[dev]"
    RESULT_VARIABLE _pip_rc
  )
else()
  execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" -m pip install -q -e "${STAGE_DIR}[dev]"
    RESULT_VARIABLE _pip_rc
  )
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")

if(NOT _pip_rc EQUAL 0)
  message(FATAL_ERROR "pip install failed (exit code ${_pip_rc})")
endif()
