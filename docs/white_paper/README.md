# gtopt White Paper — LaTeX Source

## Target Journals (in order of preference)

1. **IEEE Transactions on Power Systems** (primary target)
   - Template: `IEEEtran.cls` (journal mode)
   - Style: Two-column, `IEEEtran` BibTeX style
   - Scope: Power system optimization, expansion planning, OPF
   - Impact Factor: ~7.3 (2024)
   - URL: https://ieee-pes.org/publications/transactions-on-power-systems/

2. **Applied Energy** (Elsevier) — alternative
   - Template: `elsarticle.cls` or `cas-dc.cls`
   - Scope: Energy conversion, optimization, system planning
   - Impact Factor: ~11.2 (2024)
   - URL: https://www.sciencedirect.com/journal/applied-energy

3. **SoftwareX** (Elsevier) — alternative (software-focused)
   - Template: `elsarticle.cls`
   - Scope: Open-source research software (max 3000 words)
   - Impact Factor: ~3.4 (2024)
   - URL: https://www.sciencedirect.com/journal/softwarex

## Directory Structure

```
docs/white_paper/
├── main.tex                  # Main document (includes all sections)
├── references.bib            # BibTeX bibliography
├── .latexmkrc                # Build configuration
├── README.md                 # This file
├── sections/
│   ├── introduction.tex      # Section I
│   ├── literature_review.tex # Section II (State of the Art)
│   ├── formulation.tex       # Section III (Mathematical Formulation)
│   ├── solution_methods.tex  # Section IV (Solution Methods)
│   ├── architecture.tex      # Section V (Software Architecture)
│   ├── validation.tex        # Section VI (Validation & Benchmarks)
│   ├── case_studies.tex      # Section VII (Case Studies — TBD)
│   └── conclusions.tex       # Section VIII (Conclusions)
├── figures/                  # Raster images (PNG, PDF)
│   └── .gitkeep
├── diagrams/                 # Source diagrams (TikZ, Mermaid)
│   └── .gitkeep
└── tables/                   # Standalone table files (if needed)
    └── .gitkeep
```

## Compilation

### Using latexmk (recommended)
```bash
cd docs/white_paper
latexmk -pdf main.tex
```

### Manual compilation
```bash
pdflatex main
bibtex main
pdflatex main
pdflatex main
```

### On Overleaf
1. Upload the entire `docs/white_paper/` directory as a new project
2. Set `main.tex` as the main document
3. Overleaf will auto-detect the IEEEtran class and compile

## Switching to an Alternative Journal Template

### For Applied Energy (Elsevier)
Replace `\documentclass[journal]{IEEEtran}` with:
```latex
\documentclass[review,3p]{elsarticle}
```
And change `\bibliographystyle{IEEEtran}` to:
```latex
\bibliographystyle{elsarticle-num}
```

### For SoftwareX (Elsevier)
Same as Applied Energy, but limit the paper to ~3000 words and focus
on software description rather than mathematical formulation.

## Notes for Collaborators

- Each section is in a separate `.tex` file under `sections/`
- Add figures to `figures/` (PDF or PNG preferred for Overleaf)
- Use `\label{}` and `\ref{}` for cross-references
- Use `\cite{}` for citations (entries in `references.bib`)
- Section `case_studies.tex` is a placeholder — user-defined cases
  will be added in a future iteration
