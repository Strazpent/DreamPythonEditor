"""Self-checks over TDD.md, SPEC.md, README.md and the decision log (TDD v1.2; written for the v1.1 integration, step 4).
Adapted from tddwork/check.py: cells split on unescaped pipes only; Lands checks; link checks across files."""
import re, sys, os, collections
sys.stdout.reconfigure(encoding='utf-8')
sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else __file__.rsplit('/', 1)[0])
import lib

VERBOSE = '-v' in sys.argv
doc = lib.read(lib.TDD)
spec = lib.read(lib.SPEC)
problems = collections.Counter()


def bad(kind, *msg):
    problems[kind] += 1
    if VERBOSE or problems[kind] <= 12:
        print('  ', kind, '|', *msg)


hs = lib.headings(doc)
nums = {}
for lvl, t, s, i in hs:
    n = lib.secnum(t)
    if n: nums[n] = (lvl, t)
print('TDD headings', len(hs), 'numbered', len(nums))

# 1. continuous numbering
tops = [int(lib.secnum(t)) for l, t, s, i in hs if l == 2 and lib.secnum(t)]
if tops != list(range(1, 18)): bad('numbering', 'top', tops)
children = collections.defaultdict(list)
for n in nums:
    if '.' in n:
        p, c = n.rsplit('.', 1); children[p].append(int(c))
for p, cs in children.items():
    if sorted(cs) != list(range(1, len(cs) + 1)): bad('numbering', 'children of', p, sorted(cs))
    if p not in nums: bad('numbering', 'parent missing', p)
for n, (lvl, t) in nums.items():
    if lvl != n.count('.') + 2: bad('numbering', 'level', lvl, t)
# headings in document order are increasing
seq = [tuple(int(x) for x in lib.secnum(t).split('.')) for l, t, s, i in hs if lib.secnum(t)]
for a, b in zip(seq, seq[1:]):
    if b <= a: bad('numbering', 'order', a, b)

# 2. section references (§x.y in TDD resolve, unless they cite another document)
other_doc = re.compile(r'(\b08|\b09|\b13|R0[1-6]|SPEC|07[a-e]|Apache-2\.0|SPEC v0\.9|SPEC v1\.[01]|PEP|RFC)\s*\[?$')
for i, ln, fence, st in lib.walk(doc):
    for m in re.finditer(r'§(\d+(?:\.\d+)*)', ln):
        pre = ln[max(0, m.start() - 16):m.start()]
        if other_doc.search(pre): continue
        # "08 §7.1 and §7.2", "SPEC §6 and §10": a run of § refs after another document's name
        run = ln[max(0, m.start() - 60):m.start()]
        if re.search(r'(\b08|\b09|\b13|SPEC|R0[1-6]|07[a-e])\s*\[?§[\d.]+\]?(\([^)]*\))?((,|,? and| to|–|-|/| or)\s*\[?§[\d.]+\]?(\([^)]*\))?)*(,|,? and| to|–|-|/| or)\s*\[?$', run):
            continue
        if re.search(r'\]\((research/|SPEC\.md|\.\./)', ln[m.end():m.end() + 6]): continue
        if m.group(1) not in nums: bad('sectionref', i + 1, '§' + m.group(1), '|', pre.strip())

# 3. test IDs: defined once; every referenced ID is defined
rows = lib.test_rows(doc)
defined = collections.Counter(t for _, t, _, _ in rows)
for t, c in defined.items():
    if c > 1: bad('test-dup', t, c)
def_ids = set(defined)
tid_ref = re.compile(r'(?<![A-Za-z0-9-])T-([A-Z0-9]+)-(\d{3}[a-z]?)(?:\.\.(\d{3}))?\b')
for name, text in [('TDD', doc), ('SPEC', spec), ('README', lib.read(lib.DOCS + '/README.md')),
                   ('08', lib.read(lib.DLOG))]:
    for m in tid_ref.finditer(text):
        a, n, hi = m.group(1), m.group(2), m.group(3)
        if a in ('AREA',): continue
        ids = [f'T-{a}-{n}'] if not hi else [f'T-{a}-{k:03d}' for k in range(int(n), int(hi) + 1)]
        for x in ids:
            if x not in def_ids: bad('test-ref', name, x)

# 4. SPEC IDs referenced in the TDD exist
sids = lib.spec_ids(spec)
idre = lib.spec_id_regex(sids)
miss = collections.Counter(x for x in idre.findall(doc) if x not in sids)
for k, v in miss.items(): bad('spec-ref', k, v)

# 5. table column counts (TDD and SPEC) and fences
def table_check(name, text):
    fence = False; ncol = None
    for i, ln in enumerate(text.split('\n')):
        if ln.startswith('```'): fence = not fence; ncol = None; continue
        if fence: continue
        if ln.startswith('|'):
            n = lib.ncells(ln)
            if ncol is None: ncol = n
            elif n != ncol: bad('columns', name, i + 1, n, ncol, ln[:80])
        else:
            ncol = None
    if fence: bad('fence', name, 'unbalanced')
