#!/usr/bin/env python3
"""Writes DIR/manifest.txt for a remote file system of the page (data/, patch/ or init/ of the client or of a server): one
"path<TAB>size<TAB>version" line per file, paths relative to DIR with forward slashes, version = the first 16 hex digits of the file's SHA-256
(the page keys its Cache Storage on it: a changed file is fetched again, no HEAD request needed). usage: make_remote_manifest.py DIR [DIR...]"""
import hashlib, os, sys


def manifest(root):
    lines = []
    for dirpath, dirnames, names in os.walk(root, followlinks=True):
        dirnames.sort()
        for name in sorted(names):
            path = os.path.join(dirpath, name)
            relative = os.path.relpath(path, root).replace(os.sep, '/')
            if relative == 'manifest.txt':
                continue
            digest = hashlib.sha256()
            with open(path, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    digest.update(chunk)
            lines.append('%s\t%d\t%s' % (relative, os.path.getsize(path), digest.hexdigest()[:16]))
    text = ''.join(line + '\n' for line in lines)
    target = os.path.join(root, 'manifest.txt')
    if not os.path.exists(target) or open(target).read() != text:   # unchanged: the file keeps its time (no needless redeploy)
        with open(target, 'w') as f:
            f.write(text)
    return len(lines)


if __name__ == '__main__':
    for root in sys.argv[1:]:
        print('%s: %d files' % (os.path.join(root, 'manifest.txt'), manifest(root)))
