"""An independent reader of a HiFi XI texture pack (docs/texture-pack/format.md), for pack_test and texpack's tests.

  python -I tests\\pack_oracle.py <pack folder> [--no-data]

Checks pack.idx: the magic, version 1, the table sizes, the header's CRC, the file's exact length, the tables' CRC
(zlib.crc32: the pack's CRC-32 is zlib's), keys sorted with none twice; every known map record: its data file and level
CRCs inside the tables, its offset a multiple of 512 and inside the data file (its first 1 GiB, the index's size and the
real one), a
BC2/BC7/BC4 UNORM format, sides multiples of 4 up to 16384, 1 to the whole chain of levels, its bytes the copy layout's
(rows 256 apart, levels at multiples of 512, as GetCopyableFootprints); unused records all zero. Then (unless --no-data)
every record's bytes read from its data file against its CRC and each level's against the level CRC table.

Prints one line per problem ("FAIL ...") and a last line "pack_oracle: entries N, maps M, data files F, map bytes B";
the exit code is 0 when nothing is wrong, else 1. struct and zlib only (python -I, no numpy).
"""
import os
import struct
import sys
import zlib

HEADER = struct.Struct('<4s11IQ4Q96s2I')    # 192 bytes
ENTRY = struct.Struct('<QIIIIBBBBBBHII16s2I')  # 64 bytes, then 3 records
RECORD = struct.Struct('<BBHHHIIIIII')      # 32 bytes
BLOCK = {74: 16, 98: 16, 80: 8}             # DXGI_FORMAT_BC2_UNORM, BC7_UNORM, BC4_UNORM
assert HEADER.size == 192 and ENTRY.size == 64 and RECORD.size == 32


def footprint(fmt, w, h, levels):
    """Each level's offset in a record and the record's total bytes, as D3D12 copies it"""
    block = BLOCK[fmt]
    total, offsets = 0, []
    for i in range(levels):
        lw, lh = max(1, w >> i), max(1, h >> i)
        row = (lw + 3) // 4 * block
        pitch = (row + 255) // 256 * 256
        rows = (lh + 3) // 4
        off = (total + 511) // 512 * 512
        offsets.append(off)
        total = off + pitch * (rows - 1) + row
    return offsets, total


