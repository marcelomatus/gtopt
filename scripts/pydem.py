class DemandParser:
    """Parser for plpdem.dat format files containing bus demand data."""
    
    def __init__(self, file_path):
        self.file_path = file_path
        self.demands = []
        self.num_bars = 0
        
    def parse(self):
        """Parse the demand file and populate the demands structure."""
        with open(self.file_path, 'r', encoding='utf-8') as f:
            lines = [line.strip() for line in f if not line.startswith('#') and line.strip()]
            
        idx = 0
        self.num_bars = int(lines[idx])
        idx += 1
            
        for _ in range(self.num_bars):
            # Get bus name (removing quotes)
            name = lines[idx].strip("'")
            idx += 1
            
            # Get number of demand entries
            num_demands = int(lines[idx])
            idx += 1
            
            # Read demand entries
            demands = []
            for _ in range(num_demands):
                month, stage, demand = lines[idx].split()
                demands.append({
                    'mes': int(month),
                    'etapa': int(stage),
                    'demanda': float(demand)
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
