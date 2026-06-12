#[=======================================================================[.rst:
CxxStandard
-----------

Applies the project-wide C++ language standard and compiler-extension
settings required by gtopt.

Usage::

  include(cmake_local/CxxStandard.cmake)

Effect
^^^^^^

Sets the following CMake variables (affecting all subsequently defined
targets in the calling scope):

* ``CMAKE_CXX_STANDARD``          → ``26``
* ``CMAKE_CXX_STANDARD_REQUIRED`` → ``ON``
* ``CMAKE_CXX_EXTENSIONS``        → ``OFF``
* ``CMAKE_CXX_SCAN_FOR_MODULES``  → ``OFF``

Also sets ``CMAKE_EXPORT_COMPILE_COMMANDS ON`` so that
``compile_commands.json`` is always generated (needed by clang-tidy,
clangd, and IDE integrations).

Module scanning is disabled because gtopt does not use C++20 modules.
CMake 3.28+ enables scanning by default for C++20/23/26 targets, which
adds per-file overhead and silently prevents unity builds from working
with the Ninja generator.

.. note::
   Include this module early in ``CMakeLists.txt``, after the
   ``project()`` call but before any ``add_library()`` /
   ``add_executable()`` calls, so that every C++ target inherits the
   standard settings automatically.

#]=======================================================================]

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_policy(SET CMP0167 NEW)
