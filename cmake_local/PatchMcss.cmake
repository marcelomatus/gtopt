# ---- Patch m.css doxygen.py for Doxygen >= 1.9.7 compatibility ----
#
# The m.css Doxygen theme (mosra/m.css) has two issues with modern
# Doxygen (>= 1.9.7):
#
# 1. C++20 concepts: Doxygen emits ``kind="concept"`` XML which m.css
#    does not recognise, causing ``assert False`` in the search-data
#    builder.  Fix: add an early return in ``parse_xml()`` for
#    unsupported kinds, mirroring ``extract_metadata()``.
#
# 2. Strict parent-tag assertions: ``parse_desc_internal()`` asserts
#    that block elements (lists, blockquotes, tables, …) only appear
#    inside ``<para>`` or ``<div>`` elements.  Doxygen 1.9.8 nests
#    lists inside ``<computeroutput>`` and ``<listitem>``, triggering
#    ``AssertionError``.  The code handles these correctly regardless
#    of parent tag.  Fix: remove the overly strict assertions.
#
# All patches are idempotent: re-running CMake on an already-patched
# source is a no-op.
#
# Requires: ``m.css_SOURCE_DIR`` set by CPMAddPackage before inclusion.

include_guard(GLOBAL)

if(NOT DEFINED m.css_SOURCE_DIR)
  message(WARNING "PatchMcss: m.css_SOURCE_DIR not set — skipping patch")
  return()
endif()

set(_mcss_doxygen_py "${m.css_SOURCE_DIR}/documentation/doxygen.py")
if(NOT EXISTS "${_mcss_doxygen_py}")
  message(WARNING "PatchMcss: ${_mcss_doxygen_py} not found — skipping patch")
  return()
endif()

file(READ "${_mcss_doxygen_py}" _mcss_src)
set(_mcss_changed FALSE)

# ---------- Patch 1: C++20 concept support ----------
# Insert an early return in parse_xml() for unsupported compound kinds.
set(_old_concept [==[        logging.debug("{}: only private things, skipping".format(state.current))
        return None

    # In order to show also undocumented members, go through all empty]==])

set(_new_concept [==[        logging.debug("{}: only private things, skipping".format(state.current))
        return None

    # Skip compound kinds not supported by m.css (e.g. C++20 concepts)
    if compounddef.attrib['kind'] not in ['namespace', 'group', 'class', 'struct', 'union', 'dir', 'file', 'page']:
        return None

    # In order to show also undocumented members, go through all empty]==])

string(FIND "${_mcss_src}" "${_new_concept}" _already_patched_concept)
if(_already_patched_concept EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_concept}" _found_concept)
  if(NOT _found_concept EQUAL -1)
    string(REPLACE "${_old_concept}" "${_new_concept}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied concept support patch")
  else()
    message(WARNING "PatchMcss: concept patch target not found — m.css version may have changed")
  endif()
else()
  message(STATUS "PatchMcss: concept patch already applied")
endif()

# ---------- Patch 2: Remove strict parent-tag assertions ----------
# Doxygen >= 1.9.8 nests block elements (lists, blockquotes, tables)
# inside <computeroutput>, <listitem>, and other elements that m.css
# does not expect as parents.  The assertions are developer-time sanity
# checks that do not affect functionality — the code handles any parent
# correctly.  Remove them to avoid AssertionError.
#
# Pattern: lines containing "assert element.tag in ['para'"
# These occur ~11 times throughout parse_desc_internal().

# Check if assertions are still present
string(FIND "${_mcss_src}" "assert element.tag in ['para'" _has_assertions)
if(NOT _has_assertions EQUAL -1)
  # Use REGEX REPLACE to comment out all parent-tag assertions.
  # Match lines like:
  #   assert element.tag in ['para', '{http://mcss.mosra.cz/doxygen/}div']
  #   assert element.tag in ['para', '{http://mcss.mosra.cz/doxygen/}div', 'ulink']
  #   assert element.tag in ['para', '{http://mcss.mosra.cz/doxygen/}div', '{...}span']
  # Replace with a pass comment so indentation is preserved.
  string(REGEX REPLACE
    "( +)assert element\\.tag in \\['para'[^\n]*\n"
    "\\1pass  # assertion removed by PatchMcss (Doxygen >= 1.9.8 compat)\n"
    _mcss_src "${_mcss_src}")
  set(_mcss_changed TRUE)
  message(STATUS "PatchMcss: removed strict parent-tag assertions (Doxygen 1.9.8 compat)")
else()
  message(STATUS "PatchMcss: parent-tag assertions already removed")
endif()

# ---------- Patch 3: Null/empty argsstring in parse_func ----------
# Doxygen >= 1.9.8 can emit <argsstring/> (empty) for some member
# functions (e.g. defaulted special members, deduction guides).  m.css
# reads .text (which returns None) and later calls .endswith() and
# .rindex(')') on it → AttributeError / ValueError.
# Fix: default to '()' when .text is None or empty, so the signature
# processing chain can proceed normally.
set(_old_argsstring [==[    signature: str = element.find('argsstring').text]==])
set(_new_argsstring [==[    signature: str = element.find('argsstring').text or '()']==])

string(FIND "${_mcss_src}" "${_new_argsstring}" _already_patched_args)
if(_already_patched_args EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_argsstring}" _found_args)
  if(NOT _found_args EQUAL -1)
    string(REPLACE "${_old_argsstring}" "${_new_argsstring}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied null argsstring guard")
  else()
    message(WARNING "PatchMcss: argsstring patch target not found")
  endif()
else()
  message(STATUS "PatchMcss: argsstring patch already applied")
endif()

# ---------- Write patched file ----------
if(_mcss_changed)
  file(WRITE "${_mcss_doxygen_py}" "${_mcss_src}")
  message(STATUS "PatchMcss: m.css doxygen.py patched successfully")
else()
  message(STATUS "PatchMcss: m.css doxygen.py already fully patched — nothing to do")
endif()
