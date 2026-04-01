# latexmk configuration for gtopt white paper
$pdf_mode = 1;           # Generate PDF via pdflatex
$pdflatex = 'pdflatex -interaction=nonstopmode -synctex=1 %O %S';
$bibtex_use = 2;         # Run BibTeX when needed
$clean_ext = 'bbl run.xml synctex.gz';
