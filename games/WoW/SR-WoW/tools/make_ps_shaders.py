#!/usr/bin/env python3
"""Translates the ps_2_0 pixel shaders of the client (Shaders\\Pixel\\*.bls in the MPQs) to GLSL functions, for d3d9-webgl.
(The functions use texture2D; the wrapper compiles them as GLSL ES 3.00 with `#define texture2D texture` in front, so the table needs no regeneration.)

usage: make_ps_shaders.py GAME_DIR OUTPUT.inc [LOCALE]   (GAME_DIR: the game installation, tools/game_install.py)
The files hold the same shader compiled for ps_1_1 ... ps_2_0; the client takes the version that fits the PixelShaderVersion of the caps, and
the wrapper reports 2.0. The wrapper hashes the bytecode that CreatePixelShader gets (FNV-1a over the words from the version token to the
end token) and looks it up in the table written here. Shaders that use more than this translator knows are listed on stderr and left out
(the wrapper then draws with the fixed function pipeline)."""
import struct, sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_install

NAMES = """Desaturate FFXBox4 FFXDeath FFXGauss4 FFXGlow FFXGlowWave MapObjEnv MapObjEnvMetal MapObjExtWater0 MapObjMetal MapObjOverbright MapObjSpecular
MapObjTransDiffuse MapObjTransSpecular Model2 ocean0_s terrain1 terrain1_s terrain2 terrain2_s terrain3 terrain3_s terrain4 terrain4_s terrainp terrainp_s
terrainp_u terrainp_us FFXDeath_nvrc FFXDeath_nvts_2d FFXDeath_nvts_rect""".split()

SAMPLERS = ['u_texture', 'u_texture_lm', 'u_texture2', 'u_texture3', 'u_texture4', 'u_texture5', 'u_texture6', 'u_texture7']
SWZ = 'xyzw'
REGTYPE = {0: 'r', 1: 'v', 2: 'c', 3: 't', 8: 'oC', 9: 'oDepth', 10: 's'}


class Unsupported(Exception):
    pass


def regtype(tok):
    return ((tok >> 28) & 7) | (((tok >> 11) & 3) << 3)


def parse(words, pos):
    """-> (instructions, end position); instruction = (opcode, control, [tokens])"""
    assert words[pos] == 0xFFFF0200
    pos += 1
    out = []
    while True:
        t = words[pos]
        pos += 1
        op = t & 0xFFFF
        if op == 0xFFFF:
            return out, pos
        if op == 0xFFFE:
            pos += (t >> 16) & 0x7FFF
            continue
        n = (t >> 24) & 0xF
        out.append((op, (t >> 16) & 0xFF, list(words[pos:pos + n])))
        pos += n


