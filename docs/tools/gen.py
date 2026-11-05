"""Regenerate the generated parts of the assembled TDD.md in place (TDD v1.2).

Adapted from tddwork/generate.py, which filled <!--...--> placeholders in the fragment assembly.
The assembled document has no placeholders, so each generated region is found by its heading or
table header and replaced. Re-runnable. Table rows are split on unescaped pipes only (QA-F11).
Generated: the Contents list, the 14.1 area-code table, the 14.3 gate suites (automated suite and
manual checklist), the 14.7 summary and table, the 17.3 gap paragraph, Appendix A and Appendix B
(D1-D30, RT-01..RT-20, U1-U41, RV-01..RV-24 and PO-01..PO-03).
"""
import re, sys, collections, json
sys.stdout.reconfigure(encoding='utf-8')
sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else __file__.rsplit('/', 1)[0])
import lib

COVERS, LANDS = 4, 5


def link(title, slugs):
    return f'[{title}](#{slugs[title]})'


def replace_table(lines, header_prefix, new_rows, after=None):
    """Replace the table whose header row starts with header_prefix (the first one after line index `after`)."""
    start = None
    for i, ln in enumerate(lines):
        if after is not None and i < after: continue
        if ln.startswith(header_prefix):
            start = i; break
    assert start is not None, header_prefix
    end = start
    while end < len(lines) and lines[end].startswith('|'): end += 1
    lines[start:end] = new_rows
    return lines


def find_line(lines, pred, after=0):
    for i in range(after, len(lines)):
        if pred(lines[i]): return i
    raise KeyError


def collect_tests(doc):
    tests = collections.OrderedDict(); dups = []
    for i, tid, c, st in lib.test_rows(doc):
        if tid in tests: dups.append(tid)
        tests[tid] = dict(tier=c[1], covers=c[COVERS] if len(c) > COVERS else '',
                          lands=c[LANDS] if len(c) > LANDS else '', top=st.get(2), sec=st.get(3) or st.get(2),
                          ncells=len(c), line=i + 1)
    return tests, dups


