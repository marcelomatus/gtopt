#[=======================================================================[.rst:
GuiService
----------

CMake configuration for building and installing the gtopt GUI service.

The GUI service is a Flask web application that provides a browser-based
interface for creating, editing, and visualizing gtopt optimization cases.

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
  PATTERN "static/*"
  PATTERN "templates/*"
  PATTERN "__pycache__" EXCLUDE
  PATTERN "tests" EXCLUDE
  PATTERN ".pytest_cache" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
  PATTERN "gtopt_gui.sh" EXCLUDE  # Handled separately below
  PATTERN "gtopt_gui.bat" EXCLUDE  # Handled separately below
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

# Post-install message
install(CODE "
  message(STATUS \"To run the gtopt GUI service:\")
  message(STATUS \"  1. Install Python dependencies:\")
  message(STATUS \"     python3 -m pip install -r \${CMAKE_INSTALL_PREFIX}/share/gtopt/guiservice/requirements.txt\")
  message(STATUS \"  2. Run the gtopt_gui command:\")
  message(STATUS \"     \${CMAKE_INSTALL_PREFIX}/bin/gtopt_gui\")
  message(STATUS \"  Or run the service directly:\")
  message(STATUS \"     cd \${CMAKE_INSTALL_PREFIX}/share/gtopt/guiservice\")
  message(STATUS \"     python3 app.py\")
")
