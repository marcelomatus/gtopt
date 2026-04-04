#[=======================================================================[.rst:
FastLinker
----------

Detects and uses lld (LLVM's linker) when available, which is 2-3x faster
than the default GNU ld for large binaries.

Usage::

  include(FastLinker)

This module is safe to include unconditionally — it falls back to the
default linker when lld is not found.  Disable with
``-DGTOPT_FAST_LINKER=OFF``.

Note: mold is intentionally not used because it requires all linked
static libraries (including system-installed ones like libspdlog.a) to be
compiled with ``-fPIC``, which is not guaranteed.  lld is more lenient
and works with non-PIC static libraries.

#]=======================================================================]

option(GTOPT_FAST_LINKER "Use lld linker when available" ON)

if(GTOPT_FAST_LINKER AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  find_program(LLD_LINKER ld.lld)

  if(LLD_LINKER)
    add_link_options("-fuse-ld=lld")
    message(STATUS "Using lld linker: ${LLD_LINKER}")
  else()
    message(STATUS "Fast linker: lld not found, using default linker")
  endif()
endif()
