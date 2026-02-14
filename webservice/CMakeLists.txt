#[=======================================================================[.rst:
WebService
----------

CMake configuration for building and installing the gtopt web service.

The web service is a Next.js application that provides a REST API for
uploading gtopt input cases, running the solver, and downloading results.

**Note**: This CMake configuration is independent of the main gtopt library
and standalone binary. The web service can be installed separately without
building the C++ gtopt solver (though it requires the gtopt binary to run).

Build targets:

``webservice-install``
  Installs npm dependencies for the web service.

``webservice-build``
  Builds the Next.js application for production.

``webservice-start``
  Starts the web service in production mode.

Configuration variables:

``GTOPT_WEBSERVICE_PORT``
  Port for the web service (default: 3000).

``GTOPT_BIN``
  Path to the gtopt binary (default: auto-detected from build).

``GTOPT_DATA_DIR``
  Directory for job data storage (default: ``<webservice>/data``).

Installation:

To install the web service system-wide::

  cmake -S webservice -B build-web
  sudo cmake --install build-web

This installs:
- ``gtopt_websrv`` launcher script to ``bin/gtopt_websrv``
- Web service files to ``share/gtopt/webservice/``

#]=======================================================================]

cmake_minimum_required(VERSION 3.31.6)

project(gtoptWebService LANGUAGES NONE)

# ---- Configuration ----

set(GTOPT_WEBSERVICE_PORT "3000" CACHE STRING "Web service port")
set(GTOPT_DATA_DIR "" CACHE PATH "Data directory for job storage")

find_program(NPM_EXECUTABLE npm REQUIRED)
find_program(NODE_EXECUTABLE node REQUIRED)

set(WEBSERVICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- Install dependencies ----

add_custom_target(webservice-install
  COMMAND ${NPM_EXECUTABLE} install
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Installing web service dependencies"
)

# ---- Build ----

add_custom_target(webservice-build
  COMMAND ${NPM_EXECUTABLE} run build
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Building web service for production"
  DEPENDS webservice-install
)

# ---- Start (development) ----

add_custom_target(webservice-dev
  COMMAND ${CMAKE_COMMAND} -E env
    "PORT=${GTOPT_WEBSERVICE_PORT}"
    ${NPM_EXECUTABLE} run dev
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Starting web service in development mode on port ${GTOPT_WEBSERVICE_PORT}"
)

# ---- Start (production) ----

add_custom_target(webservice-start
  COMMAND ${CMAKE_COMMAND} -E env
    "PORT=${GTOPT_WEBSERVICE_PORT}"
    ${NPM_EXECUTABLE} run start
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Starting web service in production mode on port ${GTOPT_WEBSERVICE_PORT}"
  DEPENDS webservice-build
)

# ---- Install ----

# Install webservice files
install(DIRECTORY "${WEBSERVICE_DIR}/"
  DESTINATION share/gtopt/webservice
  FILES_MATCHING
  PATTERN "*.js"
  PATTERN "*.ts"
  PATTERN "*.tsx"
  PATTERN "*.json"
  PATTERN "*.md"
  PATTERN "src/*"
  PATTERN "public/*"
  PATTERN "test/*"
  PATTERN "node_modules" EXCLUDE
  PATTERN ".next" EXCLUDE
  PATTERN "data" EXCLUDE
  PATTERN "gtopt_websrv.sh" EXCLUDE  # Handled separately below
  PATTERN "gtopt_websrv.bat" EXCLUDE  # Handled separately below
)

# Install the gtopt_websrv launcher script (Unix-like systems)
if(UNIX)
  install(
    PROGRAMS "${WEBSERVICE_DIR}/gtopt_websrv.sh"
    DESTINATION bin
    RENAME gtopt_websrv
  )
endif()

# Install the gtopt_websrv launcher script (Windows)
if(WIN32)
  install(
    PROGRAMS "${WEBSERVICE_DIR}/gtopt_websrv.bat"
    DESTINATION bin
    RENAME gtopt_websrv.bat
  )
endif()

# Post-install message
install(CODE "
  message(STATUS \"To run the gtopt web service:\")
  message(STATUS \"  1. Install Node.js dependencies:\")
  message(STATUS \"     cd \${CMAKE_INSTALL_PREFIX}/share/gtopt/webservice\")
  message(STATUS \"     npm install --production\")
  message(STATUS \"     npm run build\")
  message(STATUS \"  2. Run the gtopt_websrv command:\")
  message(STATUS \"     \${CMAKE_INSTALL_PREFIX}/bin/gtopt_websrv\")
  message(STATUS \"  Or set environment variables:\")
  message(STATUS \"     GTOPT_BIN=\${CMAKE_INSTALL_PREFIX}/bin/gtopt gtopt_websrv\")
  message(STATUS \"     PORT=8080 gtopt_websrv\")
")
