\ Problem name: bad_bounds
\ Infeasible LP: variable x1 has conflicting bounds (lb > ub).
\ The static analyser detects this without a solver.

Minimize
 obj: x1 + x2

Subject To
 c1: x1 + x2 <= 100

Bounds
 5 <= x1 <= 2
 0 <= x2 <= 10

End
