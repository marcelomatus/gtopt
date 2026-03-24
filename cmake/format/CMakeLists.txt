# Minimal cmake project for format-checking only.
# Does not require any C++ build dependencies — just clang-format and git.
cmake_minimum_required(VERSION 3.31.6)
project(gtopt-format-check LANGUAGES NONE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../ClangFormat.cmake)
