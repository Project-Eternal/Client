#!/usr/bin/env python3
# Makes the images of the page's shell theme (web/resources/Shell*.png) from the game installation the page is built for (the build
# setting game=PATH, tools/game_install.py): the textures (BLP, DXT1/3/5, palette or uncompressed) are read from the archives in the game's order, decoded,
# cut to the part the game's interface uses, and written as PNG. Only the images that are not there yet are made: an image that is
# in place, whoever put it there, is kept; delete it to have it made again (or give --overwrite).
#
#   python3 tools/shell_art.py [--game data] [--out web/resources] [--overwrite]
#
# Needs numpy; no other library (the PNG writer is zlib). The list of what is made is ART below: output name, texture in the archive, crop rectangle
# (x0, y0, x1, y1 in pixels of the texture, None: whole), and an optional recipe. `border` recipes make a 3x3 picture for CSS border-image from the 8-piece
# edge strip of a backdrop (left, right, top, bottom, then the four corners; top and bottom are stored turned).
import argparse, os, struct, sys, zlib
np = None   # numpy, imported by main() when there is something to make (the build asks for the missing images at every start)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_install

URL = 'http://cdn.twinstar-wow.com/18414.direct/Data/interface.MPQ'   # redirects to dist.blizzard.com.edgesuite.net/wow-pod-retail/NA/15890.direct/Data/

# ---------------------------------------------------------------------------------------------------------------------------------------------------
# BLP2 to RGBA (numpy array height x width x 4, uint8)

