#!/usr/bin/env python3
# 3-way merge of 60 FPS changes into the Russian translation for entries both mods touch.
# base = vanilla decompile (V), ours = Russian decompile (R), theirs = 60 FPS GML from the apply script (F).
# Usage: merge.py <apply_60fps.csx> <workdir/m> <key>   -> writes M_<key>/<name>.gml (+ .conflict list)
import os, re, subprocess, sys

def parse_apply(csx):
    """Return {name: gml} from Apply("name", @"...") calls (C# verbatim strings, "" = ")."""
    s = open(csx, encoding='utf-8').read()
    out, pos = {}, 0
    head = re.compile(r'^Apply\("([^"]+)",\s*@"', re.M)
    while True:
        m = head.search(s, pos)
        if not m:
            return out
        i, buf = m.end(), []
        while True:
            j = s.index('"', i)
            buf.append(s[i:j])
            if s.startswith('""', j):
                buf.append('"'); i = j + 2
            else:
                i = j + 1; break
        assert s.startswith(');', i), m.group(1)
        out[m.group(1)] = ''.join(buf)
        pos = i

def norm(t):
    t = t.replace('\r\n', '\n')
    # the chapter 5 script indents with tabs, the decompiler with 4 spaces
    t = re.sub(r'^\t+', lambda m: '    ' * len(m.group(0)), t, flags=re.M)
    return t if t.endswith('\n') else t + '\n'

TOK = re.compile(r'"(?:[^"\\\n]|\\.)*"|[A-Za-z_][A-Za-z0-9_]*|\d+(?:\.\d+)?|\n|[ \t]+|.')

def merge3(ours, base, theirs, tmpdir):
    """git merge-file on (ours, base, theirs); returns (text, clean)."""
    paths = [f'{tmpdir}/m.{x}' for x in ('o', 'b', 't')]
    for p, t in zip(paths, (ours, base, theirs)):
        open(p, 'w', encoding='utf-8').write(t)
    p = subprocess.run(['git', 'merge-file', '-p', '--diff3', '-L', 'ru', '-L', 'vanilla', '-L', '60fps', *paths],
                       capture_output=True, text=True)
    for x in paths: os.remove(x)
    return p.stdout, p.returncode == 0

def tok_lines(t):
    # one token per line; real newlines become a marker so line-based merge works on tokens
    return ''.join(('\\n' if k == '\n' else k.replace('\\', '\\\\')) + '\n' for k in TOK.findall(t))

def untok(t):
    out = []
    for line in t.split('\n')[:-1]:
        out.append('\n' if line == '\\n' else line.replace('\\\\', '\\'))
    return ''.join(out)

HUNK = re.compile(r'^<<<<<<< ru\n(.*?)^\|\|\|\|\|\|\| vanilla\n(.*?)^=======\n(.*?)^>>>>>>> 60fps\n', re.M | re.S)

def resolve_tokens(text, tmpdir):
    """Re-merge each line-level conflict hunk token by token; leave unresolvable hunks as conflicts."""
    left = 0
    def fix(m):
        nonlocal left
        merged, ok = merge3(tok_lines(m.group(1)), tok_lines(m.group(2)), tok_lines(m.group(3)), tmpdir)
        if ok:
            return untok(merged)
        left += 1
        return m.group(0)
    return HUNK.sub(fix, text), left

def main():
    csx, mdir, key = sys.argv[1:4]
    F = parse_apply(csx)
    names = [l.strip() for l in open(f'{mdir}/overlap_{key}.names') if l.strip()]
    outd = f'{mdir}/M_{key}'; os.makedirs(outd, exist_ok=True)
    clean = conflict = 0
    conflicts = []
    for n in names:
        v = norm(open(f'{mdir}/V_{key}/CodeEntries/{n}.gml', encoding='utf-8').read())
        r = norm(open(f'{mdir}/R_{key}/CodeEntries/{n}.gml', encoding='utf-8').read())
        f = norm(F[n])
        tmp = [f'{outd}/{n}.{x}' for x in ('R', 'V', 'F')]
        for p, t in zip(tmp, (r, v, f)):
            open(p, 'w', encoding='utf-8').write(t)
        text, ok = merge3(r, v, f, outd)
        left = 0
        if not ok:
            text, left = resolve_tokens(text, outd)
        open(f'{outd}/{n}.gml', 'w', encoding='utf-8').write(text)
        if left == 0:
            clean += 1
            for x in tmp: os.remove(x)
        else:
            conflict += 1; conflicts.append((n, left))
    open(f'{outd}/conflicts.txt', 'w').write(''.join(f'{n} {c}\n' for n, c in conflicts))
    print(f'{key}: clean={clean} conflict={conflict} (60fps entries in script: {len(F)})')

if __name__ == '__main__':
    main()
