"""Add the crafting recipe of every mod to data/parts.csv (columns recipe, perks).

Usage: gen_recipes.py <Fallout4.esm>
recipe = "CMPO:count;..." from the mod's COBJ (FVPA), perks = "PERK;..." from its HasPerk conditions.
The game's own workbench uses the same COBJ: the pegboard crafts with the same costs.
"""
import csv, os, struct, sys, importlib.util
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('gp', os.path.join(ROOT, 'tools', 'gen_parts.py'))
src = open(spec.origin).read().split('\nr=parse(')[0]   # only the reader functions
ns = {}
exec(src, ns)
R = ns['parse'](sys.argv[1], want=(b'COBJ',))
HAS_PERK = 448
recipes = {}
for fid, (t, s) in R.items():
    cn = [d for k, d in s if k == 'CNAM']
    if not cn:
        continue
    made = struct.unpack('<I', cn[0][:4])[0]
    comps = []
    for f in (d for k, d in s if k == 'FVPA'):
        for i in range(0, len(f) - 7, 8):
            form, count = struct.unpack_from('<II', f, i)
            comps.append('%06X:%d' % (form, count))
    perks = []
    for d in (d for k, d in s if k == 'CTDA'):
        if struct.unpack_from('<H', d, 8)[0] == HAS_PERK:
            perks.append('%06X' % struct.unpack_from('<I', d, 12)[0])
    recipes.setdefault(made, (';'.join(comps), ';'.join(perks)))
path = os.path.join(ROOT, 'data', 'parts.csv')
rows = list(csv.DictReader(open(path, newline='')))
fields = list(rows[0].keys())
for extra in ('recipe', 'perks'):
    if extra not in fields:
        fields.append(extra)
found = 0
for r in rows:
    rec = recipes.get(int(r['omod'], 16))
    r['recipe'], r['perks'] = rec if rec else ('', '')
    found += rec is not None
w = csv.DictWriter(open(path, 'w', newline=''), fieldnames=fields, lineterminator='\n')
w.writeheader()
w.writerows(rows)
print(len(rows), 'rows,', found, 'with a recipe')
