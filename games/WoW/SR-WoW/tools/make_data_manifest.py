#!/usr/bin/env python3
"""Writes manifest.txt for a directory that holds the files of a WoW client (Data/*.MPQ, WoW.exe, ...).

usage: make_data_manifest.py DIRECTORY [OUTPUT]      (OUTPUT defaults to DIRECTORY/manifest.txt)

Serve DIRECTORY with any web server that answers Range requests (and adds Access-Control-Allow-Origin if it is not the
server of the page) and put its address into the Data files setting of the page. WoW.exe has to be in it: the client asks
for its size. Only Data/ and the executable are needed; Interface/, WTF/ and the like are created by the client itself.
"""
import os
import sys


def main():
    root = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "manifest.txt")
    entries = []
    # Data/ (followed when it is a link) and the executables at the top: the game's own files (WTF/,
    # Accounts/...) must not be listed, they would be read from the server and could not be written
    tops = [(root, [], sorted(n for n in os.listdir(root) if n.lower().endswith(".exe") and os.path.isfile(os.path.join(root, n))))]
    data = os.path.join(root, "Data")
    walked = os.walk(data, followlinks=True) if os.path.isdir(data) else []
    for base, dirs, files in list(tops) + list(walked):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            if relative == "manifest.txt" or os.path.abspath(path) == os.path.abspath(out):
                continue
            entries.append((relative, os.path.getsize(path)))
    with open(out, "w") as f:
        for relative, size in entries:
            f.write("%s\t%d\n" % (relative, size))
    print("%d files listed in %s" % (len(entries), out))


if __name__ == "__main__":
    main()
