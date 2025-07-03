cmake_minimum_required(VERSION 3.18)
project(plp2gtopt LANGUAGES Python)

# Find Python and required packages
find_package(Python REQUIRED COMPONENTS Interpreter Development)

# Install Python development tools
execute_process(
  COMMAND ${Python_EXECUTABLE} -m pip install pytest pylint ruff
  RESULT_VARIABLE python_tools_install_result
)
if(NOT python_tools_install_result EQUAL 0)
  message(WARNING "Failed to install Python tools - Python tests/linting will not run")
endif()

# Install Python package
set(PYTHON_PACKAGE_DIR "${CMAKE_INSTALL_PREFIX}/lib/python${Python_VERSION_MAJOR}.${Python_VERSION_MINOR}/site-packages")

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
  DESTINATION ${PYTHON_PACKAGE_DIR}/plp2gtopt
  FILES_MATCHING PATTERN "*.py"
  PATTERN "__pycache__" EXCLUDE
  PATTERN "tests" EXCLUDE
)

# Add pytest integration
enable_testing()
find_package(Python COMPONENTS Interpreter REQUIRED)
add_test(
  NAME plp2gtopt_tests
  COMMAND ${Python_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/tests
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
