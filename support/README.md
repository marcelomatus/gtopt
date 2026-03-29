# Support PLP Cases

This directory contains PLP (Programacion Lineal de la Produccion) test
cases used for validation and benchmarking of plp2gtopt conversions.

All data files (`.dat`, `.csv`) are stored as xz-compressed files to keep
the repository small. plp2gtopt reads them transparently — no manual
decompression is needed.

## Directory Structure

```
support/
  plp_5_years/          # 5-year planning horizon case
    plpblo.dat.xz
    plpcnfce.dat.xz
    plpmance.dat.xz
    ...
  plp_long_term/        # Long-term planning case
    plpaflce.dat.1.xz   # Large file split into parts
    plpaflce.dat.2.xz
    ...
    plpblo.dat.xz
    ...
```

## Adding a New PLP Case

### 1. Create the case directory

```bash
mkdir support/my_new_case
```

### 2. Copy PLP files into the directory

Copy all `.dat` and `.csv` files from your PLP case:

```bash
cp /path/to/plp/case/*.dat support/my_new_case/
cp /path/to/plp/case/*.csv support/my_new_case/
```

### 3. Compress files before committing

Run `plp_compress_case.sh` to compress all data files with xz:

```bash
scripts/plp_compress_case.sh support/my_new_case
```

This will:
- Compress each `.dat`, `.csv`, `.prn`, and `.png` file with `xz -6`
- Automatically split files that compress to more than 10 MB into numbered
  parts (e.g. `plpaflce.dat.1.xz`, `plpaflce.dat.2.xz`, ...)
- Remove the original uncompressed files

Example output:

```
Compressing 28 file(s) in support/my_new_case (xz -6, split > 10MB)...

  plpblo.dat                                   27K ->      2K  (xz)
  plpcnfce.dat                               1488K ->     26K  (xz)
  plpmance.dat                              74554K ->    750K  (xz)
  plpaflce.dat: 61234567 bytes compressed > 10485760 limit, splitting...
    plpaflce.dat.1.xz: 2917K
    plpaflce.dat.2.xz: 6404K
    ...

Done. Files are ready to commit.
```

### 4. Commit the compressed files

```bash
git add support/my_new_case/
git commit -m "feat: add my_new_case PLP test case"
```

### 5. Verify with plp2gtopt

```bash
plp2gtopt --input-dir support/my_new_case --output-dir /tmp/my_new_case_out
```

plp2gtopt automatically detects and reads `.xz` files and split parts.

## Customizing the Split Threshold

By default, files are split if the compressed size exceeds 10 MB. To change
this limit:

```bash
scripts/plp_compress_case.sh support/my_new_case --split-mb 8
```

## How Compression Works

- **Codec**: xz (LZMA2) at level 6 with multi-threaded compression (`-T0`)
- **Split files**: Large files are split by lines into parts, then each part
  is compressed independently. plp2gtopt decompresses parts in parallel
  (4 threads) and concatenates them transparently.
- **Naming convention**:
  - Single file: `plpmance.dat.xz`
  - Split file: `plpaflce.dat.1.xz`, `plpaflce.dat.2.xz`, ...

## File Size Guidelines

- **Maximum uncompressed file**: No limit (splitting handles large files)
- **Maximum compressed file/part**: 10 MB (GitHub recommended limit is 50 MB)
- **Typical compression ratio**: 10x–100x for PLP `.dat` files
