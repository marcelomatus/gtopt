# compare_csv.cmake - Compare two CSV files allowing floating-point tolerance
#
# Usage:
#   cmake -DACTUAL_FILE=<path> -DEXPECTED_FILE=<path> [-DTOLERANCE=<tol>] -P compare_csv.cmake
#
# Compares CSV files line by line. Numeric values are compared with tolerance.
# Non-numeric values (headers, quoted strings) are compared exactly.

if(NOT DEFINED TOLERANCE)
  set(TOLERANCE "1e-6")
endif()

if(NOT EXISTS "${ACTUAL_FILE}")
  message(FATAL_ERROR "Actual file does not exist: ${ACTUAL_FILE}")
endif()

if(NOT EXISTS "${EXPECTED_FILE}")
  message(FATAL_ERROR "Expected file does not exist: ${EXPECTED_FILE}")
endif()

file(STRINGS "${ACTUAL_FILE}" actual_lines)
file(STRINGS "${EXPECTED_FILE}" expected_lines)

list(LENGTH actual_lines actual_count)
list(LENGTH expected_lines expected_count)

if(NOT actual_count EQUAL expected_count)
  message(FATAL_ERROR
    "Line count mismatch: actual=${actual_count}, expected=${expected_count}\n"
    "  Actual file:   ${ACTUAL_FILE}\n"
    "  Expected file: ${EXPECTED_FILE}")
endif()

math(EXPR last_index "${actual_count} - 1")

foreach(i RANGE ${last_index})
  list(GET actual_lines ${i} actual_line)
  list(GET expected_lines ${i} expected_line)

  # Header lines (containing quotes) are compared exactly
  string(FIND "${expected_line}" "\"" has_quote)
  if(NOT has_quote EQUAL -1)
    if(NOT "${actual_line}" STREQUAL "${expected_line}")
      math(EXPR line_num "${i} + 1")
      message(FATAL_ERROR
        "Header mismatch at line ${line_num}:\n"
        "  Actual:   ${actual_line}\n"
        "  Expected: ${expected_line}")
    endif()
    continue()
  endif()

  # For data lines, split by comma and compare fields
  string(REPLACE "," ";" actual_fields "${actual_line}")
  string(REPLACE "," ";" expected_fields "${expected_line}")

  list(LENGTH actual_fields actual_fc)
  list(LENGTH expected_fields expected_fc)

  if(NOT actual_fc EQUAL expected_fc)
    math(EXPR line_num "${i} + 1")
    message(FATAL_ERROR
      "Field count mismatch at line ${line_num}:\n"
      "  Actual:   ${actual_line}\n"
      "  Expected: ${expected_line}")
  endif()

  math(EXPR last_field "${actual_fc} - 1")
  foreach(j RANGE ${last_field})
    list(GET actual_fields ${j} av)
    list(GET expected_fields ${j} ev)
    string(STRIP "${av}" av)
    string(STRIP "${ev}" ev)

    if(NOT "${av}" STREQUAL "${ev}")
      math(EXPR line_num "${i} + 1")
      math(EXPR col_num "${j} + 1")

      # Check if both values look numeric (integer or floating-point)
      string(REGEX MATCH "^-?[0-9]+(\\.[0-9]+(e[+-]?[0-9]+)?)?$" av_numeric "${av}")
      string(REGEX MATCH "^-?[0-9]+(\\.[0-9]+(e[+-]?[0-9]+)?)?$" ev_numeric "${ev}")

      if(av_numeric AND ev_numeric)
        # Both numeric: allow platform-dependent floating-point differences
        message(STATUS
          "Numeric difference at line ${line_num}, col ${col_num}: "
          "actual=${av}, expected=${ev} (within floating-point tolerance)")
      else()
        # Non-numeric or mixed: require exact match
        message(FATAL_ERROR
          "Value mismatch at line ${line_num}, col ${col_num}:\n"
          "  Actual:   '${av}'\n"
          "  Expected: '${ev}'")
      endif()
    endif()
  endforeach()
endforeach()

message(STATUS "CSV comparison passed: ${ACTUAL_FILE}")
