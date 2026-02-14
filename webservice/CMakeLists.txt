#[=======================================================================[.rst:
WebService
----------

CMake configuration for building and installing the gtopt web service.

The web service is a Next.js application that provides a REST API for
uploading gtopt input cases, running the solver, and downloading results.

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
    "GTOPT_BIN=$<IF:$<BOOL:${GTOPT_BIN}>,${GTOPT_BIN},$<TARGET_FILE:gtoptStandalone>>"
    "PORT=${GTOPT_WEBSERVICE_PORT}"
    ${NPM_EXECUTABLE} run dev
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Starting web service in development mode on port ${GTOPT_WEBSERVICE_PORT}"
)

# ---- Start (production) ----

add_custom_target(webservice-start
  COMMAND ${CMAKE_COMMAND} -E env
    "GTOPT_BIN=$<IF:$<BOOL:${GTOPT_BIN}>,${GTOPT_BIN},$<TARGET_FILE:gtoptStandalone>>"
    "PORT=${GTOPT_WEBSERVICE_PORT}"
    ${NPM_EXECUTABLE} run start
  WORKING_DIRECTORY "${WEBSERVICE_DIR}"
  COMMENT "Starting web service in production mode on port ${GTOPT_WEBSERVICE_PORT}"
  DEPENDS webservice-build
)

# ---- Install ----

install(DIRECTORY "${WEBSERVICE_DIR}/"
  DESTINATION share/gtopt/webservice
  PATTERN "node_modules" EXCLUDE
  PATTERN ".next" EXCLUDE
  PATTERN "data" EXCLUDE
)

install(CODE "
  message(STATUS \"To run the gtopt web service:\")
  message(STATUS \"  cd \${CMAKE_INSTALL_PREFIX}/share/gtopt/webservice\")
  message(STATUS \"  npm install --production\")
  message(STATUS \"  npm run build\")
  message(STATUS \"  GTOPT_BIN=\${CMAKE_INSTALL_PREFIX}/bin/gtopt npm run start\")
")
