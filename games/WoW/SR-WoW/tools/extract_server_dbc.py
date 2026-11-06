#!/usr/bin/env python3
"""Extract the DBC files the AzerothCore server loads from the archives of the 3.3.5 game installation (the same archives the client reads, in its order:
tools/game_install.py).
usage: extract_server_dbc.py GAME_DIR OUT_DBC_DIR ACORE_DIR [LOCALE]"""
import sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import game_install
data, out, acore = sys.argv[1:4]
names = set()
for root, _, files in os.walk(os.path.join(acore, 'src/server')):
    for f in files:
        if f.endswith(('.cpp', '.h')):
            names |= set(re.findall(r'"([A-Za-z_0-9]+\.dbc)"', open(os.path.join(root, f), errors='replace').read()))
stores = open(os.path.join(acore, 'src/server/game/DataStores/DBCStores.cpp'), errors='replace').read()
names |= set(re.findall(r'LOAD_DBC\(\w+,\s*"([A-Za-z_0-9]+\.dbc)"', stores))   # (not every .dbc in the text: log messages say spell.dbc)
try:
    installation = game_install.open_installation(data, sys.argv[4] if len(sys.argv) > 4 else None)
except game_install.GameError as e:
    sys.exit('extract_server_dbc: %s' % e)
os.makedirs(out, exist_ok=True)
missing = []
for n in sorted(names):
    try:
        found = installation.find('DBFilesClient\\' + n)
    except Exception:
        found = None
    if found is None:
        missing.append(n)
    else:
        open(os.path.join(out, n), 'wb').write(found[1])
print(len(names) - len(missing), 'extracted;', len(missing), 'missing:', ' '.join(missing))
