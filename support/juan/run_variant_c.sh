#!/usr/bin/env bash
# Variant C: only auto_scale=false (keep equilibration ruiz + scale_theta auto)
set -e
cd /home/marce/git/gtopt/support/juan
cp gtopt_iplp/gtopt_iplp.json.bak gtopt_iplp/gtopt_iplp.json  # restore
python3 -c "
import json, pathlib
p = pathlib.Path('gtopt_iplp/gtopt_iplp.json')
j = json.loads(p.read_text())
j['options']['model_options']['auto_scale'] = False
p.write_text(json.dumps(j, indent=4))
print('Variant C config:')
print('  cut_sharing_mode:', j['options']['sddp_options'].get('cut_sharing_mode'))
print('  equilibration:', j['options'].get('lp_matrix_options', {}).get('equilibration_method', 'ruiz (default)'))
print('  scale_theta:', j['options']['model_options'].get('scale_theta', 'auto'))
print('  scale_objective:', j['options']['model_options']['scale_objective'])
print('  auto_scale:', j['options']['model_options'].get('auto_scale', 'true (default)'))
"
exec /home/marce/.local/bin/gtopt gtopt_iplp/gtopt_iplp.json > /dev/null 2>&1
