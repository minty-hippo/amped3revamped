#!/usr/bin/env python3
"""Extracts an Xbox 360 disc image (XDVDFS / "XISO") and checks it is Amped 3.

The Linux counterpart of the reader in installer/Install-Amped3.ps1: the game
partition starts at one of a few known offsets (XGD2, XGD3, XGD1, or 0 for a
trimmed / rebuilt image); its volume descriptor (sector 32) points at the root
directory, a binary tree of entries (left / right subtree offsets in dwords,
first sector, size, attributes, name).

    xiso.py check <image.iso | folder with default.xex>
    xiso.py extract <image.iso> <destination folder>

Plain Python 3, no extra modules.
"""
import os
import struct
import sys

SECTOR = 2048
PARTITIONS = (0xFD90000, 0x2080000, 0x18300000, 0)
MAGIC = b'MICROSOFT*XBOX*MEDIA'
AMPED_TITLE_ID = 0x545407D4  # Amped 3 (USA)


def find_partition(f, size):
    for off in PARTITIONS:
        p = off + 32 * SECTOR
        if p + 28 > size:
            continue
        f.seek(p)
        if f.read(20) == MAGIC:
            return off
    return -1


def safe_name(name):
    if not name or name in ('.', '..'):
        return False
    return all(ord(c) >= 32 and c not in '\\/:*?"<>|' for c in name)


def walk(f, part, sector, size, prefix, out, depth=0):
    if size == 0 or size > 64 * 1024 * 1024 or depth > 64:
        return
    f.seek(part + sector * SECTOR)
    d = f.read(size)
    todo, seen = [0], set()
    while todo:
        off = todo.pop()
        if off < 0 or off + 14 > len(d) or off in seen:
            continue
        seen.add(off)
        left, right, esec, esize, attr, nlen = struct.unpack_from('<HHIIBB', d, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # padding / empty folder
        if off + 14 + nlen > len(d):
            continue
        name = d[off + 14:off + 14 + nlen].decode('latin-1')
        if left:
            todo.append(left * 4)
        if right:
            todo.append(right * 4)
        if not safe_name(name):
            continue
        path = prefix + name
        is_dir = bool(attr & 0x10)
        out.append((path, esec, esize, is_dir))
        if is_dir:
            walk(f, part, esec, esize, path + '/', out, depth + 1)


def list_image(f, part):
    f.seek(part + 32 * SECTOR)
    vd = f.read(28)
    root_sector, root_size = struct.unpack_from('<II', vd, 20)
    out = []
    walk(f, part, root_sector, root_size, '', out)
    return out


def xex_title_id(data):
    """default.xex's title ID (XEX2 header, execution info, header 0x00040006)."""
    if len(data) < 24 or data[:4] != b'XEX2':
        return None
    count = struct.unpack_from('>I', data, 0x14)[0]
    for i in range(count):
        o = 0x18 + i * 8
        if o + 8 > len(data):
            break
        key, value = struct.unpack_from('>II', data, o)
        if key == 0x00040006 and value + 16 <= len(data):
            return struct.unpack_from('>I', data, value + 12)[0]
    return None


def check_xex(data):
    tid = xex_title_id(data)
    if tid is None:
        return 'not an Xbox 360 executable'
    if tid != AMPED_TITLE_ID:
        return 'title ID %08X, not Amped 3 (USA, %08X)' % (tid, AMPED_TITLE_ID)
    return None


def fail(msg):
    print('ERROR: ' + msg, file=sys.stderr)
    sys.exit(1)


def cmd_check(path):
    if os.path.isdir(path):
        xex = os.path.join(path, 'default.xex')
        if not os.path.isfile(xex):
            fail('no default.xex in %s' % path)
        with open(xex, 'rb') as f:
            bad = check_xex(f.read(1 << 20))
    else:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            part = find_partition(f, size)
            if part < 0:
                fail("%s isn't an Xbox 360 disc image (no XDVDFS volume found)" % path)
            entries = list_image(f, part)
            xex = [e for e in entries if not e[3] and e[0].lower() == 'default.xex']
            if not xex:
                fail('the disc image has no default.xex')
            f.seek(part + xex[0][1] * SECTOR)
            bad = check_xex(f.read(min(xex[0][2], 1 << 20)))
    if bad:
        fail('default.xex is %s' % bad)
    print('Amped 3 (USA)')


def cmd_extract(iso, dest):
    size = os.path.getsize(iso)
    with open(iso, 'rb') as f:
        part = find_partition(f, size)
        if part < 0:
            fail("%s isn't an Xbox 360 disc image (no XDVDFS volume found)" % iso)
        entries = list_image(f, part)
        xex = [e for e in entries if not e[3] and e[0].lower() == 'default.xex']
        if not xex:
            fail('the disc image has no default.xex')
        f.seek(part + xex[0][1] * SECTOR)
        bad = check_xex(f.read(min(xex[0][2], 1 << 20)))
        if bad:
            fail("this disc's default.xex is %s" % bad)
        files = [e for e in entries if not e[3]]
        total = sum(e[2] for e in files) or 1
        print('Amped 3 (USA): %d files, %.1f GB' % (len(files), total / 2**30))
        os.makedirs(dest, exist_ok=True)
        for path, _, _, is_dir in entries:
            if is_dir:
                os.makedirs(os.path.join(dest, path), exist_ok=True)
        done, last = 0, -1
        for path, sector, fsize, _ in files:
            out = os.path.join(dest, path)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            f.seek(part + sector * SECTOR)
            left = fsize
            with open(out, 'wb') as o:
                while left:
                    chunk = f.read(min(left, 4 << 20))
                    if not chunk:
                        fail('unexpected end of the disc image')
                    o.write(chunk)
                    left -= len(chunk)
            done += fsize
            pct = done * 100 // total
            if pct != last:
                print('\r    %3d%%  %-60.60s' % (pct, path), end='', flush=True)
                last = pct
        print()


def main():
    if len(sys.argv) == 3 and sys.argv[1] == 'check':
        cmd_check(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == 'extract':
        cmd_extract(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
