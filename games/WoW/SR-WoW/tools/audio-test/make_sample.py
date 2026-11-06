#!/usr/bin/env python3
"""Writes sample-mp3.inc, the first bytes of a music file of the game (Sacred01.mp3, in common.MPQ), for the self test of
WoW-fmod.c (scons audiotest=1; the build makes it as build/game/sample-mp3.inc).
usage: make_sample.py GAME_DIR [LOCALE] > sample-mp3.inc   (GAME_DIR: the game installation, tools/game_install.py)"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import game_install
try:
    installation = game_install.open_installation(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    data = installation.read('Sound\\Music\\Musical Moments\\Sacred\\Sacred01.mp3')[:32768]
except game_install.GameError as e:
    sys.exit('make_sample: %s' % e)
print('/* the first %d bytes of a music file of the game, for the self test */' % len(data))
print('static const unsigned char sample_mp3[%d] = {' % len(data))
for i in range(0, len(data), 24):
    print('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',')
print('};')
