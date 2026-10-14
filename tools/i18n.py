#!/usr/bin/env python3
"""Interface-language catalog tooling.

    python3 tools/i18n.py gen      write source/i18n/i18n_tables.c from the catalog
    python3 tools/i18n.py check    verify the catalog against the TR()/TRN() uses
    python3 tools/i18n.py chars    print every non-Latin-1 character the catalog uses (font subset)

The catalog is source/i18n/catalog/*.json, one object per file:
    { "English text": { "ja": "...", "pt-BR": "...", "de": "...", "fr": "...", "es": "..." } }
The English text is the key, exactly as it is written in the TR("...") call.
"""
import glob, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAT_DIR = os.path.join(ROOT, 'source', 'i18n', 'catalog')
OUT = os.path.join(ROOT, 'source', 'i18n', 'i18n_tables.c')
LANGS = ['ja', 'pt-BR', 'de', 'fr', 'es']          # LANG_JA .. LANG_ES, in enum order
SRC_DIRS = ['source']
SRC_SKIP = (os.path.join('source', 'i18n'), os.path.join('source', 'audio'),
            os.path.join('source', 'gfx'), os.path.join('source', 'ui', 'fonts'))

SPEC = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t)?[diuoxXfFeEgGcsp%]')


def specs(s):
    return [m for m in SPEC.findall(s) if m != '%%']


def load_catalog():
    cat, where = {}, {}
    for path in sorted(glob.glob(os.path.join(CAT_DIR, '*.json'))):
        with open(path, encoding='utf-8') as f:
            data = json.load(f)
        for k, v in data.items():
            if k in cat:
                sys.exit('duplicate key %r in %s and %s' % (k, where[k], path))
            cat[k] = v
            where[k] = path
    return cat, where


def problems_of(cat, where):
    errs = []
    for k, v in cat.items():
        w = os.path.basename(where[k])
        extra = set(v) - set(LANGS)
        if extra:
            errs.append('%s: %r has unknown language(s) %s' % (w, k, sorted(extra)))
        for lg in LANGS:
            t = v.get(lg)
            if not t:
                errs.append('%s: %r has no %s' % (w, k, lg))
                continue
            if specs(t) != specs(k):
                errs.append('%s: %r [%s] conversions %s differ from English %s' % (w, k, lg, specs(t), specs(k)))
            if t.count('\n') != k.count('\n'):
                errs.append('%s: %r [%s] has %d line breaks, English has %d' % (w, k, lg, t.count('\n'), k.count('\n')))
            if (t[:1].isspace(), t[-1:].isspace()) != (k[:1].isspace(), k[-1:].isspace()):
                errs.append('%s: %r [%s] changes the leading/trailing space' % (w, k, lg))
    return errs


# ---- C string literal reading -------------------------------------------------
ESC = {'n': '\n', 't': '\t', '\\': '\\', '"': '"', "'": "'", 'r': '\r', '0': '\0'}


def c_unescape(body):
    out, i = bytearray(), 0
    b = body
    while i < len(b):
        c = b[i]
        if c == '\\' and i + 1 < len(b):
            n = b[i + 1]
            if n == 'x':
                j = i + 2
                while j < len(b) and b[j] in '0123456789abcdefABCDEF':
                    j += 1
                out.append(int(b[i + 2:j], 16))
                i = j
                continue
            if n in '01234567':
                j = i + 1
                while j < len(b) and j < i + 4 and b[j] in '01234567':
                    j += 1
                out.append(int(b[i + 1:j], 8))
                i = j
                continue
            out += ESC.get(n, n).encode('latin-1')
            i += 2
            continue
        out += c.encode('utf-8')
        i += 1
    return out.decode('utf-8', errors='replace')


LIT = r'"(?:[^"\\\n]|\\.)*"'
PART = r'(?:' + LIT + r'|[A-Z][A-Z0-9_]*)'          # a literal, or an all-caps string macro
CALL = re.compile(r'\b(TRN?)\s*\(\s*((?:' + PART + r'\s*)+)\)')
ONE = re.compile(PART)
DEFINE = re.compile(r'^#define\s+([A-Z][A-Z0-9_]*)\s+((?:' + LIT + r'\s*)+)$', re.M)


def string_macros():
    """#define NAME "literal" in the sources: a TRN() may splice one in."""
    macros = {}
    for d, _, fs in os.walk(os.path.join(ROOT, 'source')):
        for fn in fs:
            if fn.endswith('.h'):
                with open(os.path.join(d, fn), encoding='utf-8', errors='replace') as f:
                    for m in DEFINE.finditer(f.read()):
                        macros[m.group(1)] = ''.join(c_unescape(x[1:-1]) for x in re.findall(LIT, m.group(2)))
    return macros


