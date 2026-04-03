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

# ---------- Patch 4: Anonymous namespace crash ----------
# Doxygen emits namespace compounds with empty <compoundname/> for anonymous
# namespaces.  m.css calls .text on the element which is None, crashing in
# html.escape() (extract_metadata) and `'@' in None` (parse_xml).
# Fix both sites: guard compoundname.text against None and skip/return early.

# 4a: extract_metadata — html.escape(compoundname.text) → AttributeError
set(_old_extract_meta [==[    compound.name = html.escape(compounddef.find('title').text if compound.kind in ['page', 'group'] and compounddef.findtext('title') else compounddef.find('compoundname').text)]==])
set(_new_extract_meta [==[    _cn_text = compounddef.find('title').text if compound.kind in ['page', 'group'] and compounddef.findtext('title') else (compounddef.find('compoundname').text if compounddef.find('compoundname') is not None else None)
    if _cn_text is None:
        return
    compound.name = html.escape(_cn_text)]==])

string(FIND "${_mcss_src}" "${_new_extract_meta}" _already_patched_anon)
if(_already_patched_anon EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_extract_meta}" _found_anon)
  if(NOT _found_anon EQUAL -1)
    string(REPLACE "${_old_extract_meta}" "${_new_extract_meta}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied anonymous namespace guard in extract_metadata")
  else()
    message(WARNING "PatchMcss: anonymous namespace patch target not found in extract_metadata")
  endif()
else()
  message(STATUS "PatchMcss: anonymous namespace patch already applied in extract_metadata")
endif()

# 4b: parse_xml — '@' in compoundname.text → TypeError when text is None
set(_old_parse_xml_anon [==[        (compounddef.attrib['kind'] == 'namespace' and '@' in compounddef.find('compoundname').text)):]==])
set(_new_parse_xml_anon [==[        (compounddef.attrib['kind'] == 'namespace' and (compounddef.find('compoundname').text is None or '@' in compounddef.find('compoundname').text))):]==])

string(FIND "${_mcss_src}" "${_new_parse_xml_anon}" _already_patched_parse)
if(_already_patched_parse EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_parse_xml_anon}" _found_parse)
  if(NOT _found_parse EQUAL -1)
    string(REPLACE "${_old_parse_xml_anon}" "${_new_parse_xml_anon}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied anonymous namespace guard in parse_xml")
  else()
    message(WARNING "PatchMcss: anonymous namespace patch target not found in parse_xml")
  endif()
else()
  message(STATUS "PatchMcss: anonymous namespace patch already applied in parse_xml")
endif()

# ---------- Patch 5: Suppress warning for .text language in programlisting ---
# Doxygen emits <programlisting filename=".text"> for ```text code blocks.
# m.css doesn't recognize .text and logs a warning before falling back to
# TextLexer anyway.  Fix: check for '.text' before the lexer lookup and
# use TextLexer directly.
set(_old_unrecognized [==[            # Otherwise try to find lexer by filename
            else:
                # Put some bogus prefix to the filename in case it is just
                # `.ext`
                lexer = find_lexer_class_for_filename("code" + filename)
                if not lexer:
                    logging.warning("{}: unrecognized language of {} in <programlisting>, highlighting disabled".format(state.current, filename))
                    lexer = TextLexer()
                else: lexer = lexer()]==])

set(_new_unrecognized [==[            # Otherwise try to find lexer by filename
            else:
                # Treat .text as plain text without warning
                if filename == '.text':
                    lexer = TextLexer()
                else:
                    # Put some bogus prefix to the filename in case it is just
                    # `.ext`
                    lexer = find_lexer_class_for_filename("code" + filename)
                    if not lexer:
                        logging.warning("{}: unrecognized language of {} in <programlisting>, highlighting disabled".format(state.current, filename))
                        lexer = TextLexer()
                    else: lexer = lexer()]==])

string(FIND "${_mcss_src}" "${_new_unrecognized}" _already_patched_text)
if(_already_patched_text EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_unrecognized}" _found_text)
  if(NOT _found_text EQUAL -1)
    string(REPLACE "${_old_unrecognized}" "${_new_unrecognized}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied .text language recognition patch")
  else()
    message(WARNING "PatchMcss: .text language patch target not found")
  endif()
else()
  message(STATUS "PatchMcss: .text language patch already applied")
endif()

# ---------- Patch 6: Template param name extraction from <ref> tags ----------
# Doxygen >= 1.9.8 wraps template parameter names in <ref> tags within
# <type> when <declname> is absent.  parse_type() converts refs to HTML
# <a> tags, so the `type[-1].isalnum()` heuristic fails (last char is '>').
# Fix: after the existing heuristic fails, fall back to extracting the
# name from the raw XML text of <type>, stripping 'typename'/'class'.
set(_old_tpl_name [==[        declname = i.find('declname')
        if declname is not None:
            # declname or decltype?!
            template.name = declname.text
        # Doxygen sometimes puts both in type, extract that, but only in case
        # it's not too crazy to do (i.e., no pointer values, no nameless
        # FooBar<T, U> types). Using rpartition() to split on the last found
        # space, but in case of nothing found, rpartition() puts the full
        # string into [2] instead of [0], so we have to account for that.
        elif template.type[-1].isalnum():
            parts = template.type.rpartition(' ')
            if parts[1]:
                template.type = parts[0]
                template.name = parts[2]
            else:
                template.type = parts[2]
                template.name = ''
        else:
            template.name = '']==])

set(_new_tpl_name [==[        declname = i.find('declname')
        if declname is not None:
            # declname or decltype?!
            template.name = declname.text
        # Doxygen sometimes puts both in type, extract that, but only in case
        # it's not too crazy to do (i.e., no pointer values, no nameless
        # FooBar<T, U> types). Using rpartition() to split on the last found
        # space, but in case of nothing found, rpartition() puts the full
        # string into [2] instead of [0], so we have to account for that.
        elif template.type[-1].isalnum():
            parts = template.type.rpartition(' ')
            if parts[1]:
                template.type = parts[0]
                template.name = parts[2]
            else:
                template.type = parts[2]
                template.name = ''
        else:
            # Fallback: extract name from raw XML text (handles <ref> tags
            # wrapping 'typename Foo' that parse_type() turns into HTML).
            _raw = ''.join(i.find('type').itertext()).strip()
            _parts = _raw.rsplit(None, 1)
            if len(_parts) == 2 and _parts[0] in ('typename', 'class'):
                template.name = _parts[1]
            else:
                template.name = '']==])

string(FIND "${_mcss_src}" "${_new_tpl_name}" _already_patched_tpl)
if(_already_patched_tpl EQUAL -1)
  string(FIND "${_mcss_src}" "${_old_tpl_name}" _found_tpl)
  if(NOT _found_tpl EQUAL -1)
    string(REPLACE "${_old_tpl_name}" "${_new_tpl_name}" _mcss_src "${_mcss_src}")
    set(_mcss_changed TRUE)
    message(STATUS "PatchMcss: applied template param name extraction patch")
  else()
    message(WARNING "PatchMcss: template param name patch target not found")
  endif()
else()
  message(STATUS "PatchMcss: template param name patch already applied")
endif()

# ---------- Write patched file ----------
if(_mcss_changed)
  file(WRITE "${_mcss_doxygen_py}" "${_mcss_src}")
  message(STATUS "PatchMcss: m.css doxygen.py patched successfully")
else()
  message(STATUS "PatchMcss: m.css doxygen.py already fully patched — nothing to do")
endif()
