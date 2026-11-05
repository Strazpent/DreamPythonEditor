"""Shared helpers for docs/tools/gen.py and docs/tools/check.py."""
import re, collections, os

DOCS = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace(chr(92), '/')
TDD = os.environ.get('TDD_PATH', DOCS + '/TDD.md')
SPEC = DOCS + '/SPEC.md'
DLOG = DOCS + '/research/08-decision-log.md'

ORDER = ['Phase 0', 'M1', 'M2', 'M3', 'M4', '1.0.x', '1.1', '1.2', 'Later']
RANK = {m: i for i, m in enumerate(ORDER)}
MS_RE = re.compile(r'(Phase 0|\bM[1-4]\b|1\.0\.x|\b1\.1\b|\b1\.2\b|\bLater\b)')

TEST_HDR = '| ID | Tier | What it checks | Pass criterion | Covers |'
TEST_HDR_L = '| ID | Tier | What it checks | Pass criterion | Covers | Lands |'
TID_RE = re.compile(r'^\| *`?(T-[A-Z0-9]+-\d+[a-z]?)`? *\|')


def read(p):
    return open(p, encoding='utf-8').read().replace('\r\n', '\n')


def write(p, t):
    open(p, 'w', encoding='utf-8', newline='\n').write(t)


def cells(line):
    """Split a table row on unescaped pipes only (QA-F11). Returns stripped cells."""
    s = line.strip()
    if s.startswith('|'): s = s[1:]
    if s.endswith('|') and not s.endswith('\\|'): s = s[:-1]
    out, buf, i = [], [], 0
    in_code = False
    while i < len(s):
        ch = s[i]
        if ch == '\\' and i + 1 < len(s) and s[i + 1] == '|':
            buf.append('\\|'); i += 2; continue
        if ch == '|':
            out.append(''.join(buf).strip()); buf = []
        else:
            buf.append(ch)
        i += 1
    out.append(''.join(buf).strip())
    return out


def ncells(line):
    return len(cells(line))


def slug(text):
    s = text.strip().lower()
    s = re.sub(r'[^\w\- ]', '', s)
    return s.replace(' ', '-')


def walk(doc):
    """Yield (line_no, line, in_fence, heading_stack) for every line."""
    fence = False
    stack = {}
    for i, ln in enumerate(doc.split('\n')):
        if ln.startswith('```'):
            fence = not fence
            yield i, ln, True, dict(stack); continue
        if not fence:
            m = re.match(r'^(#{1,6}) (.*)$', ln)
            if m:
                lvl = len(m.group(1)); stack[lvl] = m.group(2).strip()
                for k in list(stack):
                    if k > lvl: del stack[k]
        yield i, ln, fence, dict(stack)


def headings(doc):
    out = []; seen = collections.Counter()
    for i, ln, fence, st in walk(doc):
        if fence: continue
        m = re.match(r'^(#{1,6}) (.*)$', ln)
        if m:
            t = m.group(2).strip(); s = slug(t)
            n = seen[s]; seen[s] += 1
            out.append((len(m.group(1)), t, s if n == 0 else f'{s}-{n}', i))
    return out


def secnum(title):
    m = re.match(r'^(\d+(?:\.\d+)*)\.? ', title)
    return m.group(1) if m else None


def milestones(text):
    """Milestones named in a Lands value, in order of appearance (deduplicated)."""
    out = []
    for m in MS_RE.findall(text or ''):
        if m not in out: out.append(m)
    return out


def spec_ids(spec_text):
    """Requirement IDs of SPEC.md: rows of the requirement tables and the PERF table, plus NFR bullets.
    Rows of other tables (Appendix D change logs, journeys) are not requirement definitions."""
    ids = collections.OrderedDict()
    hdr = None; prev = ''
    for ln in spec_text.split('\n'):
        if ln.startswith('|') and not prev.startswith('|'):
            hdr = ln.strip()
        if not ln.startswith('|'):
            hdr = None
        prev = ln
        m = re.match(r'^\| *([A-Z][A-Z0-9]*(?:-NF)?-\d+) *\|', ln)
        if m and hdr in ('| ID | Requirement | Lands | Acceptance criteria |', '| ID | Budget | Target | Kind | Source |'):
            c = cells(ln)
            sid = c[0]
            if hdr.startswith('| ID | Budget'):
                lands, kind, pkind = '', 'nfr', c[3]
            else:
                lands, kind, pkind = c[2], 'fr', ''
            withdrawn = c[1].startswith('**Withdrawn**')
            ms = milestones(lands)
            ids[sid] = dict(lands=lands, withdrawn=withdrawn, later=bool(ms) and ms[0] == 'Later',
                            ms=ms, kind=kind, pkind=pkind)
            continue
        m = re.match(r'^- ([A-Z][A-Z0-9]*(?:-NF)?-\d+)[ :(]', ln)
        if m and m.group(1) not in ids and not m.group(1).startswith(('GL-', 'NG-', 'DEP-')):
            ids[m.group(1)] = dict(lands='NFR', withdrawn=False, later=False, ms=[], kind='bullet', pkind='')
    return ids


def spec_id_regex(sids):
    areas = sorted({re.sub(r'-\d+$', '', s) for s in sids}, key=len, reverse=True)
    return re.compile(r'(?<![A-Za-z0-9-])(?:' + '|'.join(re.escape(a) for a in areas) + r')-\d+\b')


def test_rows(doc):
    """(line_index, test_id, cells, heading_stack) for every row of a test table."""
    out = []
    hdr = None; prev = ''
    for i, ln, fence, st in walk(doc):
        if fence: hdr = None; prev = ''; continue
        if ln.startswith('|') and not prev.startswith('|'):
            hdr = ln.strip()
        if not ln.startswith('|'): hdr = None
        prev = ln
        if hdr in (TEST_HDR, TEST_HDR_L):
            m = TID_RE.match(ln)
            if m:
                out.append((i, m.group(1), cells(ln), st))
    return out
