\ Problem name: feasible_small
\ A small but feasible LP for baseline testing.

Minimize
 obj: 2 x1 + 3 x2

Subject To
 c1: x1 + x2 >= 5
 c2: x1 + x2 <= 20
 c3: x1 >= 1

Bounds
 0 <= x1 <= 10
 0 <= x2 <= 10

End
