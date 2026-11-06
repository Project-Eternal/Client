#!/usr/bin/env python3
"""The remote file systems of the embedded AzerothCore server (served with the page from publish/server/acore/; the web server compresses them),
laid out as the server's local file system /AzerothCore (web/acore.js):
  data/data/db/world.sqlite, data/data/db/auth.sqlite   AzerothCore's databases built from its own SQL (data/sql/base + the updates since) by the
                                                        setup tool (server/acore/mysql-sqlite/tools/setup.cpp), without the *_locale rows
  data/data/dbc.pack                                    only with --dbc-pack: the DBC files (unpacked into data/dbc by the page); by default the
                                                        server reads them from the game in the page (mod-wowcontroller), and an older pack is deleted
  init/etc/worldserver.conf                             the server's defaults with the few keys read before the module configs (logs, threads)
  init/realm/N/db/characters.sqlite                     an empty characters database for each realm (1 Normal, 2 Sandbox): the first one of a new browser
patch/ (our settings and custom SQL) is in the repository: server/acore/patch/. The manifests are written by the SConstruct.
usage: pack-assets.py OUTROOT [--if-changed] [--dbc-pack]   (e.g. publish/server/acore; work files in build/server-tools)
  --if-changed: nothing is done when the inputs (AzerothCore's SQL, the shim, this script, the DBC files) are the ones of the last run"""
import hashlib, os, shutil, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, '..', '..', '..'))
acore = os.path.join(here, '..', 'azerothcore')
shim = os.path.join(here, '..', 'mysql-sqlite')
dbc_dir = os.path.join(here, '..', '..', '..', 'build', 'game', 'server', 'dbc')   # made by the build (scons dbc, tools/extract_server_dbc.py)
args = [a for a in sys.argv[1:] if not a.startswith('--')]
root = os.path.abspath(args[0])
work = os.path.join(repo, 'build', 'server-tools')
dbc_pack = '--dbc-pack' in sys.argv

# what the outputs are made of: a change of any of it makes them again
def inputs_digest():
    h = hashlib.sha1()
    rev = subprocess.run(['git', '-C', acore, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    h.update(rev.encode())
    for top in (os.path.join(acore, 'data', 'sql'), os.path.join(shim, 'src'), os.path.join(shim, 'tools')):
        for dirpath, dirs, names in os.walk(top):
            dirs.sort()
            for name in sorted(names):
                p = os.path.join(dirpath, name)
                h.update(p.encode() + str(os.path.getsize(p)).encode() + str(int(os.path.getmtime(p))).encode())
    h.update(open(__file__, 'rb').read())
    h.update(str(dbc_pack).encode())
    if dbc_pack and os.path.isdir(dbc_dir):
        for name in sorted(os.listdir(dbc_dir)):
            h.update(name.encode() + str(os.path.getsize(os.path.join(dbc_dir, name))).encode())
    return h.hexdigest()

stamp = os.path.join(root, '.pack-assets')
digest = inputs_digest()
if '--if-changed' in sys.argv and os.path.exists(stamp) and open(stamp).read() == digest:
    print('pack-assets: up to date')
    sys.exit(0)

db = os.path.join(root, 'data', 'data', 'db')
init_dbs = [os.path.join(root, 'init', 'realm', str(r), 'db') for r in (1, 2)]   # (the realms: 1 Normal, 2 Sandbox)
for d in [db, os.path.join(root, 'init', 'etc')] + init_dbs:
    os.makedirs(d, exist_ok=True)
for old in (os.path.join(root, 'data', 'db'), os.path.join(root, 'init', 'characters')):
    if os.path.isdir(old): shutil.rmtree(old)

# the databases: AzerothCore's SQL through the shim (the setup tool is built natively first)
subprocess.check_call(['bash', os.path.join(shim, 'tools', 'build-setup.sh'), work])
tmp = os.path.join(work, 'out')
subprocess.check_call([os.path.join(work, 'acore-db-setup'), acore, tmp, '--skip-locales', '--nocase'])
shutil.copyfile(os.path.join(tmp, 'world.sqlite'), os.path.join(db, 'world.sqlite'))
shutil.copyfile(os.path.join(tmp, 'auth.sqlite'), os.path.join(db, 'auth.sqlite'))
for init_db in init_dbs:
    shutil.copyfile(os.path.join(tmp, 'characters.sqlite'), os.path.join(init_db, 'characters.sqlite'))

# the server's defaults; the keys read before the module configs are loaded (logging, the thread pool) must be set here, everything else we force is in
# patch/etc/modules/mod_embedded_auth.conf (loaded after this file, its keys win)
early = {'LogsDir': '"/AzerothCore/logs"', 'Appender.Console': '1,4,0', 'LogLevel': '3', 'ThreadPool': '2'}   # Appender.Console: no colour codes (the Log pane shows them as text)
lines = open(os.path.join(acore, 'src', 'server', 'apps', 'worldserver', 'worldserver.conf.dist')).read().split('\n')
for i, line in enumerate(lines):
    key = line.split('=', 1)[0].strip()
    if '=' in line and not line.lstrip().startswith('#') and key in early:
        lines[i] = '%s = %s' % (key, early.pop(key))
lines += ['%s = %s' % item for item in early.items()]
open(os.path.join(root, 'init', 'etc', 'worldserver.conf'), 'w').write('\n'.join(lines))

# the DBC files in one pack (--dbc-pack: for a page without the game's DBCs): u32 count, then per file u16 name length, name, u32 size, bytes
pack = os.path.join(root, 'data', 'data', 'dbc.pack')
if dbc_pack:
    files = sorted(os.listdir(dbc_dir))
    # names that only appear in log messages (spell.dbc next to Spell.dbc) are dropped: the server opens the spelling of its LOAD_DBC lines
    files = [n for n in files if not (n == n.lower() and any(o != n and o.lower() == n for o in files))]
    with open(pack, 'wb') as g:
        g.write(struct.pack('<I', len(files)))
        for name in files:
            blob = open(os.path.join(dbc_dir, name), 'rb').read()
            g.write(struct.pack('<H', len(name)) + name.encode() + struct.pack('<I', len(blob)) + blob)
elif os.path.exists(pack):
    os.remove(pack)

open(stamp, 'w').write(digest)
for dirpath, _, names in os.walk(root):
    for name in sorted(names):
        if name.startswith('.') or name == 'manifest.txt' or dirpath == root: continue
        print('%-50s %8.1f MB' % (os.path.relpath(os.path.join(dirpath, name), root), os.path.getsize(os.path.join(dirpath, name)) / 1048576))
