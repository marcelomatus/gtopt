"""Top-level conftest for the gtopt scripts test tree.

Caps native thread pools in this process and (via inherited environment)
in every subprocess started by tests.  Pytest-xdist already runs ~20
worker processes in parallel; many tests then spawn another Python via
``subprocess.run([sys.executable, "-m", ...])`` which can in turn open
its own ThreadPoolExecutor / numpy BLAS pool sized to ``os.cpu_count()``.
On a 20-core box that compounds to 20 workers x 20 BLAS threads = 400
threads competing for 20 cores.

Forcing OMP/MKL to 1 keeps each Python interpreter single-threaded for
linear-algebra work; xdist still parallelises across tests, and
ThreadPoolExecutor instances we explicitly construct are unaffected.
``setdefault`` so a developer can still override via shell.
"""

from __future__ import annotations

import os

for _var in ("OMP_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_var, "1")