def uses():
    """{english: [(file, line), ...]} for every TR()/TRN() in the sources."""
    found = {}
    macros = string_macros()
    for base in SRC_DIRS:
        for d, _, fs in os.walk(os.path.join(ROOT, base)):
            rel = os.path.relpath(d, ROOT)
            if any(rel == s or rel.startswith(s + os.sep) for s in SRC_SKIP) or 'third_party' in rel:
                continue
            for fn in fs:
                if not fn.endswith(('.c', '.cpp', '.h', '.inc')):
                    continue
                p = os.path.join(d, fn)
                with open(p, encoding='utf-8', errors='replace', newline='') as f:
                    text = f.read()
                for m in CALL.finditer(text):
                    key = ''.join(c_unescape(x[1:-1]) if x[0] == '"' else macros.get(x, '<' + x + '?>')
                                  for x in ONE.findall(m.group(2)))
                    line = text.count('\n', 0, m.start()) + 1
                    found.setdefault(key, []).append((os.path.relpath(p, ROOT).replace(os.sep, '/'), line))
    return found


def cmd_check():
    cat, where = load_catalog()
    errs = problems_of(cat, where)
    found = uses()
    for k, sites in sorted(found.items()):
        if k not in cat:
            errs.append('missing from the catalog: %r (%s:%d)' % (k, sites[0][0], sites[0][1]))
    for k in sorted(cat):
        if k not in found:
            errs.append('catalog row used nowhere: %r (%s)' % (k, os.path.basename(where[k])))
    if os.path.exists(OUT):
        with open(OUT, encoding='utf-8') as f:
            if f.read() != render(cat):
                errs.append('source/i18n/i18n_tables.c is stale: run tools/i18n.py gen')
    else:
        errs.append('source/i18n/i18n_tables.c is missing: run tools/i18n.py gen')
    for e in errs:
        print('i18n:', e)
    print('i18n: %d strings in the catalog, %d used, %d problem(s)' % (len(cat), len(found), len(errs)))
    return 1 if errs else 0


def cesc(s):
    out = []
    for b in s.encode('utf-8'):
        if b == 0x22:   out.append('\\"')
        elif b == 0x5C: out.append('\\\\')
        elif b == 0x0A: out.append('\\n')
        elif b == 0x09: out.append('\\t')
        elif 0x20 <= b < 0x7F: out.append(chr(b))
        else: out.append('\\%03o' % b)
    return ''.join(out)


def render(cat):
    keys = sorted(cat, key=lambda s: s.encode('utf-8'))   # strcmp order
    o = ['// Generated by tools/i18n.py from source/i18n/catalog/*.json -- do NOT edit by hand.',
         '// The keys are sorted by strcmp: tr() binary-searches them.', '',
         '#include <stddef.h>', '#include "i18n.h"', '',
         '// Declared first so the definitions keep external linkage when this file is built as C++.',
         'extern const int i18n_n;', 'extern const char *const i18n_keys[];',
         'extern const char *const *const i18n_text[LANG__COUNT];', '',
         'const int i18n_n = %d;' % len(keys), '']
    o.append('const char *const i18n_keys[%d] = {' % max(1, len(keys)))
    for k in keys:
        o.append('    "%s",' % cesc(k))
    o.append('};')
    for i, lg in enumerate(LANGS):
        ident = lg.replace('-', '_')
        o.append('')
        o.append('static const char *const i18n_text_%s[%d] = {' % (ident, max(1, len(keys))))
        for k in keys:
            o.append('    "%s",' % cesc(cat[k][lg]))
        o.append('};')
    o.append('')
    o.append('const char *const *const i18n_text[LANG__COUNT] = {')
    o.append('    NULL,')
    for lg in LANGS:
        o.append('    i18n_text_%s,' % lg.replace('-', '_'))
    o.append('};')
    o.append('')
    return '\n'.join(o)


def cmd_gen():
    cat, where = load_catalog()
    errs = problems_of(cat, where)
    if errs:
        for e in errs:
            print('i18n:', e)
        return 1
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(render(cat))
    print('i18n: wrote %s (%d strings)' % (os.path.relpath(OUT, ROOT), len(cat)))
    return 0


def cmd_chars():
    cat, _ = load_catalog()
    cs = set()
    for v in cat.values():
        for t in v.values():
            cs.update(ch for ch in t if ord(ch) > 0xFF)
    sys.stdout.write(''.join(sorted(cs)))
    return 0


if __name__ == '__main__':
    cmds = {'gen': cmd_gen, 'check': cmd_check, 'chars': cmd_chars}
    if len(sys.argv) != 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    sys.exit(cmds[sys.argv[1]]())