table_check('TDD', doc)
table_check('SPEC', spec)

# 6. links and anchors across TDD, SPEC, README, the decision log (and every research file's links)
def slugs_of(path):
    t = lib.read(path)
    s = {x[2] for x in lib.headings(t)}
    s |= set(re.findall(r'<a (?:id|name)="([^"]+)"', t))
    return s
cache = {}
def anchors(path):
    path = os.path.normcase(os.path.abspath(path))
    if path not in cache: cache[path] = slugs_of(path)
    return cache[path]
files = [lib.TDD, lib.SPEC, lib.DOCS + '/README.md', lib.DLOG]
files += [os.path.join(lib.DOCS, 'research', f) for f in os.listdir(lib.DOCS + '/research') if f.endswith('.md')]
nlinks = 0
for f in files:
    text = lib.read(f)
    base = os.path.dirname(f)
    infence = False
    for i, ln in enumerate(text.split('\n')):
        if ln.startswith('```'): infence = not infence; continue
        if infence: continue
        for m in re.finditer(r'\]\(([^)\s]*)\)', ln):
            target = m.group(1)
            if re.match(r'^(https?:|mailto:)', target): continue
            nlinks += 1
            path, _, anc = target.partition('#')
            full = os.path.normpath(os.path.join(base, path)) if path else f
            if not os.path.exists(full):
                bad('link-file', os.path.basename(f), i + 1, target); continue
            if anc and full.endswith('.md') and anc not in anchors(full):
                bad('link-anchor', os.path.basename(f), i + 1, target)
print('links checked', nlinks)

# 7. coverage: every requirement landing Phase 0 or M1..M4 has a test naming it
tests = {t: c for _, t, c, _ in rows}
covers = collections.defaultdict(list)
for t, c in tests.items():
    for s in idre.findall(c[4]):
        covers[s].append(t)
for s, info in sids.items():
    if info['withdrawn'] or not info['ms']: continue
    if lib.RANK[info['ms'][0]] <= lib.RANK['M4'] and not covers.get(s):
        bad('coverage', s, info['lands'][:60])
# NFR bullets and PERF rows with no Lands: still need a test
for s, info in sids.items():
    if not info['ms'] and not info['withdrawn'] and not covers.get(s):
        bad('coverage', s, '(NFR)')

# 8. Lands: every row has one; never earlier than a covered requirement unless split
for _, t, c, st in rows:
    if len(c) != 6 or not c[5]:
        bad('lands-missing', t); continue
    ms = lib.milestones(c[5])
    if not ms: bad('lands-value', t, c[5]); continue
    split = '(' in c[5] and bool(re.search(r'\((?:Phase 0|M[1-4]|1\.0\.x|1\.[12]|Later)\b', c[5]))
    for s in idre.findall(c[4]):
        info = sids.get(s)
        # T-GAP-008 (TDD v1.1, V-delivery-tests-07): Later and Withdrawn requirements are skipped; a test naming one asserts its absence
        if not info or not info['ms'] or info['withdrawn'] or info['later']: continue
        if lib.RANK[ms[0]] < lib.RANK[info['ms'][0]] and not split:
            bad('lands-early', t, c[5], '<', s, info['lands'][:40])

# 9. SPEC self-check: unique IDs across requirement tables and bullets
seen = collections.Counter()
hdr = None; prev = ''
for ln in spec.split('\n'):
    if ln.startswith('|') and not prev.startswith('|'): hdr = ln.strip()
    prev = ln
    m = re.match(r'^\| *([A-Z][A-Z0-9]*(?:-NF)?-\d+) *\|', ln)
    if m and hdr in ('| ID | Requirement | Lands | Acceptance criteria |', '| ID | Budget | Target | Kind | Source |'):
        seen[m.group(1)] += 1
    m = re.match(r'^- ([A-Z][A-Z0-9]*(?:-NF)?-\d+)[ :(.]', ln)
    if m: seen[m.group(1)] += 1
for k, v in seen.items():
    if v > 1: bad('spec-dup', k, v)
for s, info in sids.items():
    if info['kind'] == 'fr' and not info['withdrawn'] and not info['ms']:
        bad('spec-lands', s, info['lands'][:50])
print('SPEC ids', len(sids))

# 10. mermaid blocks
for m in re.finditer(r'```mermaid\n(.*?)```', doc, re.S):
    first = m.group(1).strip().split('\n')[0].strip()
    if not re.match(r'^(sequenceDiagram|stateDiagram-v2|flowchart (LR|TD|TB|RL)|graph (LR|TD|TB))', first):
        bad('mermaid', first)

print('test rows', len(rows), 'unique', len(def_ids))
print('PROBLEMS', sum(problems.values()), dict(problems))
