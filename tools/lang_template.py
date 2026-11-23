"""Collect every translatable text of the trainer.

    python tools/lang_template.py            writes data/lang/_template.json (English keys, empty values)
    python tools/lang_template.py de zh-CN   also adds missing keys to data/lang/<code>.json (keeps what is there)

Sources: data/panel.json (tab, section, label, desc, button, short option lists) and src/*.cpp: tr("..."),
section_header/home_group("..."), and the messages of toast(...) / fail("CODE", ...). Changing parts of a message
become {1} {2}.
"""
import io, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG = os.path.join(ROOT, 'data', 'lang')
LIT = r'"(?:[^"\\]|\\.)*"'
keys = {}


def add(s):
    if s and re.search(r'[A-Za-z]{2}', s.replace('{', '')):
        keys.setdefault(s, '')


def unlit(expr, macros):
    out = ''
    for m in re.finditer(LIT + r'|[A-Z_]+', expr):
        t = m.group(0)
        out += macros.get(t, '') if t[0] != '"' else t[1:-1].encode('latin-1', 'backslashreplace').decode('unicode_escape').encode('latin-1', 'ignore').decode('utf-8', 'ignore')
    return out


def call_args(src, start):
    """argument strings of the call whose '(' is at start"""
    depth, args, cur, i = 0, [], '', start
    while i < len(src):
        m = re.compile(LIT).match(src, i)
        if m:
            cur += m.group(0); i = m.end(); continue
        c = src[i]
        if c in '([{':
            depth += 1
            if depth > 1: cur += c
        elif c in ')]}':
            depth -= 1
            if depth == 0: return args + [cur]
            cur += c
        elif c == ',' and depth == 1:
            args.append(cur); cur = ''
        else:
            cur += c
        i += 1
    return []


def split_plus(e):
    parts, depth, cur, i = [], 0, '', 0
    while i < len(e):
        m = re.compile(LIT).match(e, i)
        if m:
            cur += m.group(0); i = m.end(); continue
        c = e[i]
        depth += c in '([{'; depth -= c in ')]}'
        if c == '+' and depth == 0:
            parts.append(cur.strip()); cur = ''
        else:
            cur += c
        i += 1
    return parts + [cur.strip()]


def message(expr, macros):
    if re.search(r'\?.*:', re.sub(LIT, '', expr)):                 # a ? "x" : "y": each literal on its own
        for m in re.finditer(LIT, expr): add(unlit(m.group(0), macros))
        return
    out, n = '', 0
    for p in split_plus(expr):
        p = re.sub(r'^std::string\((.*)\)$', r'\1', p)
        if re.fullmatch(r'(\s*(' + LIT + r'|WB_PRODUCT)\s*)+', p): out += unlit(p, macros)
        else:
            n += 1; out += '{%d}' % n
    lit = re.sub(r'\{\d\}', '', out).strip(' -')
    if len(lit) >= (4 if n else 1): add(out)


def main():
    panel = json.load(io.open(os.path.join(ROOT, 'data', 'panel.json'), encoding='utf-8-sig'))
    for t in panel['tabs']:
        add(t.get('name'))
        for s in t.get('sections', []):
            add(s.get('name'))
            for it in s.get('items', []):
                add(it.get('label')); add(it.get('desc')); add(it.get('action', {}).get('button'))
                if len(it.get('options', [])) <= 12:
                    for o in it.get('options', []): add(o)
    add('Settings')
    srcs = {f: io.open(os.path.join(ROOT, 'src', f), encoding='utf-8').read() for f in sorted(os.listdir(os.path.join(ROOT, 'src'))) if f.endswith('.cpp')}
    common = io.open(os.path.join(ROOT, 'src', 'common.h'), encoding='utf-8').read()
    macros = dict((a, b) for a, b in re.findall(r'#define (WB_PRODUCT\w*) "([^"]*)"', common))
    for f, s in srcs.items():
        s = re.sub(r'(?m)^\s*//.*$', '', s)
        for m in re.finditer(r'\b(?:tr|section_header|home_group|tr_msg|toast|fail)\(', s):
            name, args = m.group(0)[:-1], call_args(s, m.end() - 1)
            if not args or not any('"' in a for a in args): continue
            if name == 'fail':
                if len(args) > 1 and args[0].strip().startswith('"'): message(args[1], macros)
            elif name in ('section_header', 'home_group'):
                for a in args: message(a, macros)
            else:
                message(args[0], macros)
        # synchronous refusals of panel options:  err = "..."
        for m in re.finditer(r'\berr = (' + LIT + r');', s): add(unlit(m.group(1), macros))
        # fixed readouts and option lists of the game commands
        for m in re.finditer(r'\{\s*"why",\s*(' + LIT + r')', s): add(unlit(m.group(1), macros))
    for o in 'Any Legendary Epic Rare Other Sword Dagger Bow Armor Cloak Ring'.split(): add(o)
    for k in ('Run', 'Hide (Insert)', 'Bridge & look settings', 'Effect #{1} is checked: ADD will replace it', 'Check an effect to replace it with the next ADD.  X removes it.', 'Removed effect #{1}',
              'Loaded {1} items', 'Crash logs saved: {1} (20 per file, old ones are always kept)', '(no gear)', '{1} - done', '{1} - failed', '{1}: ON', '{1}: OFF'): add(k)
    for bad in ('ok', 'message', ': ON', ': OFF', ' - done', ' - failed', 'done', 'failed'): keys.pop(bad, None)
    # developer-only messages nobody meets in the panel: not worth translating
    skip = os.path.join(ROOT, 'tools', 'lang_skip.json')
    if os.path.isfile(skip):
        for k in json.load(io.open(skip, encoding='utf-8')): keys.pop(k, None)
    os.makedirs(LANG, exist_ok=True)

    def write(path, head, vals):
        d = dict(head); d.update(vals)
        with io.open(path, 'w', encoding='utf-8', newline='\n') as o:
            o.write(json.dumps(d, ensure_ascii=False, indent=1) + '\n')

    write(os.path.join(LANG, '_template.json'), {'_language': 'Name of the language', '_font': ''}, keys)
    print('%d texts in _template.json' % len(keys))
    for code in sys.argv[1:]:
        p = os.path.join(LANG, code + '.json')
        old = json.load(io.open(p, encoding='utf-8-sig')) if os.path.isfile(p) else {'_language': code, '_font': ''}
        head = {k: v for k, v in old.items() if k.startswith('_')}
        vals = {k: old.get(k, '') for k in keys}
        gone = [k for k in old if not k.startswith('_') and k not in keys]
        write(p, head, vals)
        print(code, 'translated %d of %d' % (sum(1 for v in vals.values() if v), len(vals)), 'dropped', gone)


if __name__ == '__main__':
    main()