def find_shaders(data):
    words = struct.unpack('<%dI' % (len(data) // 4), data[:len(data) // 4 * 4])
    found = []
    i = 0
    while i < len(words):
        if words[i] == 0xFFFF0200:
            try:
                ins, end = parse(words, i)
                found.append((words[i:end], ins))
                i = end
                continue
            except Exception:
                pass
        i += 1
    return found


def fnv1a(words):
    h = 0x811C9DC5
    for b in struct.pack('<%dI' % len(words), *words):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class Translator:
    def __init__(self, instructions):
        self.ins = instructions
        self.defs = {}
        self.temps = 0
        self.constants_used = set()
        self.samplers_used = set()

    def reg(self, tok, writing=False):
        t, n = regtype(tok), tok & 0x7FF
        if t == 0:
            self.temps = max(self.temps, n + 1)
            return 'r%d' % n
        if t == 1:
            if n > 1: raise Unsupported('v%d' % n)
            return 'v%d' % n
        if t == 2:
            if n >= 32: raise Unsupported('c%d' % n)
            return ('cd%d' % n) if n in self.defs else ('u_ps_c[%d]' % n)
        if t == 3:
            if n > 7: raise Unsupported('t%d' % n)
            return 't%d' % n
        if t == 8:
            if n != 0: raise Unsupported('oC%d' % n)
            return 'oC0'
        if t == 9:
            raise Unsupported('oDepth')
        raise Unsupported('register type %d' % t)

    def src(self, tok):
        r = self.reg(tok)
        sw = (tok >> 16) & 0xFF
        s = ''.join(SWZ[(sw >> (2 * i)) & 3] for i in range(4))
        e = r if s == 'xyzw' else '%s.%s' % (r, s)
        mod = (tok >> 24) & 0xF
        if mod == 0: return e
        if mod == 1: return '(-%s)' % e
        if mod == 6: return '(vec4(1.0) - %s)' % e
        if mod == 7: return '(%s * 2.0)' % e
        if mod == 8: return '(-(%s * 2.0))' % e
        if mod == 11: return 'abs(%s)' % e
        if mod == 12: return '(-abs(%s))' % e
        raise Unsupported('source modifier %d' % mod)

    def run(self):
        lines = []
        for op, ctl, a in self.ins:
            if op == 81:   # def
                n = a[0] & 0x7FF
                vals = ['%.9g' % struct.unpack('<f', struct.pack('<I', x))[0] for x in a[1:5]]
                self.defs[n] = 'vec4(%s)' % ', '.join(v if re.search(r'[.eEn]', v) else v + '.0' for v in vals)
        for op, ctl, a in self.ins:
            if op in (81, 31):
                continue
            if op == 66:   # texld
                d = a[0]
                coord = self.src(a[1])
                sm = a[2] & 0x7FF
                if regtype(a[2]) != 10 or sm > 7: raise Unsupported('sampler s%d' % sm)
                self.samplers_used.add(sm)
                if ctl & 1: c = '(%s).xy / (%s).w' % (coord, coord)
                else: c = '(%s).xy' % coord
                expr = 'texture2D(%s, %s, u_lod_bias)' % (SAMPLERS[sm], c)
                lines.append(self.assign(d, expr))
                continue
            if op == 65:   # texkill
                lines.append('if (any(lessThan((%s).xyz, vec3(0.0)))) discard;' % self.reg(a[0]))
                continue
            s = [self.src(x) for x in a[1:]]
            if op == 1:   e = s[0]
            elif op == 2: e = '%s + %s' % (s[0], s[1])
            elif op == 3: e = '%s - %s' % (s[0], s[1])
            elif op == 4: e = '%s * %s + %s' % (s[0], s[1], s[2])
            elif op == 5: e = '%s * %s' % (s[0], s[1])
            elif op == 6: e = 'vec4(1.0 / (%s).x)' % s[0]
            elif op == 7: e = 'vec4(inversesqrt(abs((%s).x)))' % s[0]
            elif op == 8: e = 'vec4(dot((%s).xyz, (%s).xyz))' % (s[0], s[1])
            elif op == 9: e = 'vec4(dot(%s, %s))' % (s[0], s[1])
            elif op == 10: e = 'min(%s, %s)' % (s[0], s[1])
            elif op == 11: e = 'max(%s, %s)' % (s[0], s[1])
            elif op == 12: e = 'vec4(lessThan(%s, %s))' % (s[0], s[1])
            elif op == 13: e = 'vec4(greaterThanEqual(%s, %s))' % (s[0], s[1])
            elif op == 14: e = 'vec4(exp2((%s).x))' % s[0]
            elif op == 15: e = 'vec4(log2(abs((%s).x)))' % s[0]
            elif op == 18: e = 'mix(%s, %s, %s)' % (s[2], s[1], s[0])
            elif op == 19: e = 'fract(%s)' % s[0]
            elif op == 32: e = 'vec4(pow(abs((%s).x), (%s).x))' % (s[0], s[1])
            elif op == 35: e = 'abs(%s)' % s[0]
            elif op == 36: e = '(%s) * inversesqrt(max(dot((%s).xyz, (%s).xyz), 1.0e-20))' % (s[0], s[0], s[0])
            elif op == 88: e = 'mix(%s, %s, vec4(greaterThanEqual(%s, vec4(0.0))))' % (s[2], s[1], s[0])
            elif op == 90: e = 'vec4((%s).x * (%s).x + (%s).y * (%s).y + (%s).x)' % (s[0], s[1], s[0], s[1], s[2])
            else:
                raise Unsupported('opcode %d' % op)
            lines.append(self.assign(a[0], e))
        return lines

    def assign(self, dtok, expr):
        r = self.reg(dtok, True)
        if r == 'oC0' or r.startswith('r'):
            pass
        else:
            raise Unsupported('destination ' + r)
        mask = ''.join(SWZ[i] for i in range(4) if (dtok >> (16 + i)) & 1)
        if not mask: return ''
        mod = (dtok >> 20) & 0xF
        shift = (dtok >> 24) & 0xF
        e = '(%s)' % expr
        if shift:
            e = '(%s * %s)' % (e, '%g' % (2.0 ** (shift if shift < 8 else shift - 16)))
        if mod & 1:
            e = 'clamp(%s, 0.0, 1.0)' % e
        if mask == 'xyzw': return '%s = %s;' % (r, e)
        return '%s.%s = (%s).%s;' % (r, mask, e, mask)


def main():
    game_dir, out_path = sys.argv[1], sys.argv[2]
    try:
        installation = game_install.open_installation(game_dir, sys.argv[3] if len(sys.argv) > 3 else None)
    except game_install.GameError as e:
        sys.exit('make_ps_shaders: %s' % e)
    archives = [m for _, m in installation.archives()]
    wotlk = installation.build == 12340
    names = NAMES
    if wotlk:
        # every ps_2_0 shader file the archives list
        found = {}
        for m in archives:
            try:
                listing = m.read('(listfile)').decode('latin1').split('\r\n')
            except Exception:
                continue
            for entry in listing:
                low = entry.lower()
                if low.startswith('shaders\\pixel\\ps_2_0\\') and low.endswith('.bls'):
                    found.setdefault(low, entry[len('shaders\\Pixel\\'):-4])
        names = sorted(found.values())
    shaders = {}   # hash -> (name, words, instructions)
    for name in names:
        for m in archives:
            try:
                r = m.read('Shaders\\Pixel\\%s.bls' % name)
            except Exception:
                r = None
            if r is not None:
                for words, ins in find_shaders(r):
                    shaders.setdefault(fnv1a(words), (name, words, ins))
                break
    functions, known, skipped = [], [], []
    for h, (name, words, ins) in sorted(shaders.items(), key=lambda x: x[1][0]):
        tr = Translator(ins)
        try:
            body = tr.run()
        except Unsupported as e:
            skipped.append((name, h, str(e)))
            continue
        k = len(known) + 1
        known.append((h, k, name))
        decl = ['vec4 oC0 = vec4(0.0);']
        for n, v in sorted(tr.defs.items()):
            decl.append('vec4 cd%d = %s;' % (n, v))
        if tr.temps: decl.append('vec4 %s;' % ', '.join('r%d = vec4(0.0)' % i for i in range(tr.temps)))
        functions.append((h, k, name, 'vec4 ps_%d(vec4 v0, vec4 v1, vec4 t0, vec4 t1, vec4 t2, vec4 t3, vec4 t4, vec4 t5, vec4 t6, vec4 t7) {\n    %s\n    %s\n    return oC0;\n}\n' % (
            k, '\n    '.join(decl), '\n    '.join(l for l in body if l))))
    with open(out_path, 'w') as f:
        f.write('// Generated by tools/make_ps_shaders.py from the pixel shaders of the client (ps_2_0): do not edit.\n')
        f.write('// One GLSL function for each shader; the wrapper builds a program of its own for every shader the game sets (d3d9.cpp).\n')
        f.write('struct KnownPixelShader { unsigned int hash; int id; const char* name; const char* source; };\nstatic const KnownPixelShader known_pixel_shaders[] = {\n')
        for h, k, name, source in functions:
            f.write('    { 0x%08xu, %d, "%s",\n      R"GLSL(%s)GLSL" },\n' % (h, k, name.replace('\\', '/'), source))
        f.write('};\n')
    print('%d shaders translated, %d skipped' % (len(known), len(skipped)), file=sys.stderr)
    for s in skipped: print('skipped', s, file=sys.stderr)


if __name__ == '__main__':
    main()
