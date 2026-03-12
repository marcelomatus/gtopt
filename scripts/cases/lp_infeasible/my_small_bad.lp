\ Problem name: my_small_bad
\ Simple infeasible LP: the two constraints are mutually contradictory.
\   c1 forces x1 + x2 >= 10
\   c2 forces x1 + x2 <= 5
\ No feasible assignment satisfies both constraints simultaneously.

Minimize
 obj: x1 + x2

Subject To
 c1: x1 + x2 >= 10
 c2: x1 + x2 <= 5

Bounds
 0 <= x1 <= 20
 0 <= x2 <= 20

End
