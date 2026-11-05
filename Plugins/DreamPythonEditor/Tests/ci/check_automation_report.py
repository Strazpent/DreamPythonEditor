# SPDX-License-Identifier: Apache-2.0
"""Reads the automation report a tier-4 pass exported (-ReportExportPath=<dir>, file <dir>/index.json) and turns it into
an exit code, because the editor's "Automation ... ;Quit" exit status does not carry the test result.

The report is the engine's FAutomatedTestPassResults serialized with FJsonObjectConverter (camelCase keys: "tests",
each with "fullTestPath", "state" and "entries"; AutomationControllerManager.h). Only tests whose fullTestPath starts
with --prefix count; --prefix takes several prefixes joined with '+', as the engine's "Automation RunTests A+B" does
(run_tier4.bat --filter Dream.Keys+Dream.Output.Follow). The pass fails when any counted test failed or did not finish,
when none ran, or when a '+' part or a --require prefix matched no test that ran.

It also fails when the tests' user-settings guard (Support/DreamTestSteps.cpp) flagged a key: an Error "user setting
<Key> changed by <test>: <old> -> <new>", or "... changed by an automation run that did not finish ..." for a baseline a
killed run left behind. The guard logs at a test's end, after the engine has set the test's state, so the entry can sit
in a passing test's report. The checker looks for it in the entries of the counted tests and in the "LogDream: Error: "
lines of <report dir>/editor.log (which run_tier4.bat writes with -abslog), where it is also found for a test that does
not capture logs. The guard's warning for a change only on disk (another editor saved the file) does not fail the pass.

Usage: python.exe -I -B check_automation_report.py <report dir> [--prefix Dream Python Editor] [--require <prefix>]...
Exit code 0 pass, 1 test failures, missing tests or a flagged user setting, 2 no readable report.
"""

import argparse
import json
import os
import sys

# EAutomationState as the converter writes it: the enum name, or its value in older reports.
STATE_NAMES = {0: "NotRun", 1: "InProcess", 2: "Fail", 3: "Success", 4: "Skipped"}


def state_of(test):
    state = test.get("state")
    if isinstance(state, int):
        return STATE_NAMES.get(state, str(state))
    return str(state or "NotRun")


def error_messages(test):
    messages = []
    for entry in test.get("entries") or []:
        event = entry.get("event") or {}
        if str(event.get("type", "")).lower() == "error":
            messages.append(str(event.get("message", "")).strip())
    return messages


GUARD_MARK = "user setting "
GUARD_MARK_2 = " changed by "


def guard_message(text):
    """The guard's text in a report entry or log line ("LogDream: Error: user setting ..."), or None."""
    start = text.find(GUARD_MARK)
    if start < 0 or GUARD_MARK_2 not in text[start:]:
        return None
    return text[start:].strip()


def guard_flags(tests, report_dir):
    flags = []
    for test in tests:
        for entry in test.get("entries") or []:
            message = guard_message(str((entry.get("event") or {}).get("message", "")))
            if message is not None and message not in flags:
                flags.append(message)
    try:
        with open(os.path.join(report_dir, "editor.log"), "r", encoding="utf-8-sig", errors="replace") as handle:
            for line in handle:
                if "LogDream: Error: " in line:
                    message = guard_message(line)
                    if message is not None and message not in flags:
                        flags.append(message)
    except OSError:
        pass
    return flags


def main(argv):
    # Test names and messages may hold characters a cp1252 console cannot encode ('§'); a passing run must not fail on
    # printing them.
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report_dir")
    parser.add_argument("--prefix", default="Dream",
                        help="count tests whose full path starts with this; several prefixes are joined with '+', and "
                             "each must match a test that ran")
    parser.add_argument("--require", action="append", default=[], help="a prefix at least one test that ran must match")
    args = parser.parse_args(argv)

    index = os.path.join(args.report_dir, "index.json")
    try:
        with open(index, "r", encoding="utf-8-sig") as handle:
            report = json.load(handle)
    except (OSError, ValueError) as error:
        print("check_automation_report: no readable report at %s (%s); see %s"
              % (index, error, os.path.join(args.report_dir, "editor.log")))
        return 2

    prefixes = tuple(p.strip() for p in args.prefix.split("+") if p.strip()) or ("Dream",)
    tests = [t for t in report.get("tests") or [] if str(t.get("fullTestPath", "")).startswith(prefixes)]
    counts = {}
    problems = []
    for test in sorted(tests, key=lambda t: str(t.get("fullTestPath", ""))):
        path = str(test.get("fullTestPath", ""))
        state = state_of(test)
        counts[state] = counts.get(state, 0) + 1
        warnings = int(test.get("warnings") or 0)
        print("%-8s %s%s" % (state, path, " (%d warnings)" % warnings if warnings else ""))
        if state in ("Fail", "InProcess", "NotRun"):
            problems.append(path)
            for message in error_messages(test):
                print("         error: %s" % message)

    for message in guard_flags(tests, args.report_dir):
        print("check_automation_report: %s" % message)
        problems.append(message)

    ran = [t for t in tests if state_of(t) in ("Success", "Fail")]
    # The engine runs each '+' part on its own, so a part that matched nothing (a typo) must fail as a lone prefix does.
    required = list(args.require) + (list(prefixes) if len(prefixes) > 1 else [])
    for prefix in required:
        if not any(str(t.get("fullTestPath", "")).startswith(prefix) for t in ran):
            print("check_automation_report: no test under %s ran" % prefix)
            problems.append(prefix)
    if not ran:
        print("check_automation_report: no %s test ran" % args.prefix)
        problems.append(args.prefix)

    summary = ", ".join("%d %s" % (n, s) for s, n in sorted(counts.items())) or "no tests"
    print("check_automation_report: %s (%s)" % ("FAILED" if problems else "passed", summary))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
