#[=======================================================================[.rst:
GuiService
----------

CMake configuration for building and installing the gtopt GUI service.

The GUI service is a Flask web application that provides a browser-based
interface for creating, editing, and visualizing gtopt optimization cases.

**Note**: This CMake configuration is independent of the main gtopt library
and standalone binary. The GUI service can be installed separately without
building the C++ gtopt solver.

Build targets:

``guiservice-install``
  Installs Python dependencies for the GUI service.

``guiservice-test``
  Runs the GUI service test suite.

``guiservice-start``
  Starts the GUI service in development mode.

Configuration variables:

``GTOPT_GUISERVICE_PORT``
  Port for the GUI service (default: 5001).

``GTOPT_WEBSERVICE_URL``
  URL of the gtopt webservice for remote solving (default: http://localhost:3000).

Installation:

To install the GUI service::

  cmake -S guiservice -B build-gui
  cmake --install build-gui

The install step always uses the Python found at **configure time**
(``${PYTHON_EXECUTABLE} -m pip``).  The absolute path is baked into
``cmake_install.cmake`` so it is immune to the PATH-stripping that
``sudo`` applies by default, making both ``cmake --install`` (as a
regular user) and ``sudo cmake --install`` (system-wide) work
correctly with the same Python.

This installs:
- ``gtopt_gui`` launcher script to ``bin/gtopt_gui``
- GUI service files to ``share/gtopt/guiservice/``

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(gtoptGuiService LANGUAGES NONE)

# ---- Configuration ----

set(GTOPT_GUISERVICE_PORT "5001" CACHE STRING "GUI service port")
set(GTOPT_WEBSERVICE_URL "http://localhost:3000" CACHE STRING "Webservice URL")

find_program(PYTHON_EXECUTABLE python3 REQUIRED)

set(GUISERVICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- Check Python version ----

execute_process(
  COMMAND ${PYTHON_EXECUTABLE} -c "import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)"
  RESULT_VARIABLE PYTHON_VERSION_CHECK
  OUTPUT_QUIET
  ERROR_QUIET
)

if(NOT PYTHON_VERSION_CHECK EQUAL 0)
  message(WARNING "Python 3.10 or later is required for guiservice. Found: ${PYTHON_EXECUTABLE}")
endif()

# ---- Install Python dependencies ----

add_custom_target(guiservice-install
  COMMAND ${PYTHON_EXECUTABLE} -m pip install -r requirements.txt
  WORKING_DIRECTORY "${GUISERVICE_DIR}"
  COMMENT "Installing GUI service Python dependencies"
)

# ---- Test ----

add_custom_target(guiservice-test
  COMMAND ${PYTHON_EXECUTABLE} -m pytest tests/test_app.py -v
  WORKING_DIRECTORY "${GUISERVICE_DIR}"
  COMMENT "Running GUI service tests"
  DEPENDS guiservice-install
)

# ---- Start (development) ----

add_custom_target(guiservice-dev
  COMMAND ${CMAKE_COMMAND} -E env
    "GTOPT_GUI_PORT=${GTOPT_GUISERVICE_PORT}"
    "GTOPT_WEBSERVICE_URL=${GTOPT_WEBSERVICE_URL}"
    "FLASK_DEBUG=1"
    ${PYTHON_EXECUTABLE} app.py
  WORKING_DIRECTORY "${GUISERVICE_DIR}"
  COMMENT "Starting GUI service in development mode on port ${GTOPT_GUISERVICE_PORT}"
)

# ---- Install ----

# Install guiservice Python files and assets
install(
  DIRECTORY "${GUISERVICE_DIR}/"
  DESTINATION share/gtopt/guiservice
  FILES_MATCHING
  PATTERN "*.py"
  PATTERN "*.txt"  # requirements.txt
  PATTERN "*.md"
  PATTERN "*.service"  # systemd service file
  PATTERN "*.css"  # static/css/style.css
  PATTERN "*.js"   # static/js/app.js
  PATTERN "*.html" # templates/index.html
  PATTERN "CMakeLists.txt" EXCLUDE
  PATTERN "__pycache__" EXCLUDE
  PATTERN "tests" EXCLUDE
  PATTERN ".pytest_cache" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
  PATTERN "gtopt_gui.sh" EXCLUDE  # Handled separately below
  PATTERN "gtopt_gui.bat" EXCLUDE  # Handled separately below
  PATTERN "gtopt_guisrv.sh" EXCLUDE  # Handled separately below
  PATTERN "gtopt_guisrv.bat" EXCLUDE  # Handled separately below
)

# Install the gtopt_gui launcher script (Unix-like systems)
if(UNIX)
  install(
    PROGRAMS "${GUISERVICE_DIR}/gtopt_gui.sh"
    DESTINATION bin
    RENAME gtopt_gui
  )
endif()

# Install the gtopt_gui launcher script (Windows)
if(WIN32)
  install(
    PROGRAMS "${GUISERVICE_DIR}/gtopt_gui.bat"
    DESTINATION bin
    RENAME gtopt_gui.bat
  )
endif()

# Install the gtopt_guisrv launcher script (Unix-like systems)
if(UNIX)
  install(
    PROGRAMS "${GUISERVICE_DIR}/gtopt_guisrv.sh"
    DESTINATION bin
    RENAME gtopt_guisrv
  )
endif()

# Install the gtopt_guisrv launcher script (Windows)
if(WIN32)
  install(
    PROGRAMS "${GUISERVICE_DIR}/gtopt_guisrv.bat"
    DESTINATION bin
    RENAME gtopt_guisrv.bat
  )
endif()

# Post-install verification: ensure all critical files were installed
install(CODE "
  set(_guiservice_dir \"\${CMAKE_INSTALL_PREFIX}/share/gtopt/guiservice\")
  set(_missing_files)
  foreach(_file
      app.py
      requirements.txt
      static/css/style.css
      static/js/app.js
      static/vendor/jsuites/jsuites.js
      static/vendor/jsuites/jsuites.css
      static/vendor/jspreadsheet-ce/jspreadsheet.js
      static/vendor/jspreadsheet-ce/jspreadsheet.css
      templates/index.html
  )
    if(NOT EXISTS \"\${_guiservice_dir}/\${_file}\")
      list(APPEND _missing_files \"\${_file}\")
    endif()
  endforeach()
  if(_missing_files)
    message(FATAL_ERROR
      \"Guiservice installation is incomplete. Missing files:\\n\"
      \"\${_missing_files}\\n\"
      \"in \${_guiservice_dir}\")
  else()
    message(STATUS \"Guiservice installation verified: all critical files present\")
  endif()
")

# Post-install: install Python dependencies via pip
install(CODE "
  message(STATUS \"Installing gtopt guiservice Python dependencies via pip...\")

  # Use the Python found at configure time.  Its absolute path is baked in
  # here, so it is immune to the PATH-stripping that sudo applies by default.
  # Both plain 'cmake --install' and 'sudo cmake --install' therefore use the
  # same Python, producing a predictable installation target.
  set(_pip_cmd \"${PYTHON_EXECUTABLE}\" -m pip)
  message(STATUS \"Using pip: ${PYTHON_EXECUTABLE} -m pip\")

  execute_process(
    COMMAND \${_pip_cmd} install
      --ignore-installed
      -r \"${GUISERVICE_DIR}/requirements.txt\"
    RESULT_VARIABLE _pip_rc
  )
  if(NOT _pip_rc EQUAL 0)
    message(WARNING \"pip install failed (exit code \${_pip_rc}). \"
      \"Please run manually: pip install --ignore-installed -r ${GUISERVICE_DIR}/requirements.txt\")
  else()
    message(STATUS \"Python dependencies installed successfully\")
  endif()

  message(STATUS \"To run the gtopt GUI service:\")
  message(STATUS \"  \${CMAKE_INSTALL_PREFIX}/bin/gtopt_gui      # Interactive kiosk mode\")
  message(STATUS \"  \${CMAKE_INSTALL_PREFIX}/bin/gtopt_guisrv   # Web server mode\")
  message(STATUS \"Or run the service directly:\")
  message(STATUS \"  cd \${CMAKE_INSTALL_PREFIX}/share/gtopt/guiservice\")
  message(STATUS \"  python3 app.py\")
")
