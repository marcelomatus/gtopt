# ---- Patch m.css to skip C++20 concepts (unsupported) ----
#
# m.css has zero concept support.  Doxygen generates compound XML files for
# C++ concepts (kind="concept") that trigger an AssertionError in parse_xml().
#
# This module patches m.css's doxygen.py at configure time to early-return
# from both extract_metadata() and parse_xml() when the compound kind is
# "concept".  The patch is idempotent: it checks for a marker comment before
# writing, so CPM cache hits are safe.
#
# Usage (after CPMAddPackage for m.css):
#   include(PatchMcss)
#   patch_mcss_skip_concepts("${m.css_SOURCE_DIR}")

function(patch_mcss_skip_concepts MCSS_SOURCE_DIR)
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

  # Insert a concept skip right after every
  #   compound.kind = compounddef.attrib['kind']
  # (appears in both extract_metadata() and parse_xml()).
  string(REPLACE
    "compound.kind = compounddef.attrib['kind']"
    "compound.kind = compounddef.attrib['kind']\n    # [gtopt patch] skip C++20 concepts — m.css does not support them\n    if compound.kind == 'concept':\n        logging.debug(\"{}: skipping unsupported C++20 concept\".format(state.current))\n        return None"
    _src "${_src}"
  )

  file(WRITE "${_doxygen_py}" "${_src}")
  message(STATUS "PatchMcss: patched m.css doxygen.py to skip C++20 concepts")
endfunction()
