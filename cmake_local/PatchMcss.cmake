# ---- Patch m.css doxygen.py for compatibility with modern C++ / Doxygen ----
#
# m.css (pinned at a0d292ec) has several bugs when processing Doxygen XML
# generated from C++20/26 code:
#
#   1. C++20 concepts:  Doxygen emits compound XML files with kind="concept".
#      m.css has no concept support, so parse_xml() hits an AssertionError.
#      Fix: early-return when the compound kind is "concept".
#
#   2. Null argsstring:  For certain function declarations (e.g. deduction
#      guides, deleted/defaulted special members), Doxygen may emit an
#      <argsstring/> element with no text content.  m.css's parse_func()
#      unconditionally calls .endswith() on the result, producing
#      AttributeError: 'NoneType' object has no attribute 'endswith'.
#      Fix: default to an empty string when argsstring text is None.
#
# The patch is idempotent: it checks for a marker comment before writing,
# so CPM cache hits are safe.
#
# Usage (after CPMAddPackage for m.css):
#   include(PatchMcss)
#   patch_mcss("${m.css_SOURCE_DIR}")

function(patch_mcss MCSS_SOURCE_DIR)
  set(_doxygen_py "${MCSS_SOURCE_DIR}/documentation/doxygen.py")

  if(NOT EXISTS "${_doxygen_py}")
    message(WARNING "PatchMcss: ${_doxygen_py} not found — skipping patch")
    return()
  endif()

  file(READ "${_doxygen_py}" _src)

  # Only patch once (idempotent for CPM cache hits)
  string(FIND "${_src}" "[gtopt patch]" _already_patched)
  if(NOT _already_patched EQUAL -1)
    message(STATUS "PatchMcss: m.css doxygen.py already patched — nothing to do")
    return()
  endif()

  # --- Patch 1: skip C++20 concepts -----------------------------------
  # Insert an early-return right after every
  #   compound.kind = compounddef.attrib['kind']
  # (appears in both extract_metadata() and parse_xml()).
  string(REPLACE
    "compound.kind = compounddef.attrib['kind']"
    "compound.kind = compounddef.attrib['kind']\n    # [gtopt patch] skip C++20 concepts — m.css does not support them\n    if compound.kind == 'concept':\n        logging.debug(\"{}: skipping unsupported C++20 concept\".format(state.current))\n        return None"
    _src "${_src}"
  )

  # --- Patch 2: guard against None argsstring -------------------------
  # Replace the bare .text access with a None-safe fallback.
  string(REPLACE
    "signature: str = element.find('argsstring').text"
    "# [gtopt patch] guard against None argsstring (deduction guides, etc.)\n    _argsstring_el = element.find('argsstring')\n    signature: str = _argsstring_el.text if _argsstring_el is not None and _argsstring_el.text is not None else ''"
    _src "${_src}"
  )

  file(WRITE "${_doxygen_py}" "${_src}")
  message(STATUS "PatchMcss: patched m.css doxygen.py (concepts + argsstring)")
endfunction()
