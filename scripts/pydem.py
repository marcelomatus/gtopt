#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data."""

from typing import Dict, List, Optional, Union


class DemandParser:
    """Parser for plpdem.dat format files containing bus demand data.
    
    Attributes:
        file_path: Path to the demand file
        demands: List of parsed demand entries
        num_bars: Number of bars in the file
    """
    
    def __init__(self, file_path: str) -> None:
        self.file_path = file_path
        self.demands = []
        self.num_bars = 0
        
    def parse(self):
        """Parse the demand file and populate the demands structure."""
        with open(self.file_path, 'r', encoding='utf-8') as f:
            # Skip initial comments and empty lines
            lines = []
            for line in f:
                line = line.strip()
                if line and not line.startswith('#'):
                    lines.append(line)
        
        try:
            idx = 0
            self.num_bars = int(lines[idx])
            idx += 1
                
            for _ in range(self.num_bars):
                # Get bus name (removing quotes and any remaining comments)
                name = lines[idx].strip("'").split('#')[0].strip()
                idx += 1
                
                # Get number of demand entries
                num_demands = int(lines[idx])
                idx += 1
                
                # Read demand entries
                demands = []
                for _ in range(num_demands):
                    parts = lines[idx].split()
                    if len(parts) < 3:
                        raise ValueError(f"Invalid demand entry at line {idx+1}")
                    month = int(parts[0])
                    stage = int(parts[1])
                    demand = float(parts[2])
                    demands.append({
                        'mes': month,
                        'etapa': stage,
                        'demanda': demand
                    })
                    idx += 1
                
            self.demands.append({
                'nombre': name,
                'demandas': demands
            })
            
    def get_demands(self):
        """Return the parsed demands structure."""
        return self.demands
        
    def get_num_bars(self):
        """Return the number of bars in the file."""
        return self.num_bars
        
    def get_demand_by_name(self, name):
        """Get demand data for a specific bus name."""
        for demand in self.demands:
            if demand['nombre'] == name:
                return demand
        return None


if __name__ == '__main__':
    pass
