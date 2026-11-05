# SPDX-License-Identifier: Apache-2.0
"""T-GAP-008: the traceability check of TDD section 14.7 (SPEC requirement -> tests), run with the tier-1 scans.

It reads every test row of docs/TDD.md (ID | Tier | What it checks | Pass criterion | Covers | Lands), the requirement
tables of docs/SPEC.md and their Lands, the gate table of section 14.3 and Tests/ci/lands_terms.json, splits table rows
only on unescaped pipes ((?<!\\)\\|), and fails (section 14.6, T-GAP-008) when:
  1  a SPEC requirement that is not Later or Withdrawn has no test;
  2  a test ID repeats;
  3  a Covers cell names a requirement-shaped ID (a SPEC prefix) that is not in SPEC.md;
  4  a test row has no Lands;
  5  a test's first Lands is earlier than the first milestone of a requirement it covers and its Lands is not split;
  6  the first milestone of a requirement's Lands has no covering test whose first Lands is that milestone or one of
     whose split steps names it;
  7  a tier-8 test sits in a gate's automated suite instead of its manual checklist;
  8  a test in the automated suite of G1, G2 or G3 has a first Lands later than M1;
  9  a row's What-it-checks or Pass-criterion text matches a lands_terms.json term whose milestone is later than the
     row's first Lands, unless a split step names that milestone or a later one, or the exempt map lists the row.
The section 14.4 item 2 records (red-then-green or a reviewer attestation per new test) need the merge base of a version
control system; this repository has none, so that check reports itself inactive (tests B-note).
Requirements whose Lands is not a milestone (the NFR rows: Gate, Tracked, NFR) take part only in check 1.
"""

import json
import os
import re

MILESTONES = ["Phase 0", "M1", "M2", "M3", "M4", "1.0.x", "1.1", "1.2", "Later"]
RANK = {name: index for index, name in enumerate(MILESTONES)}
MILESTONE_RE = re.compile(r"Phase 0|M[1-4]|1\.0\.x|1\.[12]|Later")
PIPE_RE = re.compile(r"(?<!\\)\|")
TEST_ID_RE = re.compile(r"^T-[A-Z0-9]+-\d{3}$")
REQ_ID_RE = re.compile(r"^(?:[A-Z][A-Z0-9]*(?:-[A-Z]+)*-\d+|J\d+)$")
# Requirement-shaped tokens inside a Covers cell: "RUN-10", "SEC-NF-01", "J4". Decisions (D2, U19, RT-06, RV-01, PO-03,
# E7, N5), gates (G2(1)) and research references ("07a Q1", "08 §7.2", "§5.12") are not SPEC IDs; check 3 looks only at
# tokens whose prefix is one of SPEC's own prefixes.
COVERS_TOKEN_RE = re.compile(r"\b[A-Z][A-Z0-9]*(?:-[A-Z]+)*-\d+\b|\bJ\d+\b")
GATE_RE = re.compile(r"^G(\d)$")
NFR_BULLET_RE = re.compile(r"^- ([A-Z][A-Z0-9]*(?:-[A-Z]+)*-\d+)(?: \(|:)")


def split_row(line):
    """Cells of a Markdown table row, split on unescaped pipes only, stripped; None when the line is not a row."""
    if not line.startswith("|"):
        return None
    parts = PIPE_RE.split(line)
    if len(parts) < 3:
        return None
    return [cell.strip() for cell in parts[1:-1]]


def milestones_in(text):
    return MILESTONE_RE.findall(text or "")


def first_milestone(lands):
    """The milestone a Lands value starts with ("M1 (M4: trust step)" -> M1), or None (Gate, NFR, empty)."""
    match = MILESTONE_RE.match((lands or "").strip())
    return match.group(0) if match else None


def top_level_groups(text):
    """The contents of each top-level (...) group, nested parentheses kept: "M1 (M4: Live (docked) pass)" ->
    ["M4: Live (docked) pass"]."""
    groups, depth, start = [], 0, 0
    for index, char in enumerate(text):
        if char == "(":
            if depth == 0:
                start = index + 1
            depth += 1
        elif char == ")" and depth > 0:
            depth -= 1
            if depth == 0:
                groups.append(text[start:index])
    return groups


def split_steps(lands):
    """The milestones named in a test's split value: "M1 (M2: cell; M3: entry point)" -> ["M2", "M3"]."""
    first = first_milestone(lands)
    if first is None:
        return []
    rest = lands.strip()[len(first):]
    steps = []
    for inner in top_level_groups(rest):
        for part in inner.split(";"):
            if ":" not in part:
                continue
            head = part.split(":", 1)[0].strip()
            for name in milestones_in(head):
                if name not in steps:
                    steps.append(name)
    return steps


