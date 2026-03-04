#[=======================================================================[.rst:
DefaultInstallPrefix
--------------------

Sets ``CMAKE_INSTALL_PREFIX`` to ``$ENV{HOME}/.local`` for non-root users
on Unix when no explicit installation prefix has been provided.

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

When the conditions hold the prefix is set to ``$ENV{HOME}/.local``,
which follows the `XDG Base Directory Specification`_ for user-writable
installs:

* Binaries  → ``$HOME/.local/bin``
* Data      → ``$HOME/.local/share``
* Libraries → ``$HOME/.local/lib``

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
    set(CMAKE_INSTALL_PREFIX
        "$ENV{HOME}/.local"
        CACHE PATH
        "Installation prefix (defaults to ~/.local for non-root user)"
        FORCE
    )
    message(STATUS
      "Non-root install: CMAKE_INSTALL_PREFIX set to ${CMAKE_INSTALL_PREFIX}"
    )
  endif()
  unset(_default_prefix_uid)
  unset(_id_result)
endif()