def fill(doc, spec_text):
    sids = lib.spec_ids(spec_text)
    idre = lib.spec_id_regex(sids)
    tests, dups = collect_tests(doc)
    assert not dups, dups
    hs = lib.headings(doc)
    slugs = {t: s for _, t, s, _ in hs}
    lines = doc.split('\n')

    # ---- 14.1 area codes
    area = collections.OrderedDict()
    for tid, t in tests.items():
        a = re.sub(r'-\d+[a-z]?$', '', tid)[2:]
        e = area.setdefault(a, dict(n=0, tops=[]))
        e['n'] += 1
        if t['top'] not in e['tops']: e['tops'].append(t['top'])
    rows = ['| Code | Tests | Defined in |', '|---|---|---|']
    for a, e in area.items():
        rows.append(f"| `{a}` | {e['n']} | " + ', '.join(link(x, slugs) for x in e['tops']) + ' |')
    # SPEC areas whose requirements are all Later have no test area yet (UX-F07: TST)
    test_areas = set(area)
    for sa in sorted({re.sub(r'-\d+$', '', s) for s, i in sids.items() if i['kind'] == 'fr'}):
        ids = [s for s in sids if re.sub(r'-\d+$', '', s) == sa]
        if sa not in test_areas and all(sids[s]['later'] for s in ids) and not any(
                re.search(r'(?<![A-Za-z0-9-])' + sa + r'-\d+', t['covers']) for t in tests.values()):
            rows.append(f'| `{sa}` | 0 | — (SPEC {", ".join(ids)} lands Later) |')
    lines = replace_table(lines, '| Code | Tests | Defined in |', rows)

    # ---- 14.3 gate suites
    def find_heading(prefix):
        for _, t, s, _ in hs:
            if t.startswith(prefix): return t
        raise KeyError(prefix)
    owners = {'G1': find_heading('4.15 '), 'G2': find_heading('5.15.1 '), 'G3': find_heading('7.10.1 '),
              'G4': find_heading('8.9.1 '), 'G5': find_heading('4.16.1 '), 'G6': find_heading('10.10 '),
              'G7': find_heading('4.16.2 ')}
    rows = ['| Gate | Owning section | Automated suite (tiers 1–7) | Count | Manual checklist (tier 8) |',
            '|---|---|---|---|---|']
    suites = {}
    for g in ['G1', 'G2', 'G3', 'G4', 'G5', 'G6', 'G7']:
        ts = [tid for tid, t in tests.items() if re.search(r'(?<![A-Za-z0-9])' + g + r'(?![0-9])', t['covers'])]
        man = [x for x in ts if [p.strip() for p in tests[x]['tier'].split(',')] == ['8']]
        auto = [x for x in ts if x not in man]
        suites[g] = dict(auto=auto, manual=man)
        rows.append(f'| {g} | {link(owners[g], slugs)} | ' + ', '.join(auto) + f' | {len(auto)} | '
                    + (', '.join(man) if man else '—') + ' |')
    lines = replace_table(lines, '| Gate | Owning section |', rows)

    # ---- 14.7 traceability
    cover = collections.defaultdict(list); unknown = collections.Counter()
    for tid, t in tests.items():
        for sid in idre.findall(t['covers']):
            if sid in sids:
                if tid not in cover[sid]: cover[sid].append(tid)
            else:
                unknown[sid] += 1
    rows = ['| SPEC ID | Lands | Tests |', '|---|---|---|']
    gaps = []; n_ok = n_later = 0
    for sid, info in sids.items():
        ts = cover.get(sid, [])
        lands = info['lands'] if info['kind'] == 'fr' else (info['pkind'] or 'NFR')
        lands = re.sub(r'(?<!\\)\|', r'\\|', lands) or 'NFR'
        if info['withdrawn']:
            note = 'Withdrawn: no test'
        elif info['later'] and not ts:
            note = 'Later: no 1.0 test'
        elif ts:
            note = ', '.join(ts)
        else:
            note = '**no test**'; gaps.append(sid)
        if ts and not info['withdrawn']: n_ok += 1
        if info['later'] or info['withdrawn']: n_later += 1
        rows.append(f'| {sid} | {lands} | {note} |')
    i147 = find_line(lines, lambda l: l.startswith('### 14.7 '))
    lines = replace_table(lines, '| SPEC ID | Lands | Tests |', rows, after=i147)
    summ = (f'**Summary.** SPEC.md has {len(sids)} requirement IDs; {n_ok} have at least one test; '
            f'{n_later} are Later or Withdrawn; {len(gaps)} required IDs have no test'
            + (f" ({', '.join(gaps)})" if gaps else '') + f'. The document holds {len(tests)} test IDs, all unique.')
    k = find_line(lines, lambda l: l.startswith('**Summary.** SPEC.md has'), i147)
    lines[k] = summ

    # ---- 17.3 gaps
    i173 = find_line(lines, lambda l: l.startswith('### 17.3 '))
    k = find_line(lines, lambda l: l.strip() != '', i173 + 1)
    if gaps:
        lines[k] = ' '.join(f'{x}: no test yet; owner QA; closes before its Lands milestone exit.' for x in gaps)
    else:
        lines[k] = 'None. Every SPEC v1.2 requirement that is not Later or Withdrawn has at least one test (§14.7).'

    doc = '\n'.join(lines)

    # ---- Appendix A
    rows = ['| # | Failure | Section |', '|---|---|---|']; n = 0; in_fm = False
    for i, ln, fence, st in lib.walk(doc):
        if fence: in_fm = False; continue
        if (st.get(2) or '').startswith('Appendix'): break
        if ln.startswith('| Failure | Detection |'):
            in_fm = True; continue
        if in_fm:
            if not ln.startswith('|'): in_fm = False; continue
            if ln.startswith('|---'): continue
            c = lib.cells(ln)
            n += 1
            sec = st.get(4) or st.get(3) or st.get(2)
            rows.append(f'| A-{n:03d} | {c[0]} | {link(sec, slugs)} |')
    lines = doc.split('\n')
    ia = find_line(lines, lambda l: l.startswith('## Appendix A.'))
    lines = replace_table(lines, '| # | Failure | Section |', rows, after=ia)
    doc = '\n'.join(lines)

    # ---- Appendix B
    dec = collections.defaultdict(collections.Counter)
    pat = re.compile(r'(?<![A-Za-z0-9_-])(D(?:[1-9]|[12][0-9]|30)|RT-(?:0[1-9]|1[0-9]|20)|U(?:[1-9]|[1-3][0-9]|4[01])'
                     r'|RV-(?:0[1-9]|1[0-9]|2[0-4])|PO-0[1-3])(?![0-9A-Za-z_])')
    for i, ln, fence, st in lib.walk(doc):
        top = st.get(2) or ''
        if not re.match(r'^\d+\. ', top) or top.startswith('17.'): continue
        sec = st.get(3) or top
        if not lib.secnum(sec): continue
        if lib.TID_RE.match(ln): continue   # test rows: index design text only
        for d in pat.findall(ln):
            dec[d][sec] += 1

    rows = ['| Decision | Sections (most citations first) |', '|---|---|']
    allids = ([f'D{i}' for i in range(1, 31)] + [f'RT-{i:02d}' for i in range(1, 21)]
              + [f'U{i}' for i in range(1, 42)] + [f'RV-{i:02d}' for i in range(1, 25)]
              + [f'PO-{i:02d}' for i in range(1, 4)])
    notcited = []
    for d in allids:
        c = dec.get(d)
        if not c:
            rows.append(f'| {d} | not cited |'); notcited.append(d); continue
        top8 = sorted(c.items(), key=lambda kv: (-kv[1], [int(x) for x in lib.secnum(kv[0]).split('.')]))[:8]
        rows.append(f'| {d} | ' + ', '.join(f'[§{lib.secnum(s)}](#{slugs[s]})' for s, _ in top8) + ' |')
    lines = doc.split('\n')
    ib = find_line(lines, lambda l: l.startswith('## Appendix B.'))
    lines = replace_table(lines, '| Decision | Sections (most citations first) |', rows, after=ib)
    k = find_line(lines, lambda l: l.startswith('For each decision (D1–D30)'), ib)
    lines[k] = ('For each decision (D1–D30), amendment (RT-01..RT-20), UX ruling (U1–U41) and review amendment '
                '(RV-01..RV-24, 08 §9) and owner ruling (PO-01..PO-03, 08 §10): the design sections that cite it, most citations first (at most eight), '
                'counted per subsection. Test-table rows are not counted, so the index points at design text; '
                '§14 lists the tests. Generated from the text.')
    doc = '\n'.join(lines)

    # ---- Contents (last, so every heading is in it)
    hs = lib.headings(doc)
    toc = []; started = False
    for lvl, t, s, _ in hs:
        if t.startswith('1. Overview'): started = True
        if not started or lvl not in (2, 3): continue
        toc.append(('  ' if lvl == 3 else '') + f'- [{t}](#{s})')
    lines = doc.split('\n')
    ic = find_line(lines, lambda l: l == '## Contents')
    a = find_line(lines, lambda l: l.startswith('- ['), ic)
    b = a
    while b < len(lines) and (lines[b].startswith('- [') or lines[b].startswith('  - [')): b += 1
    lines[a:b] = toc
    doc = '\n'.join(lines)

    report = dict(tests=len(tests), gaps=gaps, unknown=dict(unknown), spec_ids=len(sids), covered=n_ok,
                  later_or_withdrawn=n_later, failure_rows=n, headings=len(hs), toc=len(toc),
                  suites={g: (len(v['auto']), v['manual']) for g, v in suites.items()}, appb_not_cited=notcited)
    return doc, report


if __name__ == '__main__':
    doc = lib.read(lib.TDD)
    out, rep = fill(doc, lib.read(lib.SPEC))
    if '--write' in sys.argv:
        lib.write(lib.TDD, out)
    print(json.dumps(rep, indent=1, ensure_ascii=False))
