#!/usr/bin/env python3
"""Writes the login screen's list of files (Interface\\GlueXML\\GlueXML.toc, a table of contents: the files of the login screen in the order the game loads
them) with the one line added that loads the port's own login script, Connect.lua (patch/Data/Interface/GlueXML/Connect.lua). The list is the game's own,
from the first archive that holds it (the newest patch); the line goes just before the game's last entry, so that file still loads last. The game's
line endings are kept. When the anchor line is not in the list (an unexpected version of the game) the tool fails instead of guessing a place.
usage: make_glue_toc.py GAME_DIR OUT_FILE [LOCALE]   (GAME_DIR: the game installation, tools/game_install.py)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_install

ANCHOR = b'## This Always Runs Last, Add New Items above it.'
ADDED = b'Connect.lua'

def with_connect(toc):
    """The list `toc` (bytes) with the added line before the anchor line."""
    lines = toc.split(b'\n')
    at = [i for i, line in enumerate(lines) if line.rstrip(b'\r') == ANCHOR]
    if len(at) != 1:
        raise game_install.GameError('the login list of the game has %d lines "%s", expected one: it is not the list of the version this port knows' % (len(at), ANCHOR.decode()))
    ending = b'\r' if lines[at[0]].endswith(b'\r') else b''
    lines.insert(at[0], ADDED + ending)
    return b'\n'.join(lines)

if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    try:
        installation = game_install.open_installation(sys.argv[1], sys.argv[3] if len(sys.argv) > 3 else None)
        result = with_connect(installation.read('Interface\\GlueXML\\GlueXML.toc'))
    except game_install.GameError as e:
        sys.exit('make_glue_toc: %s' % e)
    os.makedirs(os.path.dirname(os.path.abspath(sys.argv[2])), exist_ok=True)
    with open(sys.argv[2], 'wb') as f:
        f.write(result)