def full_levels(w, h):
    n, s = 1, max(w, h)
    while s > 1:
        s //= 2
        n += 1
    return n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        print(__doc__)
        return 2
    folder = args[0]
    data = '--no-data' not in sys.argv
    problems = []

    def fail(what):
        problems.append(what)
        print('FAIL ' + what)

    with open(os.path.join(folder, 'pack.idx'), 'rb') as f:
        idx = f.read()
    if len(idx) < HEADER.size:
        fail('pack.idx shorter than a header')
        return 1
    h = HEADER.unpack_from(idx, 0)
    (magic, version, header_bytes, flags, entry_bytes, record_bytes, per_entry, entry_count, file_count, crc_count,
     maker, _reserved, build_time, vt_size, vt_mtime, ft_size, ft_mtime, source, table_crc, header_crc) = h
    if magic != b'HXP1':
        fail('magic %r' % magic)
        return 1
    if version != 1:
        fail('version %d' % version)
        return 1
    if zlib.crc32(idx[:188]) != header_crc:
        fail('header CRC')
    if (header_bytes, entry_bytes, record_bytes, per_entry) != (192, 160, 32, 3):
        fail('table sizes %d %d %d %d' % (header_bytes, entry_bytes, record_bytes, per_entry))
        return 1
    if flags != 1:
        fail('flags %d (v1: 1, keys over the whole body)' % flags)
    want = 192 + 8 * file_count + 160 * entry_count + 4 * crc_count
    if len(idx) != want:
        fail('pack.idx is %d bytes, its tables say %d' % (len(idx), want))
        return 1
    if zlib.crc32(idx[192:]) != table_crc:
        fail('tables CRC')
    at = 192
    file_bytes = list(struct.unpack_from('<%dQ' % file_count, idx, at))
    at += 8 * file_count
    entries_at = at
    at += 160 * entry_count
    level_crcs = list(struct.unpack_from('<%dI' % crc_count, idx, at))

    real = []
    for i in range(file_count):
        path = os.path.join(folder, 'pack-%03d.bin' % i)
        size = os.path.getsize(path) if os.path.exists(path) else -1
        real.append(size)
        if size < 0:
            fail('pack-%03d.bin missing' % i)
        elif size < file_bytes[i]:
            fail('pack-%03d.bin is %d bytes, the index says %d' % (i, size, file_bytes[i]))

    handles = {}
    last_key = None
    maps = 0
    map_bytes = 0
    for n in range(entry_count):
        base = entries_at + 160 * n
        e = ENTRY.unpack_from(idx, base)
        k = (e[0], e[1], e[2], e[3])
        if last_key is not None and not last_key < k:
            fail('entry %d: key not after the last one' % n)
        last_key = k
        for m in range(3):
            raw = idx[base + 64 + 32 * m: base + 96 + 32 * m]
            kind, levels, file, w, hh, fmt, offset, nbytes, crc, level_crc, param = RECORD.unpack(raw)
            if kind == 0:
                if raw != bytes(32):
                    fail('entry %d map %d: unused but not zero' % (n, m))
                continue
            if kind not in (1, 2, 3):
                continue
            maps += 1
            map_bytes += nbytes
            where = 'entry %d (%016X) map %d' % (n, k[0], m)
            if file >= file_count:
                fail(where + ': data file %d of %d' % (file, file_count))
                continue
            if offset % 512:
                fail(where + ': offset %d not a multiple of 512' % offset)
            if offset + nbytes > 1 << 30:
                fail(where + ': bytes past a data file\'s 1 GiB')
                continue
            if offset + nbytes > file_bytes[file] or offset + nbytes > real[file]:
                fail(where + ': bytes past the end of pack-%03d.bin' % file)
                continue
            if fmt not in BLOCK:
                fail(where + ': format %d' % fmt)
                continue
            # (M9: a height record's param is its span, a float32 0 or above; the others' is 0)
            span = struct.unpack('<f', struct.pack('<I', param))[0]
            if kind == 2 and not (span >= 0 and span < float('inf')):
                fail(where + ': height span %r' % span)
            if kind == 1 and param != 0:
                fail(where + ': colour record param %d' % param)
            if w == 0 or hh == 0 or w % 4 or hh % 4 or w > 16384 or hh > 16384:
                fail(where + ': size %dx%d' % (w, hh))
                continue
            if levels < 1 or levels > full_levels(w, hh):
                fail(where + ': %d levels' % levels)
                continue
            offsets, total = footprint(fmt, w, hh, levels)
            if total != nbytes:
                fail(where + ': %d bytes, the copy layout has %d' % (nbytes, total))
                continue
            if level_crc + levels > crc_count:
                fail(where + ': level CRCs past the table')
                continue
            if not data:
                continue
            if file not in handles:
                handles[file] = open(os.path.join(folder, 'pack-%03d.bin' % file), 'rb')
            fh = handles[file]
            fh.seek(offset)
            body = fh.read(nbytes)
            if len(body) != nbytes:
                fail(where + ': cut short')
                continue
            if zlib.crc32(body) != crc:
                fail(where + ': CRC')
            for i in range(levels):
                end = offsets[i + 1] if i + 1 < levels else total
                if zlib.crc32(body[offsets[i]:end]) != level_crcs[level_crc + i]:
                    fail(where + ': level %d CRC' % i)
    for fh in handles.values():
        fh.close()
    print('pack_oracle: entries %d, maps %d, data files %d, map bytes %d, source "%s", maker %d%s' % (
        entry_count, maps, file_count, map_bytes, source.rstrip(b'\0').decode('ascii', 'replace'), maker,
        '' if not problems else '; %d problems' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
