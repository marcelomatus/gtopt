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

To install the web service::

  cmake -S webservice -B build-web
  cmake --install build-web

The install step always uses the npm found at **configure time**
(``${NPM_EXECUTABLE}``).  The absolute path is baked into
``cmake_install.cmake`` so it is immune to the PATH-stripping that
``sudo`` applies by default, making both ``cmake --install`` (as a
regular user) and ``sudo cmake --install`` (system-wide) work
correctly with the same npm.

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
  PATTERN "*.mjs"
  PATTERN "*.ts"
  PATTERN "*.tsx"
  PATTERN "*.json"
  PATTERN "*.md"
  PATTERN "*.css"  # CSS files
  PATTERN "*.service"  # systemd service file
  PATTERN "*.sh"  # test scripts
  PATTERN "src/*"
  PATTERN "public/*"
  PATTERN "CMakeLists.txt" EXCLUDE
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

# Post-install: verify npm and build the webservice in the destination directory
install(CODE "
  set(_ws_dir \"\${CMAKE_INSTALL_PREFIX}/share/gtopt/webservice\")

  # Use the npm found at configure time.  Its absolute path is baked in
  # here, so it is immune to the PATH-stripping that sudo applies by default.
  # Both plain 'cmake --install' and 'sudo cmake --install' therefore use the
  # same npm, producing a predictable installation target.
  set(_npm_exe \"${NPM_EXECUTABLE}\")
  if(NOT _npm_exe)
    message(WARNING \"npm was not found on this system. \"
      \"Please install Node.js/npm and then run manually:\\n\"
      \"  cd \${_ws_dir}\\n\"
      \"  npm install\\n\"
      \"  npm run build\")
  else()
    message(STATUS \"Using npm: \${_npm_exe}\")

    # npm install
    message(STATUS \"Running npm install in \${_ws_dir} ...\")
    execute_process(
      COMMAND \${_npm_exe} install
      WORKING_DIRECTORY \${_ws_dir}
      RESULT_VARIABLE _npm_install_rc
    )
    if(NOT _npm_install_rc EQUAL 0)
      message(WARNING \"npm install failed (exit code \${_npm_install_rc}). \"
        \"Please run manually:\\n\"
        \"  cd \${_ws_dir}\\n\"
        \"  npm install\\n\"
        \"  npm run build\")
    else()
      # npm run build
      message(STATUS \"Running npm run build in \${_ws_dir} ...\")
      execute_process(
        COMMAND \${_npm_exe} run build
        WORKING_DIRECTORY \${_ws_dir}
        RESULT_VARIABLE _npm_build_rc
      )
      if(NOT _npm_build_rc EQUAL 0)
        message(WARNING \"npm run build failed (exit code \${_npm_build_rc}). \"
          \"Please run manually:\\n\"
          \"  cd \${_ws_dir}\\n\"
          \"  npm run build\")
      else()
        message(STATUS \"Web service built successfully in \${_ws_dir}\")
      endif()
    endif()
  endif()

  message(STATUS \"To run the gtopt web service:\")
  message(STATUS \"  \${CMAKE_INSTALL_PREFIX}/bin/gtopt_websrv\")
  message(STATUS \"Or set environment variables:\")
  message(STATUS \"  GTOPT_BIN=\${CMAKE_INSTALL_PREFIX}/bin/gtopt gtopt_websrv\")
  message(STATUS \"  PORT=8080 gtopt_websrv\")
")
