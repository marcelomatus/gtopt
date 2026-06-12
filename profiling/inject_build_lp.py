"""Inject ``"lp_only": true`` into a gtopt JSON case file.

Usage::

    python3 inject_build_lp.py <input_json> <output_json>

Reads ``<input_json>``, sets ``options.lp_only = true``, and
writes the result to ``<output_json>``.  The ``options`` key is created
if it does not exist.  This helper is invoked at cmake configure time
by ``profiling/CMakeLists.txt``.
"""

import json
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_json> <output_json>")
        return 1

    input_json = sys.argv[1]
    output_json = sys.argv[2]

    try:
        with open(input_json, encoding="utf-8") as fh:
            data = json.load(fh)
    except Exception as exc:  # noqa: BLE001
        print(f"Error reading '{input_json}': {exc}", file=sys.stderr)
        return 1

    data.setdefault("options", {})
    data["options"]["lp_only"] = True

    # Override input_directory to the case directory so that external
    # Parquet/CSV data files are found relative to the case location.
    import os

    case_dir = os.path.dirname(os.path.abspath(input_json))
    data["options"]["input_directory"] = case_dir

    try:
        with open(output_json, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=2)
    except Exception as exc:  # noqa: BLE001
        print(f"Error writing '{output_json}': {exc}", file=sys.stderr)
        return 1

    print(f"Injected lp_only=true into '{output_json}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
