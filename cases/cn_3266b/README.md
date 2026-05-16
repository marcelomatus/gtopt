# `cn_3266b` — Chinese Provincial 3266-Bus Test Case

A 3266-bus system based on a practical provincial grid in China, including
one year of load data and renewable generation profiles.  This is a
large-scale test case for validating gtopt's SDDP and cascade methods on
real-world-sized problems.

## Source

- **Dataset**: Zhibo Xu, "118-bus, 300-bus and 3266-bus system data",
  IEEE DataPort, DOI: 10.21227/vma9-wk20 (2025).
- **Format**: MATLAB `.mat` file with `system_para.mat` containing bus,
  generator, and line parameters; `load_renew_data.csv` with 365 days of
  hourly load and renewable profiles.

## System data

| Quantity | Value |
|----------|-------|
| Buses | 3266 |
| Generators | ~200 |
| Loads | ~2000 |
| Branches | ~4000 |
| Time horizon | 8760 hours (1 year) |

## Converting to gtopt format

The MATLAB `.mat` file must be converted to pandapower format first, then
to gtopt JSON via `pp2gtopt`:

```python
import scipy.io
import pandapower as pp

# Load MATLAB data
mat = scipy.io.loadmat("system_para.mat")

# Build pandapower network from mat data
# ... (requires mapping MATLAB structures to pandapower tables)

# Convert to gtopt JSON
from pp2gtopt.convert import convert
gtopt_case = convert(net)
```

## Generating golden files

Given the system size (~3266 buses), generating golden files requires running
pandapower DC OPF and extracting results:

```python
pp.rundcopp(net)
# Write net.res_* to cases/cn_3266b/output/ as gtopt CSV files
```

## CTest coverage

Registered via `add_e2e_case(cn_3266b, cn_3266b.json)` in
`integration_test/CMakeLists.txt`.
