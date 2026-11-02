#!/usr/bin/env python3
# Turns an apply_60fps_*.csx (expects clean vanilla) into one that runs on the Russian (LazyDesman) data.win:
#  - drops the vanilla code-count guard (the "already has scr_fps_lerp_rate" guard stays),
#  - uses the merged Russian+60fps GML for entries both mods touch (m/M_<key>/<name>.gml),
#  - for chapter 3 keeps the Russian obj_ch3_couch_video untouched (60 FPS ships pre-V23 code there).
# Usage: build.py <in.csx> <m dir> <key> <out.csx>
import os, re, sys

SKIP = {'3': {'gml_Object_obj_ch3_couch_video_Create_0', 'gml_Object_obj_ch3_couch_video_Draw_0',
              'gml_Object_obj_ch3_couch_video_Other_70'}}

def apply_spans(s):
    """Yield (name, start, body_start, body_end, end) for every Apply("name", @"...");"""
    head = re.compile(r'^Apply\("([^"]+)",\s*@"', re.M)
    pos = 0
    while (m := head.search(s, pos)):
        i = m.end()
        while True:
            j = s.index('"', i)
            if s.startswith('""', j): i = j + 2
            else: break
        assert s.startswith(');\n', j + 1), m.group(1)
        yield m.group(1), m.start(), m.end(), j, j + 4
        pos = j + 4

def main():
    src, mdir, key, out = sys.argv[1:5]
    s = open(src, encoding='utf-8').read()
    s, n = re.subn(r'if \(Data\.Code\.Count != \d+\)\n\s+throw new Exception\([^;]*;\n', '', s)
    assert n == 1, 'count guard'
    overlap = set(open(f'{mdir}/overlap_{key}.names').read().split())
    skip = SKIP.get(key, set())
    parts, last, merged, skipped, total = [], 0, 0, 0, 0
    for name, a, bs, be, e in apply_spans(s):
        total += 1
        if name in skip:
            parts.append(s[last:a]); last = e; skipped += 1
            continue
        if name in overlap:
            body = open(f'{mdir}/M_{key}/{name}.gml', encoding='utf-8').read()
            assert '<<<<<<<' not in body, name
            parts.append(s[last:bs]); parts.append(body.replace('"', '""')); last = be; merged += 1
    parts.append(s[last:])
    s = ''.join(parts)
    s, n = re.subn(r'if \(__applied != (\d+)\)',
                   lambda m: f'if (__applied != {int(m.group(1)) - skipped})', s)
    assert n == 1, 'applied check'
    if key == '3':
        s, n = re.subn(r'// ---- delete event obj_ch3_couch_video Other:5.*?\n}\n', '', s, flags=re.S)
        assert n == 1, 'couch delete'
    open(out, 'w', encoding='utf-8').write(s)
    print(f'{key}: {total} entries, merged {merged}, skipped {skipped} -> {os.path.basename(out)}')

if __name__ == '__main__':
    main()
