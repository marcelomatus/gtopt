#[=======================================================================[.rst:
WhitePaper
----------

CMake configuration for building the gtopt white paper PDF from LaTeX
source files.

Requires ``latexmk`` and a TeX distribution (e.g. TeX Live) with
``pdflatex``, ``bibtex``, and the ``IEEEtran`` class.

Quick standalone build::

  cmake -S docs/white_paper -B build-paper
  cmake --build build-paper

Via the all/ super-project::

  cmake -S all -B build -DGTOPT_BUILD_WHITE_PAPER=ON
  cmake --build build --target white_paper

The resulting PDF is placed in the build directory as ``main.pdf``.

#]=======================================================================]

cmake_minimum_required(VERSION 3.16)

project(GtoptWhitePaper NONE)

# ---- Find latexmk ----
find_program(LATEXMK_EXECUTABLE latexmk)

if(NOT LATEXMK_EXECUTABLE)
  message(WARNING
    "latexmk not found — the 'white_paper' target will not be available.\n"
    "Install TeX Live or another LaTeX distribution:\n"
    "  sudo apt-get install -y texlive-full latexmk"
  )
  return()
endif()

message(STATUS "Found latexmk: ${LATEXMK_EXECUTABLE}")

# ---- Source files ----
set(WHITE_PAPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(MAIN_TEX "${WHITE_PAPER_DIR}/main.tex")
set(REFERENCES_BIB "${WHITE_PAPER_DIR}/references.bib")

file(GLOB SECTION_TEX_FILES "${WHITE_PAPER_DIR}/sections/*.tex")

# ---- Build target ----
add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/main.pdf"
  COMMAND "${LATEXMK_EXECUTABLE}"
          -pdf
          -interaction=nonstopmode
          -output-directory=${CMAKE_CURRENT_BINARY_DIR}
          "${MAIN_TEX}"
  DEPENDS
    "${MAIN_TEX}"
    "${REFERENCES_BIB}"
    ${SECTION_TEX_FILES}
  WORKING_DIRECTORY "${WHITE_PAPER_DIR}"
  COMMENT "Building white paper PDF with latexmk"
  VERBATIM
)

add_custom_target(white_paper
  DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/main.pdf"
)

# ---- Clean target ----
add_custom_target(white_paper_clean
  COMMAND "${LATEXMK_EXECUTABLE}"
          -C
          -output-directory=${CMAKE_CURRENT_BINARY_DIR}
          "${MAIN_TEX}"
  WORKING_DIRECTORY "${WHITE_PAPER_DIR}"
  COMMENT "Cleaning white paper build artifacts"
  VERBATIM
)