def rgb565(v):
    r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
    return np.stack([(r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31], axis=-1).astype(np.uint8)

def decode_dxt(data, width, height, kind):
    bw, bh = (width + 3) // 4, (height + 3) // 4
    size = 8 if kind == 'dxt1' else 16
    blocks = np.frombuffer(data[:bw * bh * size], dtype=np.uint8).reshape(bh * bw, size)
    colors = blocks[:, -8:]
    c0 = colors[:, 0].astype(np.uint32) | (colors[:, 1].astype(np.uint32) << 8)
    c1 = colors[:, 2].astype(np.uint32) | (colors[:, 3].astype(np.uint32) << 8)
    p0, p1 = rgb565(c0).astype(np.int32), rgb565(c1).astype(np.int32)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], axis=1)   # blocks x 4 x 3
    alpha_palette = np.full((blocks.shape[0], 4), 255, dtype=np.int32)
    if kind == 'dxt1': alpha_palette[:, 3] = np.where(four[:, 0], 255, 0)
    bits = (colors[:, 4].astype(np.uint32) | (colors[:, 5].astype(np.uint32) << 8) | (colors[:, 6].astype(np.uint32) << 16) | (colors[:, 7].astype(np.uint32) << 24))
    index = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3   # blocks x 16
    rgb = np.take_along_axis(palette, index[:, :, None].astype(np.int64), axis=1)
    alpha = np.take_along_axis(alpha_palette, index.astype(np.int64), axis=1)
    if kind == 'dxt3':
        a = blocks[:, :8].astype(np.uint32)
        nibble = np.stack([a & 15, a >> 4], axis=-1).reshape(-1, 16)
        alpha = (nibble * 17).astype(np.int32)
    elif kind == 'dxt5':
        a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
        table = np.zeros((blocks.shape[0], 8), dtype=np.int32)
        table[:, 0], table[:, 1] = a0, a1
        for i in range(1, 7):
            table[:, i + 1] = np.where(a0 > a1, ((7 - i) * a0 + i * a1) // 7, np.where(i < 5, ((5 - i) * a0 + i * a1) // 5, 0))
        table[:, 7] = np.where(a0 > a1, table[:, 7], 255)
        table[:, 6] = np.where(a0 > a1, table[:, 6], 0)
        raw = blocks[:, 2:8].astype(np.uint64)
        value = np.zeros(blocks.shape[0], dtype=np.uint64)
        for i in range(6): value |= raw[:, i] << np.uint64(8 * i)
        index = ((value[:, None] >> (3 * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)).astype(np.int64)
        alpha = np.take_along_axis(table, index, axis=1)
    out = np.concatenate([rgb, alpha[:, :, None]], axis=2).astype(np.uint8)   # blocks x 16 x 4
    out = out.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return out[:height, :width]

def decode_blp(data):
    assert data[:4] == b'BLP2', data[:4]
    _, encoding, alpha_depth, alpha_encoding, mips, width, height = struct.unpack('<IBBBBII', data[4:20])
    offset = struct.unpack('<I', data[20:24])[0]; size = struct.unpack('<I', data[84:88])[0]
    palette = np.frombuffer(data[148:148 + 1024], dtype=np.uint8).reshape(256, 4)
    body = data[offset:offset + size]
    if encoding == 2:
        return decode_dxt(body, width, height, {0: 'dxt1', 1: 'dxt3', 7: 'dxt5'}[alpha_encoding])
    if encoding == 1:
        index = np.frombuffer(body[:width * height], dtype=np.uint8).reshape(height, width)
        rgba = np.empty((height, width, 4), dtype=np.uint8)
        rgba[:, :, 0], rgba[:, :, 1], rgba[:, :, 2] = palette[index, 2], palette[index, 1], palette[index, 0]
        a = body[width * height:]
        if alpha_depth == 8: rgba[:, :, 3] = np.frombuffer(a[:width * height], dtype=np.uint8).reshape(height, width)
        elif alpha_depth == 1:
            bits = np.unpackbits(np.frombuffer(a[:(width * height + 7) // 8], dtype=np.uint8), bitorder='little')[:width * height].reshape(height, width)
            rgba[:, :, 3] = bits * 255
        else: rgba[:, :, 3] = 255
        return rgba
    if encoding == 3:
        bgra = np.frombuffer(body[:width * height * 4], dtype=np.uint8).reshape(height, width, 4)
        return bgra[:, :, [2, 1, 0, 3]].copy()
    raise Exception('BLP encoding %d' % encoding)

def write_png(path, rgba):
    h, w = rgba.shape[:2]
    raw = b''.join(b'\x00' + rgba[y].tobytes() for y in range(h))
    def chunk(kind, body):
        c = struct.pack('>I', len(body)) + kind + body
        return c + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

# ---------------------------------------------------------------------------------------------------------------------------------------------------
# what is made

def crop(rgba, box):
    if box is None: return rgba
    x0, y0, x1, y1 = box
    return rgba[y0:y1, x0:x1]

def border(rgba, edge):
    """3x3 picture of a backdrop's edge strip: 8 pieces of `edge` pixels across (left, right, top, bottom, TL, TR, BL, BR); the vertical pieces are
    `edge` wide and as tall as the strip, the top and bottom ones are stored turned a quarter. Result: (3*edge) square, the edges one tile long."""
    h = rgba.shape[0]
    piece = lambda i: rgba[:, i * edge:(i + 1) * edge]
    left, right, top, bottom = piece(0), piece(1), np.rot90(piece(2), -1), np.rot90(piece(3), -1)
    tl, tr, bl, br = (piece(i)[:edge] for i in range(4, 8))
    tile = min(h, 64)
    out = np.zeros((edge + tile + edge, edge + tile + edge, 4), dtype=np.uint8)
    out[:edge, :edge], out[:edge, -edge:], out[-edge:, :edge], out[-edge:, -edge:] = tl, tr, bl, br
    out[edge:edge + tile, :edge], out[edge:edge + tile, -edge:] = left[:tile], right[:tile]
    out[:edge, edge:edge + tile], out[-edge:, edge:edge + tile] = top[:, :tile][:edge], bottom[:, :tile][:edge]
    return out

# name of the PNG, texture, crop box, recipe
ART = [
    ('ShellFrame',          r'Interface\DialogFrame\UI-DialogBox-Border.blp',      None, ('border', 32)),
    ('ShellBackground',     r'Interface\DialogFrame\UI-DialogBox-Background.blp',  None, None),
    ('ShellButtonUp',       r'Interface\BUTTONS\UI-Panel-Button-Up.blp',           (0, 0, 80, 22), None),
    ('ShellButtonDown',     r'Interface\BUTTONS\UI-Panel-Button-Down.blp',         (0, 0, 80, 22), None),
    ('ShellButtonDisabled', r'Interface\BUTTONS\UI-Panel-Button-Disabled.blp',     (0, 0, 80, 22), None),
    ('ShellTooltipFrame',   r'Interface\Tooltips\UI-Tooltip-Border.blp',           None, ('border', 16)),
    # the quality indicator: the texture of the game's status bars (a grey gradient, tinted green by the CSS as the game tints a health bar)
    ('ShellStatusBar',      r'Interface\TargetingFrame\UI-StatusBar.blp',          None, None),
]

def missing_images(out, only=None):
    """The names of the images of ART that are not in `out` yet."""
    return [name for name, _, _, _ in ART if (not only or name in only) and not os.path.isfile(os.path.join(out, name + '.png'))]

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game', default=os.path.join(here, '..', 'data'), help='the game installation')
    parser.add_argument('--locale', help='the language of the installation, when it cannot be told')
    parser.add_argument('--out', default=os.path.join(here, '..', 'web', 'resources'))
    parser.add_argument('--only', help='names of the images to make, comma separated')
    parser.add_argument('--overwrite', action='store_true', help='also remake the images that exist')
    parser.add_argument('--raw', action='store_true', help='also write each whole texture as RAW-<name>.png (for choosing crops)')
    args = parser.parse_args()
    global np
    only = args.only.split(',') if args.only else None
    os.makedirs(args.out, exist_ok=True)
    todo = [n for n, _, _, _ in ART if (not only or n in only)] if args.overwrite else missing_images(args.out, only)
    if not todo:
        return
    import numpy
    np = numpy
    try:
        installation = game_install.open_installation(args.game, args.locale)
        textures = {name: installation.read(texture) for name, texture, _, _ in ART if name in todo}
    except game_install.GameError as e:
        sys.exit('shell_art: %s' % e)
    for name, texture, box, recipe in ART:
        if name not in todo: continue
        image = decode_blp(textures[name])
        if args.raw: write_png(os.path.join(args.out, 'RAW-' + name + '.png'), image)
        image = crop(image, box)
        if recipe and recipe[0] == 'border': image = border(image, recipe[1])
        write_png(os.path.join(args.out, name + '.png'), np.ascontiguousarray(image))
        print('%-22s %-58s %dx%d' % (name, texture, image.shape[1], image.shape[0]))

if __name__ == '__main__':
    main()
