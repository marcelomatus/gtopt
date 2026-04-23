# -*- coding: utf-8 -*-

"""Parser for plpcnfgnl.dat format files containing LNG terminal configuration.

Handles:
- File parsing and validation
- LNG terminal data structure creation
- Generator link and delivery schedule parsing

File format (tabular with comment lines starting with #)::

    # header
    # header
    NumTerminals
    # header (per terminal)
    Id  Name  VMax  Vini  CGnl  CVer  CReg  CAlm  GnlRen
    # header
    NumLinkedGenerators
    # header (per generator)
    GeneratorName  Efficiency
    # header
    NumDeliveryEntries
    # header (per delivery)
    StageIndex  DeliveryVolume

Field semantics (from ``genpdgnl.f``):

- **VMax**: maximum tank volume [m³]
- **Vini**: initial tank volume [m³]
- **CGnl**: fuel cost [$/m³] (used to compute generator variable cost)
- **CVer**: venting penalty cost [$/m³]
- **CReg**: regasification cost [$/m³]
- **CAlm**: storage holding cost [$/m³/day]
- **GnlRen**: LNG-to-gas conversion efficiency [p.u.]
- **Efficiency** (per generator): generator thermal efficiency [p.u.]

The combined heat rate for a linked generator is::

    heat_rate = 1.0 / (GnlRen × Efficiency × 3.6)   [m³_LNG/MWh]

where 3.6 is the MWh→GJ conversion factor (1 MWh = 3.6 GJ).
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class GnlParser(BaseParser):
    """Parser for plpcnfgnl.dat format files containing LNG terminal config."""

    @property
    def terminals(self) -> List[Dict[str, Any]]:
        """Return the parsed LNG terminal entries."""
        return self.get_all()

    @property
    def num_terminals(self) -> int:
        """Return the number of LNG terminal entries."""
        return len(self.terminals)

    @property
    def config(self) -> Dict[str, Any]:
        """Return the parsed data as a canonical lng.json-compatible dict.

        The returned structure has a single ``"terminals"`` key containing a
        list of terminal dicts ready for ``json.dump``.
        """
        return {"terminals": self.terminals}

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the plpcnfgnl.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The GNL configuration file is empty or malformed.")

        idx = 0
        # First non-comment line: NumTerminals
        num_terminals = self._parse_int(lines[idx].split()[0])
        idx += 1

        for _ in range(num_terminals):
            if idx >= len(lines):
                raise ValueError("Unexpected end of GNL configuration file.")

            # Terminal header:
            #   Id  Name  VMax  Vini  CGnl  CVer  CReg  CAlm  GnlRen
            fields = lines[idx].split()
            if len(fields) < 9:
                raise ValueError(
                    f"Expected 9 fields (Id Name VMax Vini CGnl CVer CReg"
                    f" CAlm GnlRen), got {len(fields)}: {lines[idx]}"
                )
            number = self._parse_int(fields[0])
            name = fields[1].strip().replace("'", "")
            vmax = self._parse_float(fields[2])
            vini = self._parse_float(fields[3])
            cgnl = self._parse_float(fields[4])
            cver = self._parse_float(fields[5])
            creg = self._parse_float(fields[6])
            calm = self._parse_float(fields[7])
            gnlren = self._parse_float(fields[8])
            idx += 1

            # Number of linked generators
            if idx >= len(lines):
                raise ValueError(f"Missing generator count for terminal '{name}'.")
            num_generators = self._parse_int(lines[idx].split()[0])
            idx += 1

            generators: List[Dict[str, Any]] = []
            for _ in range(num_generators):
                if idx >= len(lines):
                    raise ValueError(f"Missing generator data for terminal '{name}'.")
                gen_parts = lines[idx].split()
                if len(gen_parts) < 2:
                    raise ValueError(
                        f"Expected 2 fields (GeneratorName Efficiency),"
                        f" got {len(gen_parts)}: {lines[idx]}"
                    )
                gen_name = gen_parts[0].strip().replace("'", "")
                efficiency = self._parse_float(gen_parts[1])
                generators.append(
                    {
                        "name": gen_name,
                        "efficiency": efficiency,
                    }
                )
                idx += 1

            # Number of delivery entries
            if idx >= len(lines):
                raise ValueError(f"Missing delivery count for terminal '{name}'.")
            num_deliveries = self._parse_int(lines[idx].split()[0])
            idx += 1

            deliveries: List[Dict[str, Any]] = []
            for _ in range(num_deliveries):
                if idx >= len(lines):
                    raise ValueError(f"Missing delivery data for terminal '{name}'.")
                del_parts = lines[idx].split()
                if len(del_parts) < 2:
                    raise ValueError(
                        f"Expected 2 fields (StageIndex DeliveryVolume),"
                        f" got {len(del_parts)}: {lines[idx]}"
                    )
                stage = self._parse_int(del_parts[0])
                volume = self._parse_float(del_parts[1])
                deliveries.append({"stage": stage, "volume": volume})
                idx += 1

            terminal: Dict[str, Any] = {
                "number": number,
                "name": name,
                "vmax": vmax,
                "vini": vini,
                "cgnl": cgnl,
                "cver": cver,
                "creg": creg,
                "calm": calm,
                "gnlren": gnlren,
                "generators": generators,
                "deliveries": deliveries,
            }
            self._append(terminal)

    def get_terminal_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get terminal data by name."""
        return self.get_item_by_name(name)

    def get_terminal_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get terminal data by number."""
        return self.get_item_by_number(number)
