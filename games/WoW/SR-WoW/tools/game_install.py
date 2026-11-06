#!/usr/bin/env python3
"""The game's installation as the build reads it: the executable at its top and the archives (MPQ) in Data/ (and in Data/<language>/).
One module for every step of the build that makes a file from the game:

  inst = Installation('data')          # a folder; the build setting is game=PATH
  inst.build, inst.version             # from the version resource of the executable: 12340, '3.3.5a'
  inst.read('Interface\\\\GlueXML\\\\GlueXML.toc')   # the file from the first archive that holds it (newest patch first), or GameError
  inst.stamp()                         # the names, sizes and times of the archives: what a step depends on (the content is never read for that)

An archive may be stored as numbered parts (common.MPQ.000, common.MPQ.001, ...); the loose files of the installation are not read.
usage: game_install.py PATH [FILE...]   (checks the installation; writes the files to stdout)"""
import os, sys, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mpq


class GameError(Exception):
    pass


def _data(*names):
    return ['Data/' + name + '.MPQ' for name in names]


# The versions of the game the port is built for. `required`: the archives without which the version cannot start (web/datasource.js checks them at
# the Start; `{locale}` stands for the language). `order`: where a file is looked for, the first archive that holds it giving it (the newest patch first,
# the language's archives before the common ones of the same level).
VERSIONS = {
    12340: {
        'name': '3.3.5a',
        'required': _data('common', 'common-2', 'expansion', 'lichking', 'patch', 'patch-2', 'patch-3') +
                    ['Data/{locale}/' + name + '-{locale}.MPQ' for name in ['base', 'locale', 'speech', 'expansion-locale', 'expansion-speech', 'lichking-locale',
                                                                           'lichking-speech', 'patch']] +
                    ['Data/{locale}/patch-{locale}-2.MPQ', 'Data/{locale}/patch-{locale}-3.MPQ'],
        'order': ['Data/{locale}/patch-{locale}-3.MPQ', 'Data/patch-3.MPQ', 'Data/{locale}/patch-{locale}-2.MPQ', 'Data/patch-2.MPQ',
                  'Data/{locale}/patch-{locale}.MPQ', 'Data/patch.MPQ', 'Data/{locale}/lichking-locale-{locale}.MPQ', 'Data/lichking.MPQ',
                  'Data/{locale}/expansion-locale-{locale}.MPQ', 'Data/expansion.MPQ', 'Data/{locale}/locale-{locale}.MPQ', 'Data/{locale}/base-{locale}.MPQ',
                  'Data/common-2.MPQ', 'Data/common.MPQ', 'Data/{locale}/lichking-speech-{locale}.MPQ', 'Data/{locale}/expansion-speech-{locale}.MPQ',
                  'Data/{locale}/speech-{locale}.MPQ'],
        'login_list': True,
    },
}

SETTING_HELP = 'The build setting game=PATH names the folder of the game installation (the executable at its top, the archives in Data/); the default is data/'


def exe_build(path):
    """The build number of an executable (the last number of the FileVersion of its version resource), 0 when it has none."""
    import pefile
    pe = pefile.PE(path, fast_load=False)
    build = 0
    for file_info in getattr(pe, 'FileInfo', []):
        for info in file_info:
            for table in getattr(info, 'StringTable', []):
                parts = table.entries.get(b'FileVersion', b'').decode('latin-1').replace(' ', '').split(',')
                if len(parts) == 4 and parts[3].isdigit():
                    build = int(parts[3])
    pe.close()
    return build


def find_exe(root):
    """The path of WoW.exe in the installation (any capitalization), or None."""
    if os.path.isdir(root):
        for name in sorted(os.listdir(root)):
            if name.lower() == 'wow.exe' and os.path.isfile(os.path.join(root, name)):
                return os.path.join(root, name)
    return None


