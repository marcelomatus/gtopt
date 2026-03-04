#[=======================================================================[.rst:
DefaultInstallPrefix
--------------------

Sets ``CMAKE_INSTALL_PREFIX`` to the Python user-base directory (e.g.
``~/.local``) for non-root users on Unix when no explicit installation
prefix has been provided.

Include this module right after the ``project()`` call in any
``CMakeLists.txt`` that defines ``install()`` rules, so that a
non-privileged user who runs ``cmake --install <build>`` (without
``--prefix``) gets a writable location by default instead of
``/usr/local``.

The default is only changed when **all three** conditions hold:

1. ``CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT`` is ``TRUE``
   (the user has *not* passed ``-DCMAKE_INSTALL_PREFIX=...`` or
   ``--prefix`` on the command line).
2. The platform is Unix (Linux, macOS, BSD, …).
3. The effective UID of the configuring process is **non-zero** (not root).

When the conditions hold the prefix is determined as follows:

1. **Python user-base** – ``python -m site --user-base`` is run first.
   This returns the platform-specific user install base (e.g.
   ``~/.local`` on Linux, ``~/Library/Python/X.Y`` on macOS).
2. **``$HOME/.local`` fallback** – used when Python is unavailable or
   the command fails.

The resulting directory follows the `XDG Base Directory Specification`_
for user-writable installs:

* Binaries  → ``<prefix>/bin``
* Data      → ``<prefix>/share``
* Libraries → ``<prefix>/lib``

.. _XDG Base Directory Specification:
   https://specifications.freedesktop.org/basedir-spec/latest/

#]=======================================================================]

include_guard(GLOBAL)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND UNIX)
  # Determine the effective UID using the POSIX 'id' utility.
  execute_process(
    COMMAND id -u
    OUTPUT_VARIABLE _default_prefix_uid
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _id_result
    ERROR_QUIET
  )
  # Proceed only when 'id -u' succeeded and returned a valid decimal number.
  if(_id_result EQUAL 0
     AND _default_prefix_uid MATCHES "^[0-9]+$"
     AND NOT _default_prefix_uid EQUAL 0)

    # Try Python first: "python -m site --user-base" returns the
    # platform-appropriate user install base (e.g. ~/.local on Linux,
    # ~/Library/Python/X.Y on macOS).
    set(_python_site_result -1)
    find_program(_default_prefix_python NAMES python3 python)
    if(_default_prefix_python)
      execute_process(
        COMMAND "${_default_prefix_python}" -m site --user-base
        OUTPUT_VARIABLE _python_user_base
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _python_site_result
        ERROR_QUIET
      )
    endif()

    if(_python_site_result EQUAL 0 AND _python_user_base)
      set(_default_prefix_path "${_python_user_base}")
    else()
      # Fall back to the XDG default when Python is unavailable or fails.
      set(_default_prefix_path "$ENV{HOME}/.local")
    endif()

    set(CMAKE_INSTALL_PREFIX
        "${_default_prefix_path}"
        CACHE PATH
        "Installation prefix (defaults to Python user-base for non-root user)"
        FORCE
    )
    message(STATUS
      "Non-root install: CMAKE_INSTALL_PREFIX set to ${CMAKE_INSTALL_PREFIX}"
    )

    unset(_default_prefix_path)
    unset(_python_user_base)
    unset(_python_site_result)
    unset(_default_prefix_python CACHE)
  endif()
  unset(_default_prefix_uid)
  unset(_id_result)
endif()