class Test:
    def __init__(self, line_number, cells):
        self.line = line_number
        self.id, self.tier, self.what, self.passes, self.covers, self.lands = cells
        self.first = first_milestone(self.lands)
        self.splits = split_steps(self.lands)
        self.tiers = [int(t) for t in re.findall(r"\d", self.tier)]

    def covered_ids(self):
        return COVERS_TOKEN_RE.findall(self.covers)


def read(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().splitlines()


def parse_spec(lines):
    """{id: lands or None} for every requirement row, the Later and Withdrawn sets of Appendix A.2, the ID prefixes."""
    requirements = {}
    later, withdrawn = set(), set()
    lands_column = None
    in_id_table = False
    for line in lines:
        # The section 8 NFRs other than PERF are bullets: "- REL-01 (G6): ...", "- A11Y-01: ...".
        bullet = NFR_BULLET_RE.match(line)
        if bullet:
            requirements.setdefault(bullet.group(1), None)
            continue
        cells = split_row(line)
        if cells is None:
            in_id_table = False
            continue
        if cells and cells[0] == "ID":
            in_id_table = True
            lands_column = cells.index("Lands") if "Lands" in cells else None
            continue
        if cells and cells[0] in ("Later", "Withdrawn"):
            target = later if cells[0] == "Later" else withdrawn
            for token in re.findall(r"\b[A-Z][A-Z0-9]*(?:-[A-Z]+)*-\d+\b", " ".join(cells[1:])):
                target.add(token)
            continue
        if not in_id_table or not cells or not REQ_ID_RE.match(cells[0]):
            continue
        lands = cells[lands_column] if lands_column is not None and lands_column < len(cells) else None
        requirements[cells[0]] = lands
    for token in withdrawn:
        requirements.setdefault(token, "Withdrawn")
    for token, lands in requirements.items():
        if lands and lands.strip().startswith("Later"):
            later.add(token)
    prefixes = set()
    for token in requirements:
        prefixes.add(token.rsplit("-", 1)[0] if "-" in token else re.sub(r"\d+$", "", token))
    return requirements, later, withdrawn, prefixes


def parse_tdd(lines):
    """(tests, 14.7 lands, 14.3 gates, malformed): a row that starts with a test ID but does not have the six cells of a
    test row (ID | Tier | What it checks | Pass criterion | Covers | Lands) is malformed, not skipped (review tests r1)."""
    tests = []
    malformed = []
    trace_lands = {}
    gates = {}
    section = None
    for number, line in enumerate(lines, 1):
        if line.startswith("### "):
            section = line[4:].strip()
        cells = split_row(line)
        if cells is None:
            continue
        if cells and TEST_ID_RE.match(cells[0]) and len(cells) == 6:
            tests.append(Test(number, cells))
        elif cells and TEST_ID_RE.match(cells[0]):
            malformed.append((number, cells[0], len(cells)))
        elif section and section.startswith("14.7") and len(cells) == 3 and REQ_ID_RE.match(cells[0]):
            trace_lands[cells[0]] = cells[1]
        elif section and section.startswith("14.3") and len(cells) == 5 and GATE_RE.match(cells[0]):
            automated = re.findall(r"T-[A-Z0-9]+-\d{3}", cells[2])
            manual = re.findall(r"T-[A-Z0-9]+-\d{3}", cells[4])
            gates[cells[0]] = (automated, manual)
    return tests, trace_lands, gates, malformed


def term_matches(term, text):
    if term.get("whole_word"):
        return re.search(r"\b" + re.escape(term["term"]) + r"\b", text) is not None
    if term.get("case_sensitive"):
        return term["term"] in text
    return term["term"].lower() in text.lower()


def check(tdd_path, spec_path, terms_path):
    """(summary lines, violations)."""
    tdd = read(tdd_path)
    spec = read(spec_path)
    requirements, later, withdrawn, prefixes = parse_spec(spec)
    tests, trace_lands, gates, malformed = parse_tdd(tdd)
    with open(terms_path, "r", encoding="utf-8") as handle:
        terms_doc = json.load(handle)
    terms = terms_doc.get("terms", [])
    exempt = terms_doc.get("exempt", {})
    rel_tdd = os.path.basename(tdd_path)
    violations = []

    def at(test, message):
        violations.append("%s:%d: %s %s" % (rel_tdd, test.line, test.id, message))

    # Lands of each requirement: SPEC's own column, else the 14.7 table (the NFR tables have no Lands column).
    req_lands = {}
    for token, lands in requirements.items():
        req_lands[token] = lands if lands is not None else trace_lands.get(token)
    skipped = later | withdrawn

    # 1b. malformed test rows: a missing cell or an unescaped pipe would otherwise drop the row from every check below.
    for number, test_id, cell_count in malformed:
        violations.append("%s:%d: %s test row has %d cells, not 6 (ID | Tier | What it checks | Pass criterion | Covers | Lands): "
                          "a missing cell or an unescaped '|'" % (rel_tdd, number, test_id, cell_count))

    # 2. repeated IDs.
    seen = {}
    for test in tests:
        if test.id in seen:
            at(test, "repeats the test ID first used at line %d" % seen[test.id])
        else:
            seen[test.id] = test.line

    covering = {}
    for test in tests:
        # 4. Lands present.
        if not test.lands:
            at(test, "has no Lands")
        elif test.first is None:
            at(test, "has a Lands that names no milestone: %r" % test.lands)
        for token in test.covered_ids():
            prefix = token.rsplit("-", 1)[0] if "-" in token else re.sub(r"\d+$", "", token)
            if token in requirements:
                covering.setdefault(token, []).append(test)
            elif prefix in prefixes:
                # 3. a requirement-shaped ID that SPEC.md does not define.
                at(test, "covers %s, which is not a requirement in SPEC.md" % token)

    # 1, 5, 6.
    for token, lands in sorted(requirements.items()):
        if token in skipped:
            continue
        tested = covering.get(token, [])
        if not tested:
            violations.append("%s: requirement %s (Lands %s) has no test" % (os.path.basename(spec_path), token, req_lands.get(token)))
            continue
        req_first = first_milestone(req_lands.get(token) or "")
        if req_first is None or req_first == "Later":
            continue
        for test in tested:
            if test.first is not None and RANK[test.first] < RANK[req_first] and not test.splits:
                at(test, "Lands %s is earlier than %s's first milestone %s and is not split" % (test.first, token, req_first))
        if not any(test.first == req_first or req_first in test.splits for test in tested):
            violations.append("%s: requirement %s lands in %s, but no covering test lands there or names it in a split step (%s)"
                              % (os.path.basename(spec_path), token, req_first, ", ".join(t.id for t in tested)))

    # 7, 8: the gate suites of 14.3.
    by_id = {test.id: test for test in tests}
    for gate, (automated, manual) in sorted(gates.items()):
        for test_id in automated:
            test = by_id.get(test_id)
            if test is None:
                violations.append("%s: gate %s lists %s, which has no test row" % (rel_tdd, gate, test_id))
                continue
            if 8 in test.tiers and len(set(test.tiers)) == 1:
                at(test, "is tier 8 but sits in %s's automated suite, not its manual checklist" % gate)
            if gate in ("G1", "G2", "G3") and test.first is not None and RANK[test.first] > RANK["M1"]:
                at(test, "is in %s's automated suite but first lands in %s (after M1)" % (gate, test.first))
        for test_id in manual:
            test = by_id.get(test_id)
            if test is not None and 8 not in test.tiers:
                at(test, "is in %s's manual checklist but is not tier 8" % gate)

    # 9. lands terms.
    for test in tests:
        if test.first is None or test.id in exempt:
            continue
        text = test.what + " " + test.passes
        for term in terms:
            milestone = term["milestone"]
            if RANK[milestone] <= RANK[test.first]:
                continue
            if not term_matches(term, text):
                continue
            if any(RANK[step] >= RANK[milestone] for step in test.splits):
                continue
            at(test, "mentions %r (%s) but first lands in %s with no split step at %s or later" % (term["term"], milestone, test.first, milestone))

    # Summary: per milestone, the tests and split steps its exit requires (14.3, 15.2).
    summary = []
    journeys = [token for token in requirements if token.startswith("J") and token[1:].isdigit()]
    ids = [token for token in requirements if token not in journeys]
    required = sum(1 for token in ids if token not in skipped)
    summary.append("SPEC: %d requirement IDs and %d journeys; %d Later or Withdrawn; %d required, all tested unless listed below; "
                   "TDD: %d test rows, %d IDs" % (len(ids), len(journeys), len(skipped & set(ids)), required, len(tests), len(seen)))
    for milestone in MILESTONES:
        firsts = [t for t in tests if t.first == milestone]
        steps = [t for t in tests if milestone in t.splits]
        if firsts or steps:
            summary.append("  %-7s exit: %d tests whose first Lands is %s, %d split steps" % (milestone, len(firsts), milestone, len(steps)))
    conditional = [t for t in ("T-CORE-036", "T-VIEW-019") if t in by_id]
    if conditional:
        summary.append("  conditional on their 4.15 fallback (required only once it is invoked): %s" % ", ".join(conditional))
    summary.append("  14.4 item 2 records: inactive (no version control merge base in this repository)")
    return summary, violations, tests


def milestone_listing(tests, milestone):
    """Every test (and split step) the exit of milestone requires, for --list."""
    out = []
    for test in tests:
        if test.first == milestone:
            out.append("%s  %s" % (test.id, test.lands))
        elif milestone in test.splits:
            out.append("%s  %s  (split step)" % (test.id, test.lands))
    return out