class Installation:
    def __init__(self, root, locale=None):
        self.root = root
        self.exe = find_exe(root)
        if self.exe is None:
            raise GameError('the game installation was not found: %s has no WoW.exe (tried: %s). %s' % (root, os.path.abspath(root), SETTING_HELP))
        if not os.path.isdir(os.path.join(root, 'Data')):
            raise GameError('the game installation is incomplete: %s has no Data folder with the archives. %s' % (os.path.abspath(root), SETTING_HELP))
        self.build = exe_build(self.exe)
        if self.build not in VERSIONS:
            raise GameError('%s is the build %d of the game, which this port does not know (it knows %s)' % (
                self.exe, self.build, ', '.join('%d (%s)' % (b, v['name']) for b, v in sorted(VERSIONS.items()))))
        self.info = VERSIONS[self.build]
        self.version = self.info['name']
        uses_locale = any('{locale}' in n for n in self.info['required'])
        self.locale = (locale or self._find_locale()) if uses_locale else None
        self._archives = {}

    def _find_locale(self):
        config = os.path.join(self.root, 'WTF', 'Config.wtf')
        if os.path.isfile(config):
            m = re.search(r'^\s*SET\s+locale\s+"([^"]+)"', open(config, errors='replace').read(), re.M | re.I)
            if m:
                return m.group(1)
        data = os.path.join(self.root, 'Data')
        found = sorted(d for d in os.listdir(data) if os.path.isdir(os.path.join(data, d)) and
                       any(re.search(r'-%s\.MPQ(\.\d+)?$' % re.escape(d), f, re.I) for f in os.listdir(os.path.join(data, d))))
        if len(found) == 1:
            return found[0]
        raise GameError('the language of the installation %s is not known: its WTF/Config.wtf has no SET locale line and Data/ has %s language folders (%s). '
                        'Give the build setting locale=NAME' % (os.path.abspath(self.root), 'no' if not found else 'several', ', '.join(found) or '-'))

    def _path(self, name):
        return os.path.join(self.root, *name.replace('{locale}', self.locale or '').split('/'))

    def required(self):
        """The archives the version cannot start without, with `{locale}` filled in (names as in Data/...)."""
        return [n.replace('{locale}', self.locale or '') for n in self.info['required']]

    def check(self):
        """Raises GameError naming the archives that are missing."""
        missing = [n for n in self.required() if not mpq.part_paths(self._path(n))]
        if missing:
            raise GameError('the game installation %s lacks archives of the version %s: %s. %s' % (
                os.path.abspath(self.root), self.version, ', '.join(missing), SETTING_HELP))

    def archive_names(self):
        """The archives that exist, in the order a file is looked up (names as in Data/...)."""
        names = [n.replace('{locale}', self.locale or '') for n in self.info['order']]
        return [n for n in names if mpq.part_paths(self._path(n))]

    def archives(self):
        """[(name, MPQ)] in the lookup order; an archive is opened (its tables read) the first time."""
        result = []
        for name in self.archive_names():
            if name not in self._archives:
                self._archives[name] = mpq.MPQ(self._path(name))
            result.append((name, self._archives[name]))
        return result

    def find(self, name):
        """(archive name, bytes) of the file from the first archive that holds it, or None."""
        for archive, m in self.archives():
            data = m.read(name)
            if data is not None:
                return archive, data
        return None

    def read(self, name):
        found = self.find(name)
        if found is None:
            raise GameError('the file %s is in none of the archives of the installation %s (searched: %s)' % (
                name, os.path.abspath(self.root), ', '.join(self.archive_names())))
        return found[1]

    def stamp(self):
        """The name, size and time of change of every file of the archives the lookup uses, one line each: a step depends on this instead of the content;
        the executable and the version go with it."""
        lines = ['%s %s %d' % (self.version, self.locale, self.build)]
        for name in self.archive_names():
            for path in mpq.part_paths(self._path(name)):
                st = os.stat(path)
                lines.append('%s %d %d' % (os.path.relpath(path, self.root), st.st_size, st.st_mtime_ns))
        return '\n'.join(lines)


def open_installation(root, locale=None, check=True):
    """The Installation of the folder `root`; its archives are checked unless `check` is false."""
    inst = Installation(root, locale or None)
    if check:
        inst.check()
    return inst


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    try:
        inst = open_installation(sys.argv[1])
        print('%s (build %d, locale %s): %d archives' % (inst.version, inst.build, inst.locale, len(inst.archive_names())), file=sys.stderr)
        for n in sys.argv[2:]:
            sys.stdout.buffer.write(inst.read(n))
    except GameError as e:
        sys.exit('error: %s' % e)
