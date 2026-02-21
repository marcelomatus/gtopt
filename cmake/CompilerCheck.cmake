# ---- Compiler version requirements ----
# This project requires GCC >= 14 or Clang >= 22.
# Older compilers lack full C++23/26 support needed by the codebase.

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14")
    message(
      FATAL_ERROR
      "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old. "
      "This project requires GCC >= 14 or Clang >= 22."
    )
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "22")
    message(
      FATAL_ERROR
      "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old. "
      "This project requires GCC >= 14 or Clang >= 22."
    )
  endif()
else()
  message(
    WARNING
    "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}' "
    "(${CMAKE_CXX_COMPILER_VERSION}). "
    "This project is tested with GCC >= 14 and Clang >= 22."
  )
endif()
