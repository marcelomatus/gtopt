# Writing Style Guide — Marcelo Matus A.

Style extracted from published papers (IEEE Transactions on Power Delivery,
IEEE KPEC, Energy Procedia, IEEE PES General Meeting) for consistent
authoring of the gtopt white paper and future publications.

## Voice and Tone

- **Passive voice and impersonal constructions** throughout:
  "A novel heuristic is developed…", "The method is applied to…",
  "Optimal monitoring sets are determined…"
- Third-person references to own tools: "This paper presents results of
  applying the FESOP tool…", never "we developed" or "our tool".
- When describing contributions: "This paper presents \gtopt{}, a novel…"
- Formal, technical, concise.  No colloquial language.

## Abstract Structure (6-part pattern)

1. **Context/motivation** — 1–2 sentences on why the problem matters.
2. **Gap/challenge** — what is needed or what tradeoff exists.
3. **Contribution** — what this work does ("A novel … is developed for…").
4. **Method summary** — how it works, key technical approach.
5. **Application/validation** — where it was tested, with quantitative scope.
6. **Results** — what was found, with numbers.

## Sentence Patterns

- Complex sentences with subordinate clauses and parenthetical precision:
  "To implement an effective DTR system, it is necessary to install
  monitoring stations along the studied lines, with a tradeoff between
  accurate estimations and equipment investments."
- Technical terms introduced with brief in-line explanations.
- Quantitative specifics included directly (325 km, 30 years, four
  scenarios, 5100 m altitude).
- Favor "which" relative clauses for non-restrictive detail.

## Contribution Listing

- Formal `\enumerate` with each item as a complete noun phrase:
  "A complete mathematical formulation of the GTEP problem as a sparse
  LP/MIP, including DC optimal power flow, battery storage…
  (Section~\ref{sec:formulation})."
- Each item ends with a section cross-reference in parentheses.
- Close with "The remainder of this paper is organized as follows.
  Section~\ref{…} reviews…  Section~\ref{…} presents…"

## Vocabulary Tendencies

- "novel" for new contributions
- "heuristic" for algorithmic methods
- "tradeoff between … and …" for framing problems
- "flexibility" / "flexible" for system capabilities
- "is applied to" for validation/case studies
- "optimal" / "near-optimal" for solution quality characterization

## Formatting Conventions

- IEEE-style double-column journal format (IEEEtran).
- Two-space sentence separation after periods.
- LaTeX `\texttt{}` for software names and code identifiers.
- SI units via `\SI{}{}` or manual `\,` thin-space notation.
- All equations numbered; referenced as~\eqref{eq:label}.
- Figures referenced as Fig.~\ref{fig:label} (IEEE style).
- Tables referenced as Table~\ref{tab:label}.

## Things to Avoid

- First person ("we", "our") — use passive or "this paper".
- Informal transitions ("So", "Now", "Let's").
- Overly long paragraphs without structure (break with \paragraph{}).
- Unexplained acronyms on first use.
- Empty adjectives without quantitative support ("significant" without
  a number — prefer "10–27× speedup").

## Key Publications for Reference

1. M. Matus, D. Saez, M. Favley, C. Suazo-Martinez, "Identification of
   Critical Spans for Monitoring Systems in Dynamic Thermal Rating,"
   IEEE Trans. Power Delivery, vol. 27, no. 2, pp. 1002–1009, Apr. 2012.

2. M. P. Buitrago Villada, C. E. García Bujanda, E. Sierra Baeza,
   M. Matus A., "Optimal Expansion and Reliable Renewable Energy
   Integration in Long-Term Planning Using FESOP," Proc. IEEE KPEC,
   pp. 1–6, Apr. 2022.

3. E. Pereira-Bonvallet, S. Puschel-Lovengreen, M. Matus, R. Moreno,
   "Optimizing Hydrothermal Scheduling with Non-Convex Irrigation
   Constraints," Energy Procedia, vol. 87, pp. 132–140, 2016.

4. M. Matus, N. Caceres, S. Puschel-Lovengreen, R. Moreno, "Chebyshev
   Based Continuous Time Power System Operation Approach," Proc. IEEE
   PES General Meeting, Jul. 2015.
