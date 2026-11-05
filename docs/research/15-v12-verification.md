> Historical record: written when the product was named Sumi; the product is now "Dream Python Editor" (owner ruling PO-06 phase 3).

# 15 — Verification of the owner rulings in v1.2 (PO-01 – PO-03)

> Record of the two verification rounds run on the v1.2 patch that applies the owner's request "output should be on the top and there should be line by line execution and realtime execution similar to maya's script editor" ([08 §10](08-decision-log.md#10-owner-rulings-after-v11-po-01--po-03)). In each lens a verifier checked the patch against the rulings, the rest of the documents and the local UE 5.8 install, and an independent refuter tried to refute every issue and added any it found missing. One patcher per round applied every surviving issue, re-ran the self-check (`tools/check.py`, PROBLEMS 0) and re-rendered the mockups it changed. Round 2 checked only round 1's fix patch, as v1.1 taught ([14](14-v11-verification.md)). Neither round found a critical issue; round 1 found 11 major issues and round 2 none, so the loop stopped. Where the fixes refined the first wording of a ruling, 08 §10 says so in its "Refined when v1.2 was verified" paragraph. Section numbers and quoted text refer to the documents as they stood at that round; paths under "scratchpad" were the verifiers' working folder and are not in the repository.

## Summary

| Round | What it checked | Confirmed | Critical | Major | Minor | Refuted | Added by refuter | Skipped in part |
|---|---|---|---|---|---|---|---|---|
| [1](#round-1) | the PO-01..PO-03 patch (SPEC, TDD, 08 §10, README, all ten mockups), in three lenses: runtime and engine, consistency and delivery, UX and mockups | 51 | 0 | 11 | 40 | 5 | 7 | 8 |
| [2](#round-2) | the round-1 fix patch only, from an exact before/after diff, in two lenses: runtime and consistency, UX and mockups | 28 | 0 | 0 | 28 | 0 | 4 | 1 |

## Round 1

Scope: the PO-01..PO-03 patch (SPEC, TDD, 08 §10, README, all ten mockups), in three lenses: runtime and engine, consistency and delivery, UX and mockups.

**Patch.** I applied every confirmed issue and the four architect items to SPEC.md, TDD.md, README.md, research/08-decision-log.md (the header Date row and section 10 only), mockups 01, 02, 04 and 07 and ue.css, and re-rendered images 01, 02, 04 and 07. I read each re-rendered image and fixed the problems I saw: legend text cut off in 01 and 04 (taller canvases), and callout 6 crowding the hint chip in 01. Then I ran gen.py --write and check.py, which printed PROBLEMS 0. Before-patch copies are in scratchpad/owner1/verify/before-patch/, and my scripts and probe outputs are in .../owner1/verify/patcher/ (q1_annotations.out, q2_extent_sim.out).

Some fixes depart from the brief. Each is a confirmed fix, and each is recorded in a new "Refined when v1.2 was verified" paragraph in 08 section 10 and in its PO rows:
(1) The Run window's 480 x 200 minimum, which the brief kept, is replaced by a per-dimension minimum: 480 wide, and a height of the other rows plus 5 live-view lines, about 280 (proposed). At 200 px the live view had no room for a line.
(2) "Nothing jumps" is now met by follow state. The live view and the Output view differ in height, so the literal promise could not hold.
(3) The brief's `_sumi.echo` plus `exec` became one C call, `_sumi.run_statement`.
(4) Per A1, ran-line marks keep an Alt+Enter trail and no longer belong to the buffer's last run.
(5) The Alt+Enter engine claim was corrected: the PCG plugin has a tool-local binding.

Evidence I checked myself: SWindow.h:248-255, SWindow.cpp:193-196, 280-290, 319-331, 365 and 1603-1606; MenuStack.cpp:494-501; WindowsApplication.cpp:2330-2360, 3164, 3318-3327 and 4186-4200; SPCGCodeEditorTextBox.cpp:57-69 and PCG.uplugin:13.

A probe on the engine's 3.11.8 showed that SETUP_ANNOTATIONS is emitted for any module-level AnnAssign, including attribute targets and AnnAssigns inside compound bodies, but not inside a def or class body. That is why the TDD tests the code object instead of looking for "simple targets".

A simulation of the patched extent rules (one-line headers, the decorator walk-up, clause skipping) over 2,527 compiling engine Lib files found 0 mismatches against ast across 176,107 carets, so T-CORE-039 can now pass.

**Checks after the patch.** I ran `python tools/gen.py --write` and then `python tools/check.py` from the docs folder after the last edit. It printed PROBLEMS 0: 369 headings, 1551 links, 252 SPEC IDs, 842 unique test rows. Appendix A renumbered A-079 onwards because of the new 5.14 row; no doc text cites A numbers.

All edited files stay LF with no BOM. 08-decision-log.md differs from its backup only at line 6 (Date) and section 10 (lines 1772-1775).

I grepped SPEC, TDD, README, 08 and the mockups for every stale term and found none left in the current text: `_sumi.echo` (one intentional historical mention in the 08 refinement note), `echo(run_id`, `prep.completed`, 'replaces the set', '480 × 200', 'nothing jumps', 'Slate command binds', 'fits the rows below', 'within one pump', 'anchored at', 'never switches tabs', 'echoed with full code', 'writes chords for every command'.

Engine lines I opened: SWindow.h:248-305, SWindow.cpp:193-216, 280-380 and 1603-1606, MenuStack.cpp:488-504, WindowsApplication.cpp:2330-2362, 3160-3166, 3318-3330 and 4186-4200, SPCGCodeEditorTextBox.cpp:55-70, PCG.uplugin:5-14, GameViewportClient.cpp:670-678.

Probes on the engine's python.exe -I -B (3.11.8), output under scratchpad/owner1/verify/patcher/:
- q1_annotations: when SETUP_ANNOTATIONS is emitted.
- q2_extent_sim: the patched extent rules on synthetic one-line, decorator and walk cases, and T-CORE-039 over Lib: 0 mismatches in 176,107 carets across 2,527 compiling files.

Re-rendered with mockups\render.ps1 -Only: 01-main-window (canvas raised to 1716 px so the legend fits; callout 6 moved below the squiggle), 02-run-and-cancel, 04-traceback-to-source (canvas raised to 1282 px), 07-snippets-and-shelves. I read every re-rendered PNG and cropped detail views of 01 (gutter lanes, line 17 chip, legend), 02, 04 (legend) and 07 (trail); no overlaps or clipped text remain.

Not done: no independent verify/refute round has been run on this patch yet. Project memory says each patch round should be re-verified from the exact diff against verify/before-patch/.

### Round 1 — Runtime, engine and consistency

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [P1-runtime-04](#p1-runtime-04) | major | algorithm bug | confirmed | A compound statement whose header holds its body on the same line, followed by a clause (`try: import x` / `except ImportError: x = None`, `if c: a()` / `else: b()`), does not end with ':', so rule 6 gives only the … |
| [P1-runtime-05](#p1-runtime-05) | major | algorithm bug | confirmed | The extent starts at the caret's logical line, and the decorator rule only runs downwards. |
| [P1-runtime-08](#p1-runtime-08) | major | design flaw | confirmed | The echo pushes whole snapshot lines first_line..last_line for each statement. |
| [P1-runtime-10](#p1-runtime-10) | major | test cannot pass | confirmed | The Run window opens on the first pump, about 300 ms in, and nothing paints before it (RT-04). |
| [P1-runtime-11](#p1-runtime-11) | major | wrong engine claim | confirmed with changes | The window is to take 'the same screen position and size, from the output area widget's cached geometry'. |
| [P1-runtime-01](#p1-runtime-01) | minor | wrong runtime model | confirmed with changes | The TDD assumes a Stop that lands between statements arrives where `completed` is already k and statement k+1's echo has not been emitted. |
| [P1-runtime-02](#p1-runtime-02) | minor | wrong engine claim | confirmed with changes | Each per-statement code object is a module whose first instruction, RESUME, has line 0. |
| [P1-runtime-03](#p1-runtime-03) | minor | requirement not met | confirmed with changes | If statement k swallows ScriptCancelled (a bare `except:` or `except BaseException: pass`, which G2(1) explicitly covers), exec returns normally and the loop goes straight on to statement k+1. |
| [P1-runtime-07](#p1-runtime-07) | minor | usability | confirmed | Walking a compound statement's body with Alt+Enter re-executes the branch already run. |
| [P1-runtime-09](#p1-runtime-09) | minor | ordering | confirmed | The claim that 'each statement's echo comes after every line of the statement before it' fails for partial lines. |
| [P1-runtime-12](#p1-runtime-12) | minor | usability | confirmed | The live run view is now the Run window's main content, but it only gets the height left over by the other rows: Title, State, Location, Progress, Undo meter, Notes, Buttons and a two-hint Footer. |
| [P1-runtime-13](#p1-runtime-13) | minor | semantic difference | confirmed | Two observable differences from one exec are not handled. |
| [P1-runtime-14](#p1-runtime-14) | minor | budget | confirmed with changes | The budget counts only execution. |
| [P1-runtime-15](#p1-runtime-15) | minor | inconsistency | confirmed | The patch's new `_execute` handles only two cases: `exec(prep.code, prep.ns)` when `statements is None` (commented 'Run File, Isolated, shelf, snippet, entry point'), and the statement loop. |
| [P1-runtime-16](#p1-runtime-16) | minor | missing case | confirmed | The marks rule gives a raised or stopped mark only when the outcome is error or cancelled. |
| [P1-runtime-17](#p1-runtime-17) | minor | test wrong | confirmed | T-DBG-058 expects 'F11 from the loop's end stops on `b = 2`'. |
| [P1-runtime-18](#p1-runtime-18) | minor | wrong engine claim | confirmed with changes | (1) §4.16.1 says the engine dispatches WM_SYSKEYDOWN to OnKeyDown 'returning 0 when it is handled (:3324-3327)'. |
| [P1-runtime-19](#p1-runtime-19) | minor | usability | confirmed | Echo happens only as statements execute, and the patch removed the submitted code from REPL and console headers. |
| [P1-runtime-M-01](#p1-runtime-m-01) | minor | wrong engine claim | added by refuter | The claim is false in 5.8. |
| [P1-runtime-M-02](#p1-runtime-m-02) | minor | inconsistency | added by refuter | T-SUP-036 expects 'the 150 px area gives a 480 × 200 px window anchored at the area's top-left'. |
| [P1-runtime-M-03](#p1-runtime-m-03) | minor | usability | added by refuter | With `layout.output_position = bottom` and an output area under 200 px (120 px is allowed), anchoring the minimum-size window at the area's top-left pushes it up to 80 px below the area. |
| [P1-runtime-M-04](#p1-runtime-m-04) | minor | missing case | added by refuter | PO-03 gives every Run window a Location row with the executing line's source text, which is read only from 'the run's source snapshot'. |

#### P1-runtime-04

**Major, algorithm bug.** Verdict: confirmed.

**Where:** TDD.md §4.5 Statement extent, Terms and rules 3, 5, 6 (lines 1336-1344); SPEC.md RUN-02 statement extent ('its last token, ignoring a comment, is `:`'); TDD.md T-CORE-039 (line 1373); TDD.md T-CORE-037 (line 1371)

**Problem.** A compound statement whose header holds its body on the same line, followed by a clause (`try: import x` / `except ImportError: x = None`, `if c: a()` / `else: b()`), does not end with ':', so rule 6 gives only the header line. With the caret on the clause line, rule 3 moves S up to the header, but rule 5 does not apply, so the extent is again the header line only. NextStatementLine then returns the clause line, and the caret never advances. The `try:` form fails to compile on every press; the `if:` form re-runs `a()` on every press. T-CORE-039 (extent equals ast over the engine's Lib) cannot pass, because Lib has two such top-level statements.

**Evidence.** p3_semantics.out: `'try: import json\nexcept ImportError: json = None\n' -> 1 statement(s), lines [(1, 2)]; line 1 alone compiles: no (expected 'except' or 'finally' block)`; `'if True: a = 1\nelse: a = 2\n' -> 1 statement(s), lines [(1, 2)]; line 1 alone compiles: yes`. p4_lib_extent.out: 'top-level compound statements whose header line does not end with ':' but span >1 line: 2' at tkinter/__init__.py 98-99 and 120-121 (`try: _flatten = _tkinter._flatten` / `except AttributeError: pass`).

**Refuter.** I re-read §4.5. 'Opens a compound statement' is defined only as EndsWithColon, and rule 5 runs only in that case. For `try: x` / `except E: y`, the caret on line 1 gives rule 6 (line 1 alone, a SyntaxError). The caret on line 2 makes rule 3 move S to line 1, and rule 5 still does not apply, so the extent is again line 1 alone. NextStatementLine(1) is line 2, so Alt+Enter never advances. With `if c: a()` / `else: b()`, it re-runs a() on every press. ast gives (1, 2) for both forms (verify/r1_refute.out item 6). The engine's Lib has two top-level cases, Lib/tkinter/__init__.py:98-99 and :120-121 (`try: _flatten = _tkinter._flatten` / `except AttributeError: pass`), so T-CORE-039 cannot pass as written.

**Fix applied.** As proposed: a logical line opens a compound statement when its first token is a compound keyword (if, while, for, try, with, def, class, async, or a match keyword token), or when its last token is ':'. Rule 5's clause joining (same indentation; elif, else, except, finally) also applies after one-line headers. When rule 3 moves S up, the extent runs from S through the caret's clause and its body. Add both one-line forms, with the caret on each line, to T-CORE-037, and an Alt+Enter case to T-RUN-029.

#### P1-runtime-05

**Major, algorithm bug.** Verdict: confirmed.

**Where:** TDD.md §4.5 rules 2 and 4 (lines 1339-1341); SPEC.md RUN-02 ('a decorator line extends through the decorated definition'); TDD.md T-CORE-037 decorator case; TDD.md T-CORE-039 (first lines only)

**Problem.** The extent starts at the caret's logical line, and the decorator rule only runs downwards. Nothing walks up over preceding `@` lines. With the caret on the `def` or `class` line of a decorated definition, rule 5 runs the bare definition, so the Session name is silently rebound to the undecorated function or class (losing for example `@unreal.uclass()`, `@dataclass` or `@functools.cache`). With the caret on the second of two decorators, the first is dropped. T-CORE-037 expects 'the decorator caret gives decorators plus the whole def' for two decorators, which rules 2 and 4 contradict when the caret is on the lower one. T-CORE-039 only tests the first line, the top decorator, so the gap is untested.

**Evidence.** p3_semantics.out item 5: `decorated def: lineno = 3 | decorator lines = [1, 2] | end = 4` (the ast statement spans lines 1-4). Rule 2: 'S is the statement start of the logical line that holds C'. Rule 4: 'while the first token of the current logical line is @, the extent runs on through the next statement start...' (downwards only).

**Refuter.** Rule 2 starts at the caret's logical line, and rule 4 only walks downwards from an '@' line. Nothing walks up from a `def` or `class` line, or from a lower decorator, to the first decorator. With the caret on `class Foo:` under `@dataclass` (or `@unreal.uclass()`), rule 5 runs the bare class and silently rebinds the Session name to the undecorated object. That is the caret position users most often choose. RUN-02 ('a decorator line extends through the decorated definition') does not cover it. T-CORE-039 puts the caret on 'its first line' and expects the range 'from its first decorator', so it never exercises the def line. T-CORE-037's 'decorator caret' does not say which decorator, so the claimed contradiction only holds if the test uses the lower one. The gap itself is real and silently produces wrong Session state.

**Fix applied.** After rule 2, if S is a decorator line or a def/class line, walk up over the contiguous decorator logical lines at the same indentation (comment and blank lines between them are allowed), and start the extent at the first one. Put the caret on every decorator line and on the def/class line in T-CORE-037 and T-CORE-039, and add the def-line case to RUN-02's acceptance.

#### P1-runtime-08

**Major, design flaw.** Verdict: confirmed.

**Where:** TDD.md §6.2 Design: input echo, The call and What is echoed (lines 3550-3555); SPEC.md OUT-12; TDD.md T-OUT-041 (line 4068); SPEC.md RUN-01 / TDD §5.3.2 'Selection starting mid-line' (only the start is extended)

**Problem.** The echo pushes whole snapshot lines first_line..last_line for each statement. When several statements share a line, each one echoes the full line again: the REPL one-liner `import os; os.getcwd()` shows `>>> import os; os.getcwd()` twice, and a three-statement one-line selection shows the line three times, defeating 'a history of what ran'. When a selection ends mid-line (only the start is extended to the line start), the echo shows text that was not run.

**Evidence.** p3_semantics.out item 2: `ranges for 'a = 1; b = 2': [(1, 1), (1, 1)]`. TDD §6.2: 'pushes one raw record per source line, first_line to last_line, read from the run's source snapshot'. RUN-01: 'A selection that starts mid-line is extended to its line start' (nothing about the end).

**Refuter.** §6.2 says the echo 'pushes one raw record per source line, first_line to last_line'. Statements that share a line therefore echo the same buffer line again. My probe gives `import os; os.getcwd()` → [(1, 1), (1, 1)], and `x = [1,\n 2]; y = 3` → [(1, 2), (2, 2)] (verify/r1_refute.out item 4). The engine's Lib has 26 top-level statement pairs that share a line (verify/r2_lib_shared_lines.out), and `;` one-liners are typical REPL input. Besides the doubled history, OUT-10 Copy as script copies 'the echo text in order' (§6.4). A run's echo of `x += 1; print(x)` would then copy as two identical lines and run twice when pasted. A selection that ends mid-line also echoes text that did not run, because only the start is extended (RUN-01, §5.3.2).

**Fix applied.** Emit each snapshot line at most once per run: statement k pushes lines max(first_line, last_echoed + 1) through last_line. Clip the last line of a selection run to the selection end, or mark the clipped part. Add a `;` line and a mid-line selection end to T-OUT-041, T-REPL-018 and T-OUT-046.

#### P1-runtime-10

**Major, test cannot pass.** Verdict: confirmed.

**Where:** SPEC.md LRS-01 acceptance ('shows each line in the Run window ... within 150 ms of the print'); TDD.md T-SUP-037 (line 3288); TDD.md §5.5.3 'The live run view' first bullet (line 2638); TDD.md §5.13 row 'Live run view latency'

**Problem.** The Run window opens on the first pump, about 300 ms in, and nothing paints before it (RT-04). A line printed at t≈0 is therefore first painted at roughly 300-377 ms, and the line at 200 ms at 100-177 ms. The SPEC criterion 'each line within 150 ms' can never pass. T-SUP-037's p95 over about 26 lines has one certain and one likely outlier, so it is flaky (nearest-rank p95 is the 25th of 26). Separately, 'a line written since the last pump is painted by this one' ignores the SumiOutput thread, which 'collects for up to 5 ms' before the SPSC hop. A line written just before a pump is drained by the next pump.

**Evidence.** LRS-01: 'watchdog thread arms about 300 ms into a run' and the window 'opens on the first pump'. T-SUP-032: first callback 'within one period plus 10 ms after arming'. TDD §6.2 table: SumiOutput 'Wakes on an event ... then collects for up to 5 ms ... pushes processed batches to the view queue (SPSC)'. §5.4.2: drain at step 4 moves only what is already in the view queue. T-SUP-037: 'statement 2 prints one line every 200 ms for 5 s ... Each line is painted ... within 150 ms of its print (proposed, p95)'.

**Refuter.** Nothing paints before the first pump: LRS-01 opens the Run window on the first pump, about 300 ms in. T-SUP-032 allows the first callback up to one period plus 10 ms after arming, and the hand-off adds 5-7 ms. A line printed by statement 2 at t≈0 is therefore first painted at about 305-390 ms. SPEC LRS-01's 'shows each line ... within 150 ms of the print' cannot hold for it. The line at 200 ms is painted 105-190 ms after its print. T-SUP-037 has 25-26 lines, so the nearest-rank p95 is the 24th or 25th value, which is the second outlier, and the test fails whenever the 200 ms line misses. Separately, §5.5.3's 'a line written since the last pump is painted by this one' ignores SumiOutput's collection window of up to 5 ms before the SPSC push (§6.2 table), so a line written just before a pump is painted one pump later.

**Fix applied.** In LRS-01's acceptance and T-SUP-037, measure latency from max(print time, first paint of the Run window), or exclude lines written before the first pump. Reword §5.5.3 and the §5.13 row as 'painted by this pump or the next; bound = one pump period + the 5 ms ingest window + the hand-off'.

#### P1-runtime-11

**Major, wrong engine claim.** Verdict: confirmed with changes.

**Where:** TDD.md §5.5.3 steps 3-4 (Position PO-03, line 2610); TDD.md T-SUP-036 (line 3287); SPEC.md LRS-01 ('exactly over the output area: the same screen position and size')

**Problem.** The window is to take 'the same screen position and size, from the output area widget's cached geometry'. Cached absolute geometry is in desktop pixels, but SNew(SWindow) by default multiplies ScreenPosition and ClientSize by the monitor's DPI scale. Step 3's argument list, which otherwise names every SWindow argument, does not turn this off. At 150% the Run window comes out 1.5× too large and offset from the output area. T-SUP-036's 4 px check has no high-DPI case, so CI at 100% would not catch it. Whether the 480 × 200 minimum is in Slate units or physical pixels is not stated.

**Evidence.** Engine/Source/Runtime/SlateCore/Public/Widgets/SWindow.h:255 `_AdjustInitialSizeAndPositionForDPIScale(true)`, :303 the argument. SWindow.cpp:284-289: `else if (InArgs._AdjustInitialSizeAndPositionForDPIScale && !WindowPosition.IsZero()) { ... WindowPosition *= InitialDPIScale; }`. SWindow.cpp:365: `DPIScaledClientSize = InArgs._AdjustInitialSizeAndPositionForDPIScale ? InArgs._ClientSize * DPIScale : ...`. The engine itself passes `.AdjustInitialSizeAndPositionForDPIScale(false)` where it already has pixels (SWindow.cpp:451).

**Refuter.** The engine facts hold. SWindow.h:255 defaults `_AdjustInitialSizeAndPositionForDPIScale(true)`. SWindow.cpp:284-289 multiplies a non-zero ScreenPosition by the DPI scale, and :365 multiplies ClientSize. A second default the TDD also omits is `_AutoCenter(EAutoCenter::PreferredWorkArea)` (SWindow.h:252). With it, Construct centres the window and ignores ScreenPosition (SWindow.cpp:341-355 and the centring branch). The engine's own geometry-placed windows set all of these: MenuStack.cpp:494-501 (`.ScreenPosition(...).AutoCenter(EAutoCenter::None).ClientSize(...).AdjustInitialSizeAndPositionForDPIScale(false)`) and TabManager.cpp:3864-3870. This is an omission, not a wrong claim: step 3 names no position or size arguments at all. But the patch made 'exactly ... same screen position and size' a requirement with a 4 px test at one DPI. A reader would get a 1.5× window at 150%, or a centred one.

**Fix applied.** In step 3/4, list `.AutoCenter(EAutoCenter::None)`, `.ScreenPosition(Geo.GetAbsolutePosition())`, `.ClientSize(Geo.GetAbsoluteSize())` and `.AdjustInitialSizeAndPositionForDPIScale(false)`, as MenuStack.cpp:494-501 does. State that 480 × 200 is in Slate units, so the physical minimum is 480 × 200 × the DPI scale at the area. Add 125% and 150% cases to T-SUP-036, plus a case on a second monitor with a different scale.

#### P1-runtime-01

**Minor, wrong runtime model.** Verdict: confirmed with changes.

**Where:** TDD.md §5.3.7 _execute and bullets (lines 2379-2392); TDD.md §5.14 row 'Stop lands in the runner's loop between two statements' (line 3172); TDD.md §4.8.2 Ran-line marks, Data (line 1514); TDD.md T-RUN-034 (line 3242); TDD.md T-VIEW-020 (line 1573); SPEC.md EDT-18

**Problem.** The TDD assumes a Stop that lands between statements arrives where `completed` is already k and statement k+1's echo has not been emitted. The Python loop actually has three eval-breaker sites outside user frames. (1) After the C call `_sumi.echo` returns: the echo of k+1 has been pushed and `index` recorded as started, but the statement never runs. (2) After `exec(code, ns)` returns but before `prep.completed = index + 1`: the statement completed but is counted as not returned. (3) JUMP_BACKWARD: the only clean site. Consequences: §4.8.2 marks a statement that fully ran as 'Stopped' (site 2), or a statement that never ran as 'Stopped' (site 1). The detail 'stopped after statement k of n' is one too low. A Stop arriving just after the last statement returns gives 'stopped after statement n-1 of n' instead of 'Stop arrived as the code finished'. The old bullet at line 2392 ('no frame from prep.filename' means 'Stop arrived as the code finished') now matches every between-statement stop, so the two bullets conflict. T-RUN-034 tier 2 fakes an `echo` that raises, which the real C echo never does.

**Evidence.** dis of the TDD loop (p1_landing.out): line 30 `CALL` (exec), then line 31 `STORE_ATTR completed`, then `JUMP_BACKWARD`; 3.11 checks the eval breaker after each CALL. p1 case A, with a C call standing in for echo that trips the eval breaker at index 1: 'landed at _execute_c ... echoes[index]() | echoed indices: [0, 1] | completed = 1 | statement 1 ran: False | user frame in tb: False'. p2_landing_stats.out, 285 random deliveries over 20,000 one-line statements: 96 'statement returned, completed NOT incremented (off by one)', 86 'inside the statement frame, line 0 (RESUME)', 62 'clean boundary (completed incremented)', 41 'echo of statement k done, statement never ran'.

**Refuter.** The three runner-frame check sites are real. 3.11 checks the eval breaker after a C CALL returns. In p1 case A, a delivery made pending inside a C call that stands in for echo landed on the echo line: index 1 was echoed, statement 1 did not run, and completed was 1. In p2, site 2 (exec returned, completed not yet stored) took 96 of 285 deliveries. So the §5.3.7 claim that only the clean JUMP_BACKWARD case exists is incomplete. Site 2 produces an off-by-one 'stopped after statement k of n' and a 'Stopped' mark on a statement that returned. For a one-statement Alt+Enter run it gives 'stopped after statement 0 of 1'. The impact is smaller than claimed, though. Only one detail string and one gutter mark are wrong. The window is a pending call falling due between two statements. p2's rate comes from 200,000 trivial statements. In real UE scripts, reflected calls release the GIL (TDD §5.4.5, PyUtil.cpp:644-647), so the pending bit is set when the call returns and is taken at that CALL's check inside the statement frame. No listed test fails: T-RUN-034 tier 4 lands inside statement 2, and T-VIEW-020 and T-RUN-034 tier 2 use fakes. A raising fake echo is a legitimate way to simulate a boundary delivery, but it covers only one site. §5.14 already gives the between-statements detection rule ('No user frame in the traceback; completed below n'), so the two §5.3.7 bullets mostly reconcile. Site 2 on the last statement is the exception. Severity: minor, not major.

**Fix applied.** Replace `_sumi.echo` + `exec` + `prep.completed = index + 1` with one C call, `_sumi.run_statement(run_id, index, code_or_None, ns)`. It records 'started', pushes the echo, runs PyEval_EvalCode, and records 'returned' before it returns. Every runner-frame check site then sees consistent counters, and `report_outcome` takes completed from C. In statement mode, state the detail rule as: returned == n → 'Stop arrived as the code finished'; otherwise 'stopped after statement <returned> of n'. Add T-RUN-034 tier-2 cases with a delivery pending as the call returns, both mid-buffer and after the last statement.

#### P1-runtime-02

**Minor, wrong engine claim.** Verdict: confirmed with changes.

**Where:** TDD.md §5.4.7 line at the last pump and source text (lines 2559-2561); TDD.md §5.3.9 cancelled 'file and line where it landed'; TDD.md §5.5.3 Location row; TDD.md §5.5.4 Execution arrow; SPEC.md LRS-01 Location row

**Problem.** Each per-statement code object is a module whose first instruction, RESUME, has line 0. A pending call (pump or Stop) that runs at a statement's RESUME therefore sees f_lineno 0. The Location row shows 'Line 0' with no text, and a cancel reports '<file>:0'. In one-exec mode the module RESUME runs once at t=0, before the watchdog arms (about 300 ms), so it was unreachable; statement mode makes it a check site for every statement. At the runner's own check sites (about 70% of deliveries in p2) no frame with the run's filename is on the stack at all. §5.4.7 ('the innermost frame whose co_filename is the run's filename') then finds nothing, and the TDD does not say what the Location row, the cancel location or the LRS-09 gutter arrow show.

**Evidence.** p1_landing.out case B: 'per-statement code object co_firstlineno = 1 | first instruction: RESUME positions: (0, 1, 0, 0)'; 'interrupted frame {'f_lineno': 0, 'co': '<sumi:t#1>'} | traceback user entry: [(0, '')]'. p2: 86 of 285 deliveries at RESUME line 0; 199 of 285 with no user frame in the traceback.

**Refuter.** Reproduced on the engine's 3.11.8 (verify/r1_refute.out). Every per-statement code object built as in §5.3.2 (ast.parse, increment_lineno to line 40, Module([stmt])) starts with RESUME at line 0. A signal made pending just before exec lands in the statement frame with f_lineno 0, a traceback entry `('<sumi:Untitled#1>', 0)`, and the statement not run. §5.4.7 gives no fallback for a pump that lands in the runner loop. Its 'plus the innermost frame if different' would then name a `sumi_editor/_impl/runner.py` frame. The impact is narrow for the same reason as P1-runtime-01: the pending call must fall due exactly at a statement start or between statements. A wrong Location at a pump is replaced at the next pump, about 50-100 ms later. Only a Stop that lands at RESUME leaves a lasting '<file>:0' location and a line-0 traceback link. Severity: minor.

**Fix applied.** In §5.4.7 and §5.3.9: when the matched frame's code object is one of the run's prepared statements and its line is ≤ 0, report that statement's first_line. When no frame with the run's filename is on the stack in statement mode, show the range of the last started statement, for example 'Lines 12–14 · `for a in actors:` · statement 3 of 12', and never show a runner frame as the 'innermost frame'. Apply the same rule to the cancel location and to the LRS-09 arrow. Add a tier-2 case that delivers at a statement's RESUME and in the runner loop, and checks the Location row and the cancel location.

#### P1-runtime-03

**Minor, requirement not met.** Verdict: confirmed with changes.

**Where:** SPEC.md RUN-27 ('An exception or Stop ends the run at that statement; later statements do not run'); TDD.md §5.3.7 _execute (lines 2379-2387); TDD.md §5.3.9 row 'Returned | Yes | cancelled | the script handled Stop and returned'

**Problem.** If statement k swallows ScriptCancelled (a bare `except:` or `except BaseException: pass`, which G2(1) explicitly covers), exec returns normally and the loop goes straight on to statement k+1. LRS-12 re-delivers only 250 ms after the handler ends. So after the user pressed Stop, later statements run, possibly destructive asset calls in a line-by-line buffer. If they all finish before re-delivery, the run ends 'cancelled · the script handled Stop and returned', and EDT-18 marks them 'ok'. The loop has a safe, cheap checkpoint between statements and does not use it.

**Evidence.** §5.3.7 loop: `_sumi.echo(...)`, then `exec(code, prep.ns)`, then `prep.completed = index + 1`, with no stop check. LRS-02 and LRS-12: 're-delivery every 250 ms once the exception was swallowed'; 'While PyErr_GetHandledException() is Sumi's ScriptCancelled, nothing is re-delivered'. §12.1 already has `stop_requested(run_id) -> bool` ('polled between task steps').

**Refuter.** Confirmed. The §5.3.7 loop has no stop check, and `stop_requested` (§12.1) is 'polled between task steps' only. A statement that swallows ScriptCancelled therefore lets statements k+1.. run until the LRS-12 re-delivery 250 ms later. If they finish first, the run ends 'the script handled Stop and returned' with all of them marked ok. This is the same exposure one exec already has for code after a swallowing handler (v1.1 LRS-12 behaviour), so it is not a regression. RUN-27's 'later statements do not run' can be read as covering only a Stop that propagates. Still, statement mode has a free checkpoint that should be used, and the user-facing result after a Stop is misleading. Severity: minor.

**Fix applied.** Before statement k+1 starts, check Stop. Do it in C inside the per-statement call, or call `_sumi.stop_requested(run_id)` and raise the run's ScriptCancelled. The outcome is cancelled, 'stopped after statement k of n'. Say in RUN-27 that a Stop the script handled still prevents later statements. Add a T-RUN-034 case: statement 2 does `except BaseException: pass` around a latch wait, statement 3 writes a marker, and the marker is never written.

#### P1-runtime-07

**Minor, usability.** Verdict: confirmed.

**Where:** TDD.md §4.5 rule 3 and Next statement; SPEC.md RUN-26

**Problem.** Walking a compound statement's body with Alt+Enter re-executes the branch already run. From a body line, the advance target is the next statement start, which is the `else:`, `except:` or `finally:` line. The next press then runs the whole compound statement again (rule 3), repeating the side effects of the branch the user just stepped through.

**Evidence.** Buffer `if a:` / `    x = f()` / `else:` / `    y = 2`, caret on `x = f()`: rule 6 runs line 2 alone; NextStatementLine(2) = line 3 (a statement start); Alt+Enter on line 3: rule 3 moves S to line 1 and rule 5 runs lines 1-4, so f() is called again.

**Refuter.** Traced through §4.5. With the caret on the body line `x = f()` of `if a:` / `x = f()` / `else:` / `y = 2`, rule 6 runs line 2 alone. NextStatementLine(2) is the `else:` line, which counts as a statement start under §4.5's definition. The next Alt+Enter applies rule 3, moves S up to `if a:`, and rule 5 runs lines 1-4, calling f() again. This is a real wart in the walk-the-buffer workflow PO-02 asks for, but it affects only users who step into a body.

**Fix applied.** In NextStatementLine: when the target is a clause line (elif, else, except, finally, or a case keyword) of a compound statement that contains Last, advance to the first statement start after that compound statement's full extent. Add the case to T-CORE-038 and T-RUN-029.

#### P1-runtime-09

**Minor, ordering.** Verdict: confirmed.

**Where:** TDD.md §6.2 Design: input echo, Order (line 3554); TDD.md §6.1 Design: line splitting (partial-line buffer per thread, stream and run)

**Problem.** The claim that 'each statement's echo comes after every line of the statement before it' fails for partial lines. A statement that writes text without a newline (`print('Working...', end='')`) leaves it in the ingest thread's per-(thread, stream, run) partial buffer. The next statement's Echo records are a different key, so they are committed first, and the earlier text appears under the later statement's echo once its newline arrives.

**Evidence.** TDD §6.1: 'one partial-line buffer per (native thread ID, stream, run ID)'; 'A partial line is committed by a newline, at run end, or when it reaches 16 KB'; the view shows partial text 'as a provisional last line of its block'. §6.2: 'pushes one raw record per source line ... with type Echo'.

**Refuter.** §6.1 keeps one partial-line buffer per (thread, stream, run), and a partial line is committed only by a newline, at run end, or at 16 KB. Echo records are pushed as complete lines (§6.2). Text from `print('Working...', end='')` in statement k therefore stays provisional while echo k+1 is committed, and it lands after echo k+1 when its newline arrives. That breaks §6.2's 'each statement's echo comes after every line of the statement before it'. The pattern is common in progress output, but the effect is cosmetic.

**Fix applied.** When an Echo record for a run is ingested, first commit (flagged Partial) every open game-thread partial line of that run, or split it there. Add `print('a', end='')` followed by a statement that prints 'b' to T-OUT-041.

#### P1-runtime-12

**Minor, usability.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 Run window rows and 'It takes the height the other rows leave (proposed)'; TDD.md §5.5.3 step 4 minimum 480 × 200 px; TDD.md §6.4 Split (each slot ≥ 120 px)

**Problem.** The live run view is now the Run window's main content, but it only gets the height left over by the other rows: Title, State, Location, Progress, Undo meter, Notes, Buttons and a two-hint Footer. The output area may legitimately be 120-200 px, which gives the 200 px minimum window. That leaves the live view only one or two lines, so the user cannot really 'watch it run' (PO-03) at that size. The 200 px minimum was sized ('fits the rows below') for the old output tail.

**Evidence.** TDD §5.5.3 row table (Title, State, Location, Progress, Live run view, Undo meter, Notes, Buttons, Footer). Mockup 02 draws the case at `.outarea { height: 380px; }` (mockups/02-run-and-cancel.html:31), well above the minimum.

**Refuter.** In §5.5.3 the live view 'takes the height the other rows leave'. The other rows are Title, State, Location, Progress, Undo meter, Notes, Buttons and a two-hint Footer. The 480 × 200 minimum is the unchanged v1.1 value 'fits the rows below', which was sized for the old Output tail row. At an output area of 120-200 px, which §6.4 allows, the live view gets one or two lines or none. Mockup 02 draws only the comfortable 380 px case. The default 40% split normally gives enough room, so the problem appears only with a small output area.

**Fix applied.** Give the live view a minimum of about 6 lines and derive the window's minimum height from that. Hide the Undo and Notes rows while they are empty, or let the window grow into the code area, which is not drawn during a run.

#### P1-runtime-13

**Minor, semantic difference.** Verdict: confirmed.

**Where:** TDD.md §5.3.7 Statement mode bullet ('tracebacks, linecache, __annotations__ ... behave the same'); SPEC.md RUN-27 Execute; TDD.md §5.3.2 Statement mode row (whole-module compile then per-statement compile)

**Problem.** Two observable differences from one exec are not handled. (1) The compiler emits SETUP_ANNOTATIONS at the start of a module that has any annotated assignment. With one exec, `__annotations__` therefore exists from statement 1; in statement mode it appears only when the annotated statement runs. (2) Compile-time SyntaxWarnings (for example `x is 1`) are emitted by both the whole-module check and the per-statement compile, so every such warning shows twice in the run's output.

**Evidence.** p3_semantics.out item 1: `__annotations__ visible to statement 1:  one exec = True | statement mode = False`. Item 6: `SyntaxWarning count in statement mode: 2 ['"is" with a literal. Did you mean "=="?']`, `SyntaxWarning count with one exec: 1`.

**Refuter.** Reproduced (verify/r1_refute.out). Item 7: statement 1 sees `__annotations__` with one exec (True) but not in statement mode (False), because SETUP_ANNOTATIONS is emitted per module. Item 5: the warnings shown during prepare are `[('SyntaxWarning', 2), ('SyntaxWarning', 2)]`, once from the whole-module compile and once from the per-statement compile, because compile-time warnings bypass the registry. The TDD says nothing about either case (no SyntaxWarning or catch_warnings text). The final namespaces still match, so the impact is small.

**Fix applied.** If the whole module has a module-level AnnAssign with a simple target (including inside compound bodies), run `ns.setdefault('__annotations__', {})` before statement 0. Do the per-statement compiles under `warnings.catch_warnings()` with SyntaxWarning and DeprecationWarning ignored. Add both cases to T-RUN-030 and T-RUN-031.

#### P1-runtime-14

**Minor, budget.** Verdict: confirmed with changes.

**Where:** TDD.md §5.13 row 'Statement-mode overhead (RUN-27)' ('a 10,000-statement buffer pays at most 50 ms', line 3103); SPEC.md RUN-27 ('proposed budget: ≤ 5 µs per statement'); TDD.md T-RUN-033 (line 3241)

**Problem.** The budget counts only execution. Prepare compiles everything twice (the whole module, then each statement alone) during Starting, on the game thread, before any pump can run. On the engine interpreter the extra per-statement compiles cost about 9 µs per statement, more than the whole statement-mode budget. A 10,000-statement buffer pays about 93 ms of extra frozen time in Starting, not 'at most 50 ms'. T-RUN-033 measures only execution, so it cannot see this.

**Evidence.** p3_semantics.out item 8: `10,000 statements: whole compile 58.4 ms; extra per-statement compiles 93.3 ms (9.3 us per statement)`. timing.py: execution loop 0.24 µs per statement (so the exec budget itself is fine).

**Refuter.** The point holds, but the verifier's number is high. Best of 5 on the engine's 3.11.8 (verify/r1_refute.out item 3), for 10,000 `x = i` statements: whole compile 17.6 ms, per-statement compiles 48.8 ms (4.9 µs per statement), extra exec-loop cost 0.13 µs per statement. The per-statement compile alone roughly uses up the whole '≤ 5 µs per statement' budget. It runs in Starting on the game thread, where nothing paints. Including the echo push (about 1 µs, §6.2), a 10,000-statement buffer pays about 60 ms, not 'at most 50 ms'. If T-RUN-033 measures end to end, it fails at about 6 µs per statement. If it measures execution only, it misses this cost.

**Fix applied.** Budget prepare separately in §5.13 (for example ≤ 6 µs per statement, proposed), or compile statement k lazily inside the supervised loop, which pumps, after the whole-module check. State in T-RUN-033 whether prepare is included, and include it.

#### P1-runtime-15

**Minor, inconsistency.** Verdict: confirmed.

**Where:** TDD.md §5.3.7 _execute definition (lines 2379-2381) vs the call-site comment (line 2369); TDD.md §5.2.1 routes table, Entry point 'Off (one exec)'

**Problem.** The patch's new `_execute` handles only two cases: `exec(prep.code, prep.ns)` when `statements is None` (commented 'Run File, Isolated, shelf, snippet, entry point'), and the statement loop. Task steps (a generator step per `_run` entry), reload routes and the entry point (reload, then call `<module>.main`, §9) are not an exec of a code object, yet they also have `statements is None`. A reader following this code would exec them. The call site still says 'statement mode, one exec, a task step, a reload or an entry point'.

**Evidence.** TDD line 2369: `_execute(prep)  # statement mode, one exec, a task step, a reload or an entry point`; lines 2380-2381: `if prep.statements is None:  # Run File, Isolated, shelf, snippet, entry point (§5.2.1)` / `exec(prep.code, prep.ns); return`. TDD §9 entry point (lines ~6231-6238): reload first, then call the entry function. TDD line 2419: 'Task steps are separate ExecPythonCommandEx calls'.

**Refuter.** The patch replaced the v1.1 abstract `_execute(prep)  # exec(code, ns), a task step, a reload or an entry point` with a definition that has only two branches. Its `statements is None` branch carries the comment 'Run File, Isolated, shelf, snippet, entry point' and runs `exec(prep.code, prep.ns)`. Task steps call `next(gen)` (§5.10, line 3058). The entry point reloads, imports and calls `<module>.main` (§9.4 steps 1-6). Neither is an exec of prep.code. The §5.2.1 column 'Off (one `exec`)' for Entry point is also inaccurate.

**Fix applied.** Dispatch in `_execute` by prep.kind (statement mode, one exec, task step, reload, entry point), with the latter three delegated to §5.10 and §9. Change §5.2.1's Entry point cell to 'Off (reload and call, §9.4)'.

#### P1-runtime-16

**Minor, missing case.** Verdict: confirmed.

**Where:** TDD.md §4.8.2 Ran-line marks, Data (line 1514); TDD.md §5.3.9 SystemExit rows; SPEC.md EDT-18

**Problem.** The marks rule gives a raised or stopped mark only when the outcome is error or cancelled. `sys.exit()` or `SystemExit(0)` in statement k ends the run `ok` with completed = k-1, so the statement that ended the run is unmarked and looks 'not reached', as do the later ones. SystemExit(n) gives 'Raised', even though nothing failed.

**Evidence.** §4.8.2: 'Statements below that count are ok. If the run ended error or cancelled and the last started statement did not return, that statement is raised or stopped'. §5.3.9: 'SystemExit(0) or SystemExit(None) | ok | exited (code 0)'; 'SystemExit(n) | error'.

**Refuter.** §4.8.2 adds a raised or stopped mark only when the outcome is error or cancelled. `sys.exit()` in statement k ends the run ok, 'exited (code 0)' (§5.3.9), with k-1 statements returned, so statement k is unmarked and looks 'not reached'. SystemExit(n) is outcome error and gets 'Raised'. That mark is defensible but says nothing about the exit.

**Fix applied.** When the run ended through SystemExit, mark the last started statement 'exited' (tooltip 'Exited in #22 (code n)', its own dash pattern). Add the case to T-VIEW-020.

#### P1-runtime-17

**Minor, test wrong.** Verdict: confirmed.

**Where:** TDD.md T-DBG-058 (line 6018)

**Problem.** T-DBG-058 expects 'F11 from the loop's end stops on `b = 2`'. On 3.11.8, stepping off the last body line jumps back to the `for` header, which gets a line event when the iterator is exhausted. The next stop is therefore the `for` line, and only the following F11 reaches `b = 2`. The boundary behaviour itself, F10 and F11 crossing into the next statement without stopping in runner frames, is confirmed.

**Evidence.** p5_bdb_boundaries.out (stock bdb, skip=['sumi_editor','sumi_editor.*'], statements `a = 1` / `for i in range(2): pass` / `b = 2`): 'F11 x8 from the breakpoint on statement 1: [1, 2, 3, 2, 3, 2, 4]'; 'after the last 'pass' (5th stop) the next F11 stops on line 2'.

**Refuter.** p5 models SumiBdb (stock bdb, runner frames skipped, return events ignored) and gives F11 stops [1, 2, 3, 2, 3, 2, 4] on 3.11.8. Stepping off the last `pass` stops on the `for` header first, and the iterator runs out there. T-DBG-058's What-it-checks says 'F11 from the loop's last iteration', so 'F11 from the loop's end stops on `b = 2`' fails from the last body line. The boundary crossing itself (the `for` line to `b = 2`, with no stop in runner frames) works.

**Fix applied.** Change the pass criterion to: 'F11 from the last body line stops on the `for` line; the next F11 stops on `b = 2`'.

#### P1-runtime-18

**Minor, wrong engine claim.** Verdict: confirmed with changes.

**Where:** TDD.md §4.16.1 Alt+Enter (RUN-26) bullet (line 1996); TDD.md §5.14 row 'the platform's modal filter drops input to every other window' (line 3173)

**Problem.** (1) §4.16.1 says the engine dispatches WM_SYSKEYDOWN to OnKeyDown 'returning 0 when it is handled (:3324-3327)'. In fact ProcessMessage's WM_SYSKEYDOWN case defers the message and then breaks to `return DefWindowProc(...)`, so DefWindowProc always sees it, and the 0 from ProcessDeferredMessage is discarded (DeferMessage returns void). The conclusion holds only because DefWindowProc ignores WM_SYSKEYDOWN with VK_RETURN, and WM_SYSCHAR returns 0. (2) The modal input filter is Slate's, not the platform's.

**Evidence.** WindowsApplication.cpp:2346-2360 (`case WM_SYSKEYDOWN: ... DeferMessage(...); } break;`), :3164 `return IntCastChecked<int32>(DefWindowProc(hwnd, msg, wParam, lParam));`, :4188 `void FWindowsApplication::DeferMessage(...)`. p7_defwindowproc_altenter.out: 'DefWindowProc(WM_SYSKEYDOWN, VK_RETURN, Alt): no WM_SYSCOMMAND / menu message'; 'contrast, DefWindowProc(WM_SYSCHAR, '\r'): [('0x112', '0xf100', ...)]' (SC_KEYMENU). SlateApplication.cpp:3762-3778: GetInteractiveTopLevelWindows returns only `ActiveModalWindows.Last()` (plus its interactive tooltip).

**Refuter.** Part 1 holds. WindowsApplication.cpp:2346-2359 defers WM_SYSKEYDOWN and then `break`s to `return DefWindowProc(...)` at :3164. DeferMessage returns void (:4188), and its immediate branch discards ProcessDeferredMessage's result (:4197). So the 0 at :3324-3327 never reaches Windows, and DefWindowProc always sees WM_SYSKEYDOWN. The conclusion survives only because DefWindowProc generates nothing for VK_RETURN (p7) and WM_SYSCHAR returns 0 (:2332-2344). Part 2 is not a patch defect: §5.14's 'the platform's modal filter' repeats v1.1's own wording in §5.5.3 ('The platform filters input to every other window ... SlateApplication.cpp:4886-4901').

**Fix applied.** Reword §4.16.1: 'WM_SYSKEYDOWN is deferred to Slate and still passed to DefWindowProc, which generates no SC_KEYMENU for VK_RETURN (probe); the menu and beep come from WM_SYSCHAR, which ProcessMessage answers with 0 (:2332-2344).' Leave §5.14's wording alone, or change it in both places together.

#### P1-runtime-19

**Minor, usability.** Verdict: confirmed.

**Where:** TDD.md §6.4 'REPL and console runs do not repeat the submitted code in the header' (line 3759); TDD.md T-RUN-032 ('no echo line is emitted'); TDD.md §6.4 REPL link fallback / T-REPL-018; SPEC.md OUT-12

**Problem.** Echo happens only as statements execute, and the patch removed the submitted code from REPL and console headers. A submission that ends `not executed` (a SyntaxError found by the whole-module compile, or a failed trust or snapshot step) therefore leaves no input in Output or the REPL tab. Only the error remains: for a multi-line REPL block, just the offending line. The `<sumi:REPL#N>` link fallback scrolls to a header that no longer holds the code. Maya's history shows the code it tried to run.

**Evidence.** T-RUN-032: 'Both end not executed with the SyntaxError at its buffer line; the marker is never written; no echo line is emitted'. §6.4: 'REPL and console runs do not repeat the submitted code in the header (PO-02): the run's echo lines, with >>> and ... prompts, follow it'. §6.4 links table: 'If those lines are hidden ... or evicted, scroll to run #42's header'.

**Refuter.** T-RUN-032 requires that a `not executed` statement-mode run emit no echo. §6.4 and OUT-12 drop the submitted code from REPL and console headers, and T-REPL-004 clears the prompt into history even for a `not executed` input. A REPL submission with a SyntaxError therefore leaves only the SyntaxError's own line in Output and the REPL tab. The `<sumi:REPL#N>` link's fallback (T-REPL-018) scrolls to a header that no longer shows the code. v1.1 at least showed the first line plus '(+n lines)'.

**Fix applied.** For REPL and console runs that end `not executed`, push the submitted lines from the snapshot as echo lines flagged not-run, or keep the v1.1 header input for those runs only. Add the case to T-REPL-018 and T-RUN-032.

#### P1-runtime-M-01

**Minor, wrong engine claim.** Raised by the refuter.

**Where:** SPEC.md RUN-26 ('No editor or Slate command binds Alt+Enter in 5.8; the only handler is the game viewport's fullscreen toggle'); TDD.md §4.9.4 RunLineAdvance bullet (line 1716); research/08-decision-log.md §10 PO-02 row (line 1772)

**Problem.** The claim is false in 5.8. The PCG plugin, enabled by default, binds Alt+Enter to 'Apply Changes' in its HLSL source editor text box. The design conclusion still holds: that binding lives in the widget's own command list, and ComputeShadowed (TDD §11.1.4) reads only the main-frame, Level Editor and consume lists, so T-KEY-018's 'no Alt+Enter row' still passes. The docs nonetheless state an unverified absolute as an engine fact.

**Evidence.** Engine/Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:57-69: context "PCGShaderTextEditorDocumentTextBox"; `UI_COMMAND(ApplyChanges, "Apply Changes", ..., FInputChord(EKeys::Enter, EModifierKey::Alt))`. Engine/Plugins/PCG/PCG.uplugin:13 `"EnabledByDefault": true`. No other Alt+Enter chord was found under Engine/Source/Editor, Slate or Plugins (grep).

**Fix applied.** Reword all three places, for example: 'No main-frame, Level Editor or consume-list command binds Alt+Enter in 5.8 (the PCG HLSL editor's Apply Changes, SPCGCodeEditorTextBox.cpp:69, acts only inside that widget); the game viewport's fullscreen toggle (GameViewportClient.cpp:674) acts only while the viewport has focus.'

#### P1-runtime-M-02

**Minor, inconsistency.** Raised by the refuter.

**Where:** TDD.md T-SUP-036 (line 3287); TDD.md §5.5.3 step 4 ('a smaller output area gets the minimum'); SPEC.md LRS-01 ('if the output area is smaller, the window uses the minimum')

**Problem.** T-SUP-036 expects 'the 150 px area gives a 480 × 200 px window anchored at the area's top-left'. Dragging the splitter changes only the area's height, so the area stays as wide as the centre column (normally well over 480 px). If the minimum is applied per dimension, as a minimum size normally is, the window is (area width) × 200 and the test fails. If the whole 480 × 200 minimum replaces the area's size, the window covers only part of the output area's width, the stale output frame stays visible beside it, and PO-03's 'the Run window is the output area' hand-off no longer holds.

**Evidence.** T-SUP-036: 'with the output area dragged to 150 px ... the 150 px area gives a 480 × 200 px window anchored at the area's top-left'. §5.5.3 step 4: 'The minimum size stays 480 × 200 px ...; a smaller output area gets the minimum, anchored at the output area's top-left.' §6.4 Split: the output area spans the centre column.

**Fix applied.** State per-dimension clamping: width = max(area width, 480), height = max(area height, 200). Change T-SUP-036's expected rect to (area width) × 200 at the area's top-left, and add a case with an area narrower than 480 px.

#### P1-runtime-M-03

**Minor, usability.** Raised by the refuter.

**Where:** TDD.md §5.5.3 step 4 (anchored at the output area's top-left); SPEC.md LRS-01; SPEC.md WS-03 (`layout.output_position = bottom`)

**Problem.** With `layout.output_position = bottom` and an output area under 200 px (120 px is allowed), anchoring the minimum-size window at the area's top-left pushes it up to 80 px below the area. That puts it over the tab edge and status bar and, with a maximised main frame, past the bottom of the monitor work area. The bottom rows (Buttons, Footer) can be hidden. Slate does not correct this: sane placement checks only the top-left corner and clamps only the size.

**Evidence.** SWindow.cpp:319-331: with AutoCenter None and SaneWindowPlacement, only a top-left corner outside VirtualDisplayRect resets the placement. The manual-placement branch clamps WindowSize to the virtual display but never moves WindowPosition. TDD §6.4: each area ≥ 120 px. §5.5.3 rows: Buttons and Footer are last. Esc and Shift+F5 still stop the run, so this is not a safety issue.

**Fix applied.** Anchor the minimum-size window at the area's edge next to the code area: top-left in the top layout, bottom-left in the bottom layout. Clamp the rect into the monitor work area. Add the bottom-layout small-area case to T-SUP-036.

#### P1-runtime-M-04

**Minor, missing case.** Raised by the refuter.

**Where:** TDD.md §5.4.7 ('that line's source text (PO-03), read in C++ from the run's source snapshot (§6.2)'); TDD.md §6.2 'Source snapshot'; SPEC.md LRS-01 Location row

**Problem.** PO-03 gives every Run window a Location row with the executing line's source text, which is read only from 'the run's source snapshot'. §6.2 defines that snapshot only for buffer routes and for REPL and console runs. API runs of a file not open in a tab, shelf and snippet items, entry points and task steps also open the Run window, but nothing says where their source text comes from or that it is omitted. C++ cannot read linecache without Python, which §5.4.7 forbids.

**Evidence.** §6.2: 'Source snapshot. For buffer routes, the TextSnapshot that the request's text was cut from ... For REPL and console runs, a snapshot of the submitted text'. §5.2.1 routes: API (file), Shelf/snippet (item .py path), Entry point (module file), Task step (generator's file). §5.4.7: 'never through Python'.

**Fix applied.** Either capture a TextSnapshot of the compiled file text in prepare for every route (the text is already read there), or state that routes without a snapshot show 'Line 43 · <file>' with no source text. Add one such route to T-SUP-037.

### Round 1 — Consistency and delivery

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [P1-consistency-01](#p1-consistency-01) | major | test cannot pass / contradiction | confirmed | At the end of a run the hand-off promises two things that cannot both be true: the same first visible line and follow mode at the bottom ('nothing jumps'). |
| [P1-consistency-02](#p1-consistency-02) | major | acceptance cannot pass / inconsistency | confirmed | The new LRS-01 criterion requires each printed line to appear within 150 ms of its print. |
| [P1-consistency-M-01](#p1-consistency-m-01) | major | usability / made false by patch | added by refuter | PO-03 makes the live run view the run's only output surface: the output and code areas are not drawn during a run (RV-16). |
| [P1-consistency-03](#p1-consistency-03) | minor | stale text | confirmed | OUT-01 now lists `echo` as a line type but still says the forwarder sends every line to LogPython. |
| [P1-consistency-04](#p1-consistency-04) | minor | missing test | confirmed | The patch added an acceptance criterion to LRS-09 (the docked gutter execution arrow), but no test covers it. |
| [P1-consistency-05](#p1-consistency-05) | minor | test cannot work as written | confirmed | T-OUT-043 toggles the toolbar's Echo input 'during … a run' and expects the Run window's live view to follow. |
| [P1-consistency-07](#p1-consistency-07) | minor | drawing without spec | confirmed | Mockup 01 and SPEC §9.5 show an Alt+Enter hint chip on the caret line, with a behaviour ('shows … until you first use the key'). |
| [P1-consistency-08](#p1-consistency-08) | minor | drawing contradicts text | confirmed | Mockup 04 still draws the v1.1 whole-range outcome bar: a red bar on every line 12–20 of Run #31, a selection run that raised at line 18. |
| [P1-consistency-09](#p1-consistency-09) | minor | drawing contradicts text | confirmed | Behind the Run window, the code view highlights line 43 as the current line. |
| [P1-consistency-10](#p1-consistency-10) | minor | ambiguous rule / test mismatch | confirmed with changes | The rule 'a smaller output area gets the minimum' does not say whether it applies per dimension. |
| [P1-consistency-11](#p1-consistency-11) | minor | missing fixture | confirmed with changes | Two new tier-4 editor tests need a 'flush delay' mode in FaultyFileOps, described as 'a delay mode added to the §10.10 fault injector, proposed'. |
| [P1-consistency-12](#p1-consistency-12) | minor | wrong engine claim | confirmed | The docs say no editor or Slate command binds Alt+Enter in 5.8. |
| [P1-consistency-13](#p1-consistency-13) | minor | stale text | confirmed | D.4's affected-ID lists leave out sections the patch changed. |
| [P1-consistency-14](#p1-consistency-14) | minor | stale text | confirmed | Two amendment notes are out of date. |

#### P1-consistency-01

**Major, test cannot pass / contradiction.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 'The live run view (PO-03)' Hand-off bullet (line 2641); TDD.md §5.15.3 T-SUP-037 (line 3288); SPEC.md LRS-01 (line 276)

**Problem.** At the end of a run the hand-off promises two things that cannot both be true: the same first visible line and follow mode at the bottom ('nothing jumps'). The live run view and the Output tab body have different heights and different vertical offsets inside the same rectangle. So when following, the Output view pinned to its newest line shows a different first line, and the text moves on screen. T-SUP-037 asserts both at once, so it cannot pass. There are also two cases where the area cannot show 'the same block' at all: a buffer run while the REPL tab is active (that view is filtered to route = Repl), and a run while Problems is showing.

**Evidence.** TDD 2641: 'the output area's active Output or REPL view takes the live view's scroll anchor (first visible line ID and pixel offset) and follow state, so after the window is destroyed the area under it shows the same block, scrolled the same way (follow mode at the bottom), and nothing jumps. If Problems was showing, the area keeps it'. TDD T-SUP-037: 'after the run the Output view shows the same block from the same first visible line, in follow mode'. In the Run window (TDD §5.5.3 rows, lines 2627-2635) the live view sits below the Title, State, Location and Progress rows and above the Undo meter, Notes, Buttons and Footer. The output area is the tab row, then the tab body, then the prompt strip (§6.4 line 3710). TDD §6.4: 'Route filter: fixed at route = `Repl` for the REPL tab's view'. SPEC LRS-01 states without exception: 'the output area under it shows the same block, scrolled the same way (follow mode at the bottom), so nothing jumps'.

**Refuter.** Holds on re-reading. TDD 2631 says the live view 'takes the height the other rows leave'. TDD 2641 promises the area 'shows the same block, scrolled the same way (follow mode at the bottom), and nothing jumps'. T-SUP-037 (TDD 3288) asserts 'same first visible line, in follow mode'. I measured mockup 02 in headless Edge (verify/measure_rw.py). The live view is [543,363,1560,556], 193 px tall and starting at y=363. The Output tab body is [528,248,1575,572], 324 px tall and starting at y=248, inside the same 380 px rect. In follow mode (OUT-09, TDD 3820: auto-scroll at the bottom) the Output view therefore shows about 8 more lines above the live view's first line, so T-SUP-037 cannot pass. There is a second way it fails. The live view is filtered to the run, but Output applies no route filter (TDD 3808), so in follow mode a block shorter than the tab body has earlier runs' lines at its top. When not following, the anchored line moves up about 115 px on screen, so 'nothing jumps' is false in both states. The REPL view is fixed to route = Repl (TDD 3808), so after a buffer run with REPL active it cannot show the block. And since 'a run never switches tabs' (TDD 2641), that case is real. The Problems exception exists only in the TDD; SPEC LRS-01 states the promise without it. One note on the fix: mockup 02 callout 4 says only 'shows the same block' and does not contain 'nothing jumps'. 08 §10 PO-03 (line 1773), the top-precedence ruling, carries the same 'scrolled the same way, so nothing jumps' wording.

**Fix applied.** Define the hand-off per follow state, in SPEC LRS-01 and TDD §5.5.3 alike. When following, the Output view is in follow mode with the run's last line at its bottom. When not following, it puts the live view's first visible line (and pixel offset) at its top. Drop 'nothing jumps' (or read it as 'same follow state and anchor'), and note in 08 §10 that this is how 'scrolled the same way' is met. Split T-SUP-037's after-run check the same way: follow on means the Output view follows with the run's last line visible at the bottom; follow off (after a wheel scroll) means the same first visible line. For a non-REPL run that ends while the REPL tab is active, either switch to Output (and say so) or scope the promise to the case where the run's route is visible in the active view. Add the Problems exception to SPEC LRS-01.

#### P1-consistency-02

**Major, acceptance cannot pass / inconsistency.** Verdict: confirmed.

**Where:** SPEC.md LRS-01 acceptance (line 276); TDD.md §5.15.3 T-SUP-037 (line 3288); TDD.md §5.13 'Live run view latency' row (line 3102)

**Problem.** The new LRS-01 criterion requires each printed line to appear within 150 ms of its print. The Run window does not exist until the first pump, about 300 ms in plus up to one pump period. So a line printed in the first ~150 ms of the run (the natural `print(); sleep(0.2)` loop prints at t≈0) cannot meet 150 ms. T-SUP-037 quietly relaxes the rule to p95. It can still fail when a second early line misses too (2 of ~26 lines is 7.7%).

**Evidence.** SPEC LRS-01: 'A script that prints one line every 200 ms for 5 s shows each line in the Run window, which covers the output area, within 150 ms of the print (proposed)'. Same row: 'arms about 300 ms into a run … opens on the first pump'. TDD T-SUP-016: '400 ms case: first pump at 300 ms plus at most one period, and the Run window opens'. TDD T-SUP-037: 'Each line is painted in the live view within 150 ms of its `print` (proposed, p95 …)'.

**Refuter.** Holds. SPEC LRS-01 (line 276) requires 'each line ... within 150 ms of the print'. The same row says the window opens on the first pump, about 300 ms in, and T-SUP-016 (TDD 3268) allows 'first pump at 300 ms plus at most one period'. A line printed at t≈0 cannot be painted before the first pump, so it misses the SPEC criterion every time. T-SUP-037 (TDD 3288) silently relaxes this to p95 and leaves the loop shape open. With print-then-sleep, the lines at t≈0 and t≈200 ms are first painted at 300–400 ms. The second one misses whenever the first paint is after 350 ms, and then the nearest-rank p95 of 26 samples (the 25th value) fails. So the TDD test is flaky and the SPEC criterion is impossible.

**Fix applied.** In both SPEC LRS-01 and T-SUP-037, write: 'each line written after the Run window opens is painted within 150 ms of its write; lines written before the first pump appear at the first pump'. Use one rule in both places (every line, or the same p95), and fix the loop shape in T-SUP-037 (for example print, then sleep).

#### P1-consistency-M-01

**Major, usability / made false by patch.** Raised by the refuter.

**Where:** SPEC.md LRS-01 (line 276): 'The 480 × 200 px minimum stays'; TDD.md §5.5.3 step 4 (line 2610): 'The minimum size stays 480 × 200 px (proposed: fits the rows below)'; TDD.md §5.5.3 Contents, Live run view row (line 2631): 'It takes the height the other rows leave (proposed)'; TDD.md §6.4 Split (each slot keeps at least 120 px); TDD.md T-SUP-036 (line 3287); mockups/02-run-and-cancel.html

**Problem.** PO-03 makes the live run view the run's only output surface: the output and code areas are not drawn during a run (RV-16). But the view only gets the height the fixed rows leave, and the 200 px minimum leaves none. At the minimum, the window shows no output line at all. That is the 150 px case of T-SUP-036, any layout where the output area is under about 250 px (the default 40% split of a Sumi tab docked as a bottom panel; slots may shrink to 120 px), and probably the centred window when the tab is hidden, whose size is never specified. In those layouts the owner's 'realtime execution' (watch the output appear) is lost, and 'fits the rows below' is no longer true.

**Evidence.** I measured mockup 02 in headless Edge (verify/measure_rw.py, a copy of the writer's measure.py with the Edge profile kept under verify/). #runWin = #outArea = [528,222,1575,602], 380 px tall; the live view #rwLive = [543,363,1560,556], 193 px. The other rows take 187 px: title 30 (223–253), body padding 10+10, state row 26 (263–289), location 17 + 7 gap (296–313), meters 32 + 9 gap (322–354), live-view gap 9, footer 35 (566–601). That is the compact layout; it omits the TDD's separate Notes and Buttons rows. At 200 px the live view gets 200 − 187 = 13 px, less than one 17 px line. TDD step 4 says a non-visible output area gives a window 'centred on GetActiveTopLevelRegularWindow(), or the main frame' with no size; T-SUP-036 checks only the centre.

**Fix applied.** Size the minimum from the rows plus a minimum live view, for example at least 5 lines (about 190 px of rows + 90 px gives a 480 × 280 px minimum, proposed). When the output area is shorter, extend the window toward the code area (see P1-consistency-10). Give the centred window an explicit size (for example 640 × 360, proposed). Replace '(proposed: fits the rows below)' accordingly, and add a T-SUP-036 check that the live view shows at least 5 lines in the 150 px case and in the centred case.

#### P1-consistency-03

**Minor, stale text.** Verdict: confirmed.

**Where:** SPEC.md OUT-01 (line 299)

**Problem.** OUT-01 now lists `echo` as a line type but still says the forwarder sends every line to LogPython. That contradicts OUT-12, the §7.6 intro and TDD §6.3, which say echo lines are never forwarded.

**Evidence.** SPEC OUT-01: 'Line types: stdout, stderr …, result, echo (OUT-12), …'; then '`FSumiLogForwarder` forwards every line to LogPython in batches through C++ `UE_LOG`'. SPEC OUT-12: 'Echo lines are **not forwarded to LogPython**'. TDD §6.3: 'Input echo of statement-mode runs (OUT-12) … Never forwarded'.

**Refuter.** Holds. SPEC OUT-01 (line 299) now lists 'echo (OUT-12)' among the line types, then says '`FSumiLogForwarder` forwards every line to LogPython'. That contradicts OUT-12 (line 310: 'not forwarded to LogPython') and the TDD §6.3 forwarding table (line 3655: 'Never forwarded'). The same OUT-01 sentence then says 'above the echo cap ... summary lines point to the run log'. There 'echo' means LogPython forwarding (TDD 3669 'Echo cap ... lines are not echoed'), so right after `echo` is introduced as a line type, a reader could think the cap throttles input echo.

**Fix applied.** In OUT-01, write 'forwards every stdout, stderr, result and log-level line (not `echo` lines, OUT-12; headers and notices go to LogSumi, TDD §6.3) to LogPython ...', and call the cap 'the Output Log forwarding cap (D19)' in that sentence.

#### P1-consistency-04

**Minor, missing test.** Verdict: confirmed.

**Where:** SPEC.md LRS-09 acceptance (line 284); TDD.md §14.7 LRS-09 row (line 8584); TDD.md T-SUP-022 (line 3274)

**Problem.** The patch added an acceptance criterion to LRS-09 (the docked gutter execution arrow), but no test covers it. The 14.7 row lists T-SUP-008, T-SUP-010 and T-SUP-022, and none of them checks the arrow.

**Evidence.** SPEC LRS-09 acceptance: 'During a docked run, the gutter's execution arrow is at the line of the last pump.' T-SUP-022: 'Docked protections | Focus on the banner; buffers read-only; CanClose vetoes; window actions vetoed; completion and hover disabled'. TDD §5.5.4 defines the arrow ('It moves at each pump and is removed in finish step 3') with no test.

**Refuter.** Holds. SPEC LRS-09 (line 284) gained the acceptance 'During a docked run, the gutter's execution arrow is at the line of the last pump'. TDD §5.5.4 (line 2656) defines the arrow. A search of every test row for 'arrow' finds only T-VIEW-017 (the Paused source view) and T-DBG-055. The 14.7 LRS-09 row (TDD 8584) lists T-SUP-008, T-SUP-010 and T-SUP-022, and none of them checks the arrow.

**Fix applied.** Add an arrow step to T-SUP-022 (Lands M4). The arrow is on the last-pump line, moves at each pump, is absent when that line is outside the run's buffer, and is gone after finish step 3. T-SUP-022 is already in cut-list item 3 (TDD 8814), so the step moves with it.

#### P1-consistency-05

**Minor, test cannot work as written.** Verdict: confirmed.

**Where:** TDD.md §6.8 T-OUT-043 (line 4070)

**Problem.** T-OUT-043 toggles the toolbar's Echo input 'during … a run' and expects the Run window's live view to follow. In the M1 Run-window presentation the toolbar is behind the modal, is not drawn, and input to it is swallowed. So no UI path can toggle it during a run, and no API to toggle it is specified.

**Evidence.** T-OUT-043: 'Echo input toggle on the toolbar and the `echo` type toggle, during and after a run | … the Run window's live run view follows it'. SPEC §9.1 Windows: 'while either is open, nothing outside it is drawn or clickable (RV-16)'. TDD §5.6.2 Run window column: 'Mouse down … Pass if inside our window …, else swallow'.

**Refuter.** Holds. T-OUT-043 (TDD 4070) toggles the Echo input toggle 'during and after a run' and expects the live view to follow. In M1 there is no route to toggle it during a run. The toolbar is outside the modal and is neither drawn nor clickable (RV-16; TDD §5.6.2: mouse down passes only inside our window). Automation latent commands run from FTSTicker, which the pump never ticks (TDD §5.15 intro). No Python or test API to set type toggles is specified.

**Fix applied.** Set the toggle before the run and check that the live view honours it. If a mid-run flip is wanted, name a `_sumi_test` hook that the run calls, or make that step a split step for task mode (M3), where the editor paints between steps.

#### P1-consistency-07

**Minor, drawing without spec.** Verdict: confirmed.

**Where:** mockups/01-main-window.html lines 142 and 242 (callout 14); SPEC.md §9.5 row 01 (line 742); SPEC.md RUN-26; TDD.md §4.5 and §4.9.4

**Problem.** Mockup 01 and SPEC §9.5 show an Alt+Enter hint chip on the caret line, with a behaviour ('shows … until you first use the key'). No requirement, design section, setting or test defines it: when it shows, what dismisses it, where 'first use' is stored, and whether it shows in the PyCharm preset, where Alt+Enter is unbound.

**Evidence.** mockup 01 callout 14: 'The hint chip shows on the caret line until you first use the key (proposed).' SPEC §9.5: 'the hint chip "Alt+Enter runs this line and moves to the next" on the caret line (RUN-26)'. A search of SPEC.md and TDD.md for 'hint chip', 'hintchip' or 'runs this line' finds only the §9.5 caption.

**Refuter.** Holds. Mockup 01 line 142 draws the chip, and callout 14 (line 242) gives it behaviour: 'shows on the caret line until you first use the key (proposed)'. SPEC §9.5 (line 742) captions it. No requirement, TDD section, setting or test defines when it shows, what dismisses it, where 'first use' is stored, or whether it shows in the PyCharm preset, where RunLineAdvance has no chord (SPEC §9.3 line 693; T-KEY-019). A search of SPEC.md and TDD.md for 'hint chip' or 'runs this line' finds only the §9.5 caption. The brief required drawing the chip, not specifying it, but the drawing now claims behaviour that nothing defines.

**Fix applied.** Add the chip to RUN-26 and TDD §4.9.4: shown only while RunLineAdvance has a chord, hidden after the first RunLineAdvance, with the flag in per-user session state. Add a T-KEY-018 step. Otherwise cut the behaviour sentence from callout 14.

#### P1-consistency-08

**Minor, drawing contradicts text.** Verdict: confirmed.

**Where:** mockups/04-traceback-to-source.html lines 26, 177-185 (image 04, panel A); mockups/01-main-window.html lines 136-141 and mockups/ue.css lines 245-249; TDD.md §4.8.2 Paint bullet (line 1517)

**Problem.** Mockup 04 still draws the v1.1 whole-range outcome bar: a red bar on every line 12–20 of Run #31, a selection run that raised at line 18. Under EDT-18, which replaced those markers, statements 1–3 (lines 12–16) would be ok, lines 17–19 raised, and line 20 unmarked. Mockup 01 and ue.css tint the raised and stopped lines red and amber ('each with a light tint'), but TDD §4.8.2 tints only ok lines.

**Evidence.** 04 css line 26: '.lines .rng { box-shadow: inset 3px 0 0 var(--err); } /* range that ran, failed */', applied to lines 12–20. TDD §4.8.2 gutter table: 'one bar per line of each statement that completed, raised or was running when Stop took effect'. TDD 1517: 'a light tint of the ok colour behind ok lines (layer 1, mockup 01's `.ran` style)'. ue.css 245: 'ok is a solid bar (.ran), raised a dashed bar, stopped a dotted bar, each with a light tint'.

**Refuter.** Holds on both counts. Mockup 04 line 26 keeps '.lines .rng { box-shadow: inset 3px 0 0 var(--err); } /* range that ran, failed */' on every line 12–20 of Run #31. The patch moved that panel's output area but left the code view as it was. Under EDT-18 / TDD §4.8.2 (line 1509: 'one bar per line of each statement that completed, raised or was running when Stop took effect'), lines 12–16 would be ok, the `for` statement (17–19) raised, and line 20 unmarked. On the tint: TDD 1517 says 'a light tint of the ok colour behind ok lines', while ue.css 245–249 gives `.ranerr` and `.ranstop` red and amber tints and says 'each with a light tint'.

**Fix applied.** Redraw mockup 04 panel A with per-statement marks: `.ran` on 12–16, `.ranerr` on 17–19, nothing on 20. Pick one tint rule: either TDD §4.8.2 tints every outcome in its own colour, or drop the backgrounds from `.ranerr` and `.ranstop`.

#### P1-consistency-09

**Minor, drawing contradicts text.** Verdict: confirmed.

**Where:** mockups/02-run-and-cancel.html line 225 (code view) and line 441 (callout 8)

**Problem.** Behind the Run window, the code view highlights line 43 as the current line. That is exactly the line the Location row says is executing, while the newly added caption says the running line is not shown in the code view. A reader will take the highlight for an execution marker.

**Evidence.** Line 225: '<div class="curline">    tools.import_asset_tasks([task])</div>' (line 43, status bar 'Ln 43, Col 5'). Callout 8: 'the code area as it was, so the running line shows in the Location row, not in the code view'. The Location row reads 'Line 43 · tools.import_asset_tasks([task])'.

**Refuter.** Holds as a misleading drawing, though not a strict contradiction. The highlight is the caret line (status 'Ln 43, Col 5', mockup 02 line 238), and the same coincidence was in v1.1 (caret on line 31, location 31). But the patch added callout 8's 'so the running line shows in the Location row, not in the code view' (line 441) and the Location text 'Line 43 · tools.import_asset_tasks([task])' (line 262). With the caret highlight on exactly that line, the drawing visually suggests the opposite of the caption.

**Fix applied.** Move the caret and current-line highlight to another line (for example line 27, with the status bar at 'Ln 27, Col 1'), so that line 43 appears only in the Location row.

#### P1-consistency-10

**Minor, ambiguous rule / test mismatch.** Verdict: confirmed with changes.

**Where:** SPEC.md LRS-01 (line 276); TDD.md §5.5.3 step 4 (line 2610); TDD.md T-SUP-036 (line 3287); TDD.md §5.14 row A-080 (line 3174)

**Problem.** The rule 'a smaller output area gets the minimum' does not say whether it applies per dimension. T-SUP-036 makes a 150 px tall but full-width area yield a 480 × 200 window, which shrinks the width below the area it is supposed to cover exactly. A per-dimension clamp (area width × 200) would fail the test. Also, when the area sits at the bottom, anchoring at its top-left makes a taller window grow downward past the tab's bottom edge. The failure-mode row says it 'overlaps the code area a little', which is true only for the top position.

**Evidence.** TDD 2610: 'a smaller output area gets the minimum, anchored at the output area's top-left'. T-SUP-036: 'with the output area dragged to 150 px … the 150 px area gives a 480 × 200 px window anchored at the area's top-left'. A-080 outcome: 'The Run window overlaps the code area a little, or opens centred'.

**Refuter.** Partly refuted. There is no rule/test mismatch: SPEC LRS-01 and TDD 2610 say a smaller area 'gets the minimum', and T-SUP-036 (3287) reads that literally as 480 × 200, so doc and test agree. What remains is real. (1) A full-width 150 px area gives a 480 px wide window, so most of the area's frozen frame stays visible beside it. That breaks 'the Run window is the output area' (TDD 2637). (2) With `layout.output_position = bottom`, anchoring at the area's top-left grows the window downward past the area, over the Sumi and editor status bars and possibly off the work area. The A-080 outcome (TDD 3174), 'overlaps the code area a little', is true only for the top position. See also P1-consistency-M-01: at 200 px the live view has no room.

**Fix applied.** Clamp per dimension: width = max(area width, 480), height = max(area height, minimum). Grow toward the code area: down from the top-left when the output area is on top, up from the bottom-left when it is at the bottom. Update T-SUP-036 to expect area width × minimum height in both positions, and make A-080's outcome say the window extends over the code area by the shortfall.

#### P1-consistency-11

**Minor, missing fixture.** Verdict: confirmed with changes.

**Where:** TDD.md T-VIEW-021 (line 1574); TDD.md T-OUT-045 (line 4072); TDD.md §10.10 Fault injection (line 6838); TDD.md §14.2 kill -9 harness row (line 8421)

**Problem.** Two new tier-4 editor tests need a 'flush delay' mode in FaultyFileOps, described as 'a delay mode added to the §10.10 fault injector, proposed'. §10.10 and the §14.2 fixtures were not updated. FaultyFileOps is described only as a tier-1 fixture for the journal-torture driver, and nothing says how the editor's journal writer would use it in a tier-4 session.

**Evidence.** T-VIEW-021 and T-OUT-045: 'held by a flush delay in `FaultyFileOps`, a delay mode added to the §10.10 fault injector, proposed'. §10.10: '**Fault injection (tier 1).** `FaultyFileOps` tears writes …, drops unflushed data …, fails `rename` …, and returns `ENOSPC` and `EACCES`.' The §14.2 harness row lists the same modes and no delay.

**Refuter.** Narrowed. The claim that FaultyFileOps is tier-1 only, with no tier-4 injection path, is wrong. T-PER-047 (TDD 6911) is a tier-4 test 'through `FaultyFileOps`', and §10.13 (6931) says the editor's file operations 'go through the platform `FileOps` (§10.2.1), so `FaultyFileOps` can inject faults'. T-PER-015 (6879, tier 4) already holds a run in the pre-run snapshot wait with 'a hung `FileOps`'. T-PER-041 (6905) uses 'a `FileOps` whose flush blocks forever' and then releases it. The only defect is the dangling reference: T-VIEW-021 and T-OUT-045 say the delay mode was 'added to the §10.10 fault injector', but §10.10 (6838) and the §14.2 harness row (8421) list no such mode. Note also that T-PER-015 makes a held snapshot end the run `not executed` after 2 s, so the hold must be released within 2 s.

**Fix applied.** In T-VIEW-021 and T-OUT-045, write 'held by a hung FileOps as in T-PER-015, released by the driver within 2 s', or add one clause about a release-controlled flush delay to §10.10 and the §14.2 fixture rows.

#### P1-consistency-12

**Minor, wrong engine claim.** Verdict: confirmed.

**Where:** SPEC.md RUN-26 (line 267) and §9.3 intro; TDD.md §4.9.4 RunLineAdvance bullet (line 1716); research/08-decision-log.md §10 PO-02 row

**Problem.** The docs say no editor or Slate command binds Alt+Enter in 5.8. The PCG plugin, enabled by default, binds Alt+Enter in its own TCommands context. The conclusion still holds (it is a tool-local binding that never acts while Sumi has focus), but the stated fact is false.

**Evidence.** Engine/Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:69: 'UI_COMMAND(ApplyChanges, "Apply Changes", "Apply Changes", EUserInterfaceActionType::Button, FInputChord(EKeys::Enter, EModifierKey::Alt));' in context 'PCGShaderTextEditorDocumentTextBox'. PCG.uplugin:13: '"EnabledByDefault": true'. SPEC RUN-26: 'No editor or Slate command binds Alt+Enter in 5.8; the only handler is the game viewport's fullscreen toggle'.

**Refuter.** Verified in the engine. Engine/Plugins/PCG/Source/PCGEditor/Private/Widgets/SPCGCodeEditorTextBox.cpp:69 has 'UI_COMMAND(ApplyChanges, "Apply Changes", ..., FInputChord(EKeys::Enter, EModifierKey::Alt))' in context 'PCGShaderTextEditorDocumentTextBox' (line 59). PCG.uplugin:7 is 'Category: Editor' and :13 is 'EnabledByDefault: true'. My regex search over Engine/Source and Engine/Plugins found no other Alt+Enter chord; the only other handler is GameViewportClient.cpp:674. So 'No editor or Slate command binds Alt+Enter in 5.8' is false in SPEC RUN-26, SPEC §9.3 intro (line 685), TDD §4.9.4 (1716) and 08 §10 PO-02 (1772). The conclusion still holds: the binding is tool-local, and ComputeShadowed covers only main-frame, Level Editor and consume-list commands (TDD 1717, 7105), so T-KEY-018's 'no Alt+Enter row' stays correct.

**Fix applied.** Reword in all four places: 'No main-frame, Level Editor or consume-list command binds Alt+Enter in 5.8. The only bindings are tool-local (the PCG HLSL editor's Apply Changes, SPCGCodeEditorTextBox.cpp:69) and the game viewport's fullscreen toggle (GameViewportClient.cpp:674), and none acts while a Sumi widget has focus.'

#### P1-consistency-13

**Minor, stale text.** Verdict: confirmed.

**Where:** SPEC.md Appendix D.4 (lines 1083-1089)

**Problem.** D.4's affected-ID lists leave out sections the patch changed. OUT-01 (the echo line type) is in no row. The PO-02 rows also omit §1 (the Alt+Enter and echo bullet) and the §7.6 OUT intro, and the PO-03 row omits the §7.5 LRS intro.

**Evidence.** D.4 PO-02c affected IDs: 'OUT-03, OUT-05, OUT-10, OUT-12, REPL-01, §2.1, §9.1'. SPEC.md diff: OUT-01 gained 'echo (OUT-12)'; §1 gained 'Alt+Enter to run a line and move to the next, each run echoed into the output statement by statement'; §7.6 intro became 'Decided in D19 as amended by RT-07 and PO-02 …'; §7.5 intro gained 'PO-03 makes the Run window the live output area'.

**Refuter.** Holds. The SPEC diff changes OUT-01 (it adds 'echo (OUT-12)' as a line type), but no D.4 row (SPEC 1083–1089) lists OUT-01. The §1 bullet that gained 'Alt+Enter to run a line and move to the next, each run echoed into the output statement by statement' is PO-02 content, yet §1 appears only in the PO-01 and PO-03 rows. The §7.5 and §7.6 intro changes are trivial.

**Fix applied.** Add OUT-01 to the PO-02c row and §1 to the PO-02a or PO-02c row. Listing the §7.5 and §7.6 intros is optional.

#### P1-consistency-14

**Minor, stale text.** Verdict: confirmed.

**Where:** research/08-decision-log.md header Date row (line 6); TDD.md §15 intro (line 8772)

**Problem.** Two amendment notes are out of date. 08's header Date still reads 2026-10-28, although its Status row now cites §10, which was accepted on 2026-10-29. The TDD §15 delivery-plan intro still lists the amendments without PO-01, which moved WS-03 to M1; SPEC §10's matching sentence was updated.

**Evidence.** 08 line 6: '| **Date** | 2026-10-28 |'; 08 §10: '**Status:** Accepted, 2026-10-29'. TDD 8772: 'The plan follows D9 as amended by RT-10, RT-12, RT-13, RT-20 and RV-11 to RV-15, with SPEC §6 and §10.' SPEC §10: 'D9 as amended by RT-10, RT-12, RT-13, RT-20, RV-11 to RV-15, and PO-01 and PO-02 (WS-03 moves to M1; …)'.

**Refuter.** Holds. research/08 line 6 still reads 'Date | 2026-10-28', while its Status row (line 7) now cites §10, which is 'Accepted, 2026-10-29'. TDD §15 intro (8772) still reads 'D9 as amended by RT-10, RT-12, RT-13, RT-20 and RV-11 to RV-15', while SPEC §10 (757) adds 'PO-01 and PO-02 (WS-03 moves to M1; RUN-26, RUN-27 and OUT-12 land in M1, EDT-18 in M2)'. SPEC §6 (line 149) differs from both: it adds PO-01 only.

**Fix applied.** Set 08's Date to '2026-10-28; amended 2026-10-29 (§10)'. Add 'and PO-01 and PO-02 (WS-03 to M1; RUN-26, RUN-27, OUT-12 in M1; EDT-18 in M2)' to the TDD §15 intro, and align SPEC §6's list with SPEC §10.

### Round 1 — UX and mockups

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [P1-ux-01](#p1-ux-01) | major | usability flaw that defeats the owner's request | confirmed | Ran-line marks belong only to the buffer's last run, and every Alt+Enter press is a separate run of that buffer. |
| [P1-ux-02](#p1-ux-02) | major | requirement/test that works against line by line execution | confirmed with changes | The advance target can make the next Alt+Enter re-execute code that already ran. |
| [P1-ux-M-01](#p1-ux-m-01) | major | test that cannot pass / realtime hand off claim | added by refuter | The live run view is always about 130 px shorter than the Output view it hands off to: the Run window's rows take about 187 px, while the Output view loses only the tab row and the prompt strip. |
| [P1-ux-03](#p1-ux-03) | minor | usability flaw (Run window hides what the user needs) / test that encodes it | confirmed with changes | The 480 × 200 px minimum was sized for the old rows. |
| [P1-ux-04](#p1-ux-04) | minor | contradiction (SPEC vs TDD) / realtime visibility | confirmed | SPEC promises that after the run the output area shows the same block, scrolled the same way. |
| [P1-ux-05](#p1-ux-05) | minor | drawing contradicts the text | confirmed | Mockup 04 still draws the v1.1 run-range bar: one solid red bar over all of Run #31's lines 12-20. |
| [P1-ux-06](#p1-ux-06) | minor | drawing ambiguity / A11Y 05 / drawing vs TDD | confirmed | In mockup 01 the ok ran mark and the change bars are both solid 3 px green bars side by side. |
| [P1-ux-08](#p1-ux-08) | minor | drawing vs input rules / keyboard reachability | confirmed | The echo fold chip is drawn as a clickable button inside the Run window's live view, but clicks there are swallowed during the run, so the chip is inert exactly where the user is watching. |
| [P1-ux-09](#p1-ux-09) | minor | missing piece / keymap gap | confirmed with changes | The "Alt+Enter runs this line and moves to the next" hint chip is named in SPEC 9.5 and drawn, but no requirement or design defines when it shows or hides. |
| [P1-ux-10](#p1-ux-10) | minor | rule that erases the history | confirmed | REPL and console headers no longer show the submitted code, and echo can be hidden by the Echo input toggle or the Minimal preset. |
| [P1-ux-11](#p1-ux-11) | minor | missing piece the brief required (drawing) | confirmed | No drawing shows output sitting between two statements' echoes, which is the line-by-line history the owner asked for. |
| [P1-ux-12](#p1-ux-12) | minor | drawing contradicts the text | confirmed | Mockup 01 puts the transaction note after the echo lines. |
| [P1-ux-14](#p1-ux-14) | minor | drawing vs callout | confirmed | The frozen code view behind the Run window highlights line 43 (current-line tint, bold number, "Ln 43, Col 5"), which is the executing line in the Location row. |
| [P1-ux-15](#p1-ux-15) | minor | visual quality (overlap) | confirmed | Now that the output area sits between the code and the shelf, the dashed drag path from the code selection to the shelf crosses the output area. |
| [P1-ux-M-02](#p1-ux-m-02) | minor | drawing contradicts the text | added by refuter | Callout 4 now says line 15 was typed after the run, and that the gutter marks lines changed since the last save. |

#### P1-ux-01

**Major, usability flaw that defeats the owner's request.** Verdict: confirmed.

**Where:** SPEC.md §7.3 EDT-18; TDD.md §4.8.2 Ran-line marks (Lifetime); TDD.md T-VIEW-020; SPEC.md §7.4 RUN-26; images/01-main-window.png

**Problem.** Ran-line marks belong only to the buffer's last run, and every Alt+Enter press is a separate run of that buffer. So the Maya-style walk erases the trail at each press, and the code view never shows which lines ran (owner ask (c)). It only shows the one statement run last.

**Evidence.** SPEC EDT-18: "Marks belong to the buffer's last run: the next run of that buffer replaces them". TDD §4.8.2: "Lifetime. The next run of that buffer replaces the set" and "When a statement-mode run (RUN-27) of the buffer ends (selection, line, cell or buffer route)". T-VIEW-020: "the next run of the buffer replaces the set". RUN-26: "Each press is one numbered run (U8)… repeated presses walk the buffer statement by statement". After five Alt+Enter presses, statements 1-4 are unmarked and only statement 5 carries a mark. Mockup 01 shows the effect already: cell [1] ran ok in Run #13 (echo lines 2-5 in Output), yet code lines 2-5 have no mark, because cell [2]'s Run #14 replaced the set (crop verify/m01_gutter.png).

**Refuter.** The current docs say what the verifier quotes. SPEC EDT-18: "Marks belong to the buffer's last run: the next run of that buffer replaces them". TDD §4.8.2 Data covers line routes ("When a statement-mode run (RUN-27) of the buffer ends (selection, line, cell or buffer route)"), and Lifetime says "The next run of that buffer replaces the set". RUN-26 makes every Alt+Enter press its own run. So after an Alt+Enter walk only the last statement is marked, and the earlier statements look the same as "Statements not reached stay unmarked". That defeats interpretation (c) exactly in workflow (a). Nothing else in the docs gives the code view a trail. Cell header tags cover cells only, and the echo trail is in Output, not the code view. Mockup 01 shows the effect: cell [1] ran in Run #13, yet lines 2-5 carry no mark. v1.1 also cleared marks at the next run, but v1.1 had no run-per-line walk, so the patch created this interaction.

**Fix applied.** Make marks accumulate per buffer. A run replaces marks only on the lines it covers, and each mark keeps its own run number ("Ran in #23 · ok"). Clear the whole set on Run whole buffer, Run File, Reset Session, or a new View ▸ Clear Ran-Line Marks command. Change SPEC EDT-18 (sentence and acceptance), TDD §4.8.2 Lifetime and T-VIEW-020, adding: three Alt+Enter presses leave statements 1-3 marked ok with runs #N..#N+2. Update mockup 01: lines 2-5 get ok marks from Run #13, and callout 4 drops "the next run of the buffer replaces the marks".

#### P1-ux-02

**Major, requirement/test that works against line by line execution.** Verdict: confirmed with changes.

**Where:** TDD.md §4.5 Statement extent (rules 3 and 5, Next statement); TDD.md T-CORE-037, T-CORE-038; SPEC.md §7.4 RUN-26

**Problem.** The advance target can make the next Alt+Enter re-execute code that already ran. (a) Stepping line by line through an if/try/for body lands the caret on the following elif/else/except/finally line. The next press then runs the whole compound statement from its header, so the body lines just walked run again with their side effects (renames, imports, spawns). (b) At the end of the buffer the caret stays on the last statement, so a further press re-runs it.

**Evidence.** TDD §4.5 Terms: a continuation is only a line that "starts in a string… after a trailing backslash… or at a positive bracket depth"; a statement start is "the first line of a logical line that is not blank and not comment-only", so `else:` qualifies. Next statement: "`NextStatementLine(Last)` is the first statement start after line `Last`". Rule 3: "if S's first token is `elif`, `else`, `except` or `finally`, S moves up to the previous statement start at the same indentation", and rule 5 then runs the whole compound statement. T-CORE-038 only forbids "a blank, comment-only or continuation line" as a target; T-CORE-037 asserts that "the clause and `case` carets give the whole compound statement". End of buffer: "When there is none, the caret stays and the status bar says 'End of buffer'". Example: in `try:` / `a = load()` / `b = process(a)` / `except Exception:` / `log()`, pressing Alt+Enter on `a = load()`, then `b = process(a)`, lands on `except Exception:`, and the third press re-runs load() and process().

**Refuter.** Part (a) holds. Under TDD §4.5 Terms, a continuation is a line in a string, after a backslash or at positive bracket depth, so `except Exception:` is a statement start and a valid NextStatementLine target (T-CORE-038 forbids only blank, comment-only and continuation targets). Rule 3 then lifts S to `try:`, and rule 5 runs the whole statement. I simulated rules 1-6 and NextStatementLine on the engine Python 3.11.8 (verify/uxref/walk_probe.py, output in walk_probe.out): "press 2: caret line 3 -> runs lines 3-3 ['b = process(a)'] ; caret -> 4" and "press 3: caret line 4 -> runs lines 1-5 ['try:', 'a = load()', 'b = process(a)', 'except Exception:', 'log()']". So load() and process() run a second time, and so would non-undoable renames or imports. The same happens after stepping an if body (it lands on else/elif, then the whole if/else re-runs), a for/while body with else, and a case body (it lands on the next case, then the whole match re-runs). Part (b) does not hold. The brief and the SPEC acceptance choose this deliberately ("the caret ends on the last line with 'End of buffer'"), it matches RStudio-style run-and-advance, and the press that runs the last statement already shows the "End of buffer" notice. A later press is a plain run-current-line.

**Fix applied.** Drop the end-of-buffer part. In TDD §4.5 Next statement, add this rule. When the executed range lies inside a compound statement's body and the first statement start after it is a continuation clause of that statement (elif/else/except/finally at the header's indentation, or a sibling case of the enclosing match), skip that clause and its body, and every later clause, so the walk lands on the first statement start after the whole compound statement with indentation no deeper than its header. Mirror it in SPEC RUN-26 ("…and not a clause of a compound statement whose body the run was inside"). Add T-CORE-038 and T-RUN-029 cases: stepping a try body lands after the try statement, and stepping an if body skips its else.

#### P1-ux-M-01

**Major, test that cannot pass / realtime hand off claim.** Raised by the refuter.

**Where:** TDD.md §5.5.3 The live run view (Hand-off); TDD.md T-SUP-037; SPEC.md §7.5 LRS-01

**Problem.** The live run view is always about 130 px shorter than the Output view it hands off to: the Run window's rows take about 187 px, while the Output view loses only the tab row and the prompt strip. So "take the first visible line" and "follow mode at the bottom" cannot both hold after hand-off. In follow mode the taller Output view, pinned to the bottom, starts about 8 lines earlier. Out of follow mode, which PO-03 explicitly supports with the wheel and mockup 02 draws, keeping the same first visible line moves the text about 115 px up the screen. "Nothing jumps" is false either way, and T-SUP-037 asserts both conditions at once.

**Evidence.** TDD §5.5.3: "the output area's active Output or REPL view takes the live view's scroll anchor (first visible line ID and pixel offset) and follow state, so after the window is destroyed the area under it shows the same block, scrolled the same way (follow mode at the bottom), and nothing jumps". T-SUP-037 (a 5 s follow-mode run): "after the run the Output view shows the same block from the same first visible line, in follow mode". SPEC LRS-01: "scrolled the same way (follow mode at the bottom), so nothing jumps". Headless-Edge measurement of mockup 02 (verify/uxref/m02_handoff.out): #runWin [528,222,1575,602] = #outArea; #rwLive [543,363,1560,556] (193 px, about 11 lines at 17 px); #outArea .out [528,248,1575,572] (324 px, about 19 lines). At the bottom the first visible lines differ by about 8. With a first-line anchor, that line moves from y≈363 to y≈248.

**Fix applied.** Hand off by mode. In follow mode, pin the Output view to the bottom (the newest line stays at the bottom, with more history above), and drop "from the same first visible line" from T-SUP-037. Out of follow mode, keep the anchor line at the same screen y (scroll the Output view so the live view's first visible line is drawn at its old screen y, clamped at the start of the store). Assert in T-SUP-037 that the anchor line's screen y changes by no more than a few px. Reword "nothing jumps" in SPEC LRS-01 to match.

#### P1-ux-03

**Minor, usability flaw (Run window hides what the user needs) / test that encodes it.** Verdict: confirmed with changes.

**Where:** TDD.md §5.5.3 step 4 (Position) and the Live run view row; SPEC.md §7.5 LRS-01; TDD.md T-SUP-036; SPEC.md WS-02 (120 px minimum); images/02-run-and-cancel.png

**Problem.** The 480 × 200 px minimum was sized for the old rows. Now the live run view is the reason the window exists, and it takes only the height the other rows leave. At the minimum height that is under one line, and less still when the Notes row or Interrupt cleanup shows. T-SUP-036 also expects a short but wide output area to yield a 480-px-wide window, so the window no longer covers the area it should be standing in for.

**Evidence.** TDD §5.5.3: "The minimum size stays 480 × 200 px (proposed: fits the rows below); a smaller output area gets the minimum". Live run view row: "It takes the height the other rows leave (proposed)". WS-02: "each area keeps at least 120 px". T-SUP-036: "the 150 px area gives a 480 × 200 px window anchored at the area's top-left". Headless-Edge DOM measurement of mockup 02 (verify/measure.py): #runWin [528,222,1575,602], #rwLive [543,363,1560,556], #rwFoot [529,566,1574,601]. The fixed rows take 141 px above the live view and 46 px below it, 187 px in all, so a 200 px window leaves about 13 px for a view whose lines are about 17 px tall.

**Refuter.** The numbers hold. I re-measured mockup 02 (verify/uxref/m02_handoff.out): #runWin [528,222,1575,602] and #rwLive [543,363,1560,556]. The rows take 141 px above the live view and 46 px below it, 187 px in all, so a 200 px window leaves about 13 px, under one 17 px line. The TDD's own row list (Title, State, Location, Progress, Undo meter, Notes, Buttons, Footer) cannot fit in 200 px with a usable live view either, so "(proposed: fits the rows below)" is wrong. T-SUP-036's expectation "the 150 px area gives a 480 × 200 px window" also shrinks the width of a ~1000 px-wide area to 480 when only the height is short. With output at the bottom, anchoring at the top-left pushes the extra height below the tab. It is minor rather than major because it only bites when the user has shrunk the output area below about 200-290 px. The default split is 40% of the centre column, and the brief itself said the 480 × 200 minimum stays.

**Fix applied.** Clamp each dimension on its own: width = the output area's width (at least 480), height = max(area height, H_min). Grow toward the code area (down when the area is on top, up when it is at the bottom). Derive H_min from the fixed rows plus at least 3-5 live-view lines (about 250-290 px at default zoom, proposed), and drop "fits the rows below". Change T-SUP-036 to expect the area's width × H_min, with the live view showing at least that many lines.

#### P1-ux-04

**Minor, contradiction (SPEC vs TDD) / realtime visibility.** Verdict: confirmed.

**Where:** SPEC.md §7.5 LRS-01; TDD.md §5.5.3 The live run view (Hand-off); SPEC.md WS-02

**Problem.** SPEC promises that after the run the output area shows the same block, scrolled the same way. The TDD keeps Problems if it was showing, and it hands the anchor to a REPL view that is filtered to route = REPL and so cannot hold a buffer run's lines. With REPL or Problems active, a run's echo and output are not visible at all, including every short Alt+Enter run, because "a run never switches tabs".

**Evidence.** LRS-01: "When the run ends, the window is destroyed and the output area under it shows the same block, scrolled the same way (follow mode at the bottom), so nothing jumps." TDD hand-off: "the output area's active Output or REPL view takes the live view's scroll anchor (first visible line ID…)… If Problems was showing, the area keeps it (proposed: a run never switches tabs)." WS-02: "the REPL tab is a filtered view of the output store (route = REPL)".

**Refuter.** SPEC LRS-01 promises without condition: "the output area under it shows the same block, scrolled the same way". TDD §5.5.3 Hand-off hands the anchor to "the output area's active Output or REPL view" and keeps Problems if it was showing ("a run never switches tabs"). TDD §6.5 makes the REPL tab "a filtered view … with the route filter fixed at Repl", so after a buffer or line run the REPL view cannot hold the anchor line, and the hand-off as written cannot work there. With Problems active, the block is not shown at all. So SPEC and TDD contradict each other, and the LRS-01 acceptance ("after the run the output area shows the same block") fails in those tab states. That short runs never switch tabs is v1.1 behaviour, but the patch added the hand-off promise.

**Fix applied.** At run start (or at hand-off), select the Output tab when the active tab cannot show the run (Problems, or REPL for a non-REPL route). Alternatively, state the exception in LRS-01 and its acceptance. Add a T-SUP-037 case starting a buffer run with REPL active and with Problems active.

#### P1-ux-05

**Minor, drawing contradicts the text.** Verdict: confirmed.

**Where:** images/04-traceback-to-source.png panel A; mockups/04-traceback-to-source.html; SPEC.md EDT-18; TDD.md §4.8.2 Paint

**Problem.** Mockup 04 still draws the v1.1 run-range bar: one solid red bar over all of Run #31's lines 12-20. That contradicts EDT-18 per-statement marks and their patterns (solid is the ok pattern), and it marks line 20, which never ran.

**Evidence.** Crop verify/m04_a.png: a solid red left bar from line 12 to line 20; the red tint and ✖ are on line 18. Run #31 covers Untitled-3 lines 12-20 and raises at line 18 inside `for mesh in meshes:` (lines 17-19). By EDT-18, lines 12-16 (three completed statements) should carry the ok mark, lines 17-19 the raised mark, and line 20 (`print(...)`, not reached) none. The ue.css v1.2 comment says "ok is a solid bar (.ran), raised a dashed bar, stopped a dotted bar". The patch moved this panel's output area above the code but kept the bar.

**Refuter.** Mockup 04 still uses its local v1.1 rule `.lines .rng { box-shadow: inset 3px 0 0 var(--err); } /* range that ran, failed */` on every line 12-20 of the code view (HTML: `<div class="rng">` ×9, `rng errline` on 18). The patch did not touch it, as the mockups.diff for 04 has no rng change. Run #31 is a selection run (lines 12–20), so under RUN-27 it ran in statement mode and EDT-18 applies: the statements at 12, 13-14 and 15-16 completed (ok, solid), 17-19 raised (dashed), and 20 was not reached (unmarked). The drawing shows a solid error bar over all nine lines, including line 20.

**Fix applied.** Redraw lines 12-16 with .ran, 17-19 with .ranerr and 20 unmarked, keeping the OUT-11 tint and ✖ on 18. Or remove the bar and point to mockup 01 for marks.

#### P1-ux-06

**Minor, drawing ambiguity / A11Y 05 / drawing vs TDD.** Verdict: confirmed.

**Where:** images/01-main-window.png code gutter; mockups/ue.css (.gutter .chg, .lines .ran, .ranerr); TDD.md §4.8.2 gutter table and Paint

**Problem.** In mockup 01 the ok ran mark and the change bars are both solid 3 px green bars side by side. Lines 10-12 (change bars plus the dashed red raised bar) therefore read as "ok and raised", and neither colour nor pattern tells an ok mark from an added-line bar. The drawing also tints raised lines red, while the TDD tints only ok lines.

**Evidence.** ue.css: `.gutter .chg { box-shadow: inset -3px 0 0 #2e7d32; }` and `.lines .ran { … box-shadow: inset 3px 0 0 var(--ok); }` with `--ok: #4caf50`. mockup 01 HTML: lines 10-12 are `class="chg"` in the gutter and `class="ranerr"` in the text (crop verify/m01_gutter.png). `.ranerr` adds `rgba(255,82,82,.11)`. TDD §4.8.2: "Change bars | Added, modified, deleted wedge" and Paint: "a light tint of the ok colour behind ok lines" (no error or stop tint).

**Refuter.** ue.css draws change bars at the gutter's right edge (`.gutter .chg { box-shadow: inset -3px 0 0 #2e7d32; }`) and the ok mark at the text's left edge (`.lines .ran { … inset 3px 0 0 var(--ok) }`, --ok #4caf50). These are two adjacent solid green 3 px bars. In the crop (verify/m01_gutter.png), lines 10-12 show a green bar beside the dashed red bar, which reads as ok plus raised. The lane order also contradicts TDD §4.8.2 (Line numbers | Run and ran markers | Change bars), where ran marks sit left of the change bars. The drawing (`.ranerr` adds rgba(255,82,82,.11); `.ranstop` adds rgba(255,179,0,.10); the css comment says "each with a light tint") also disagrees with TDD Paint, which tints only ok lines.

**Fix applied.** Draw ran marks in the TDD's run lane (left of the change bars), with change bars as the §4.6 wedges. Or give change bars a shape that cannot be mistaken for a solid ok bar. Decide whether raised and stopped statements get a tint, and make TDD §4.8.2 Paint, SPEC EDT-18 and ue.css say the same.

#### P1-ux-08

**Minor, drawing vs input rules / keyboard reachability.** Verdict: confirmed.

**Where:** images/02-run-and-cancel.png live run view; TDD.md §5.6.4 (live run view); TDD.md §5.5.3; SPEC.md OUT-12 Folding; TDD.md §6.4 Echo folding

**Problem.** The echo fold chip is drawn as a clickable button inside the Run window's live view, but clicks there are swallowed during the run, so the chip is inert exactly where the user is watching. In Output, the chip "expands in place" by click only, and no keyboard path is specified.

**Evidence.** Mockup 02 HTML: `<span class="foldchip" id="foldChip">… 3 more lines</span>`, styled with a border and background (ue.css `.foldchip`). TDD §5.6.4: "takes mouse-wheel events, scrollbar drags and clicks on its '↓ n new lines' chip… A click on its text is swallowed in the view". §5.5.3: "the pre-processor passes exactly these". OUT-12: "a chip '… 34 more lines' that expands in place".

**Refuter.** Mockup 02 draws `<span class="foldchip" id="foldChip">… 3 more lines</span>` in the live view, styled as a bordered button (ue.css .foldchip). TDD §5.6.4 passes only wheel events, scrollbar drags and clicks on the "↓ n new lines" chip, and says "A click on its text is swallowed in the view". §5.6.2's mouse-down row lists only the scrollbar and the new-lines chip. So the drawn chip does nothing during the run. In Output, OUT-12 and TDD §6.4 Echo folding say only that the chip "expands in place", with no command, even though §4.11 says "every command is in the palette" (A11Y-01).

**Fix applied.** Either pass clicks on echo fold chips in the live view (and list them in §5.6.2, §5.6.4 and T-SUP-038), or draw the chip as inert text during runs. Add Expand / Collapse Echo commands (context menu and palette, and Enter on the chip line in the read-only Output view).

#### P1-ux-09

**Minor, missing piece / keymap gap.** Verdict: confirmed with changes.

**Where:** images/01-main-window.png callout 14; SPEC.md §9.5 row 01; SPEC.md RUN-26; TDD.md §4.9.1 sub-handler order

**Problem.** The "Alt+Enter runs this line and moves to the next" hint chip is named in SPEC 9.5 and drawn, but no requirement or design defines when it shows or hides. Only a mockup callout says so. It is drawn with the completion popup open, and the TDD does not say whether Alt+Enter then reaches the popup's Enter handler or RunLineAdvance. It is also drawn on an incomplete indented line (`unreal.EditorAssetLibrary.sa`), where Alt+Enter would raise AttributeError.

**Evidence.** Callout 14: "The hint chip shows on the caret line until you first use the key (proposed)." No match for "hint chip" in SPEC requirements or TDD. TDD §4.9.1: "Sub-handler order: snippet session…, popup (Up, Down, PgUp, PgDn, Enter, Tab, Esc), prompt", ahead of "Bound for this context?". Mockup 01 HTML: the hintChip span sits inside the curline beside popup #compl (measured hintChip [687,973,945,992] directly above #compl [635,993,955,1075]).

**Refuter.** The hint chip is defined only in mockup 01 callout 14 ("until you first use the key (proposed)") and named in SPEC §9.5 row 01 as "(RUN-26)". RUN-26 and TDD §4.5/§4.8 say nothing about it, so a reader cannot tell whether to build it or when it shows. It is drawn on an incomplete indented line (`unreal.EditorAssetLibrary.sa`, inside the `with` body) while completion is open, where following the hint would raise AttributeError outside the transaction. The popup-routing part is weaker than claimed. TDD §7.6 already says "A run start closes the popup (D6)", and the question of whether the popup's Enter sub-handler also catches modified Enter chords is a v1.1 question shared by Ctrl+Enter and Ctrl+Alt+Enter, not one the patch created.

**Fix applied.** Add one sentence to RUN-26 and TDD §4.5 defining the hint: shown at the caret's statement start until the first Alt+Enter use, hidden while typing or while a popup is open, kept in user state, with a setting to turn it off. Redraw the chip on a complete statement line (for example line 17) with no completion popup. Optionally, state in §4.9.1 that the popup sub-handler takes unmodified Enter only.

#### P1-ux-10

**Minor, rule that erases the history.** Verdict: confirmed.

**Where:** SPEC.md OUT-12; TDD.md §6.4 header rule and Presets; SPEC.md WS-02 (REPL tab)

**Problem.** REPL and console headers no longer show the submitted code, and echo can be hidden by the Echo input toggle or the Minimal preset. With echo hidden, REPL blocks (and the REPL tab) show results without the input that produced them, which defeats the console history.

**Evidence.** OUT-12: "REPL and console run headers no longer repeat the submitted code: the echo lines replace it". TDD §6.4: "Minimal hides `echo` and `info` lines", and the Echo input toggle "hides or shows only echo lines". T-REPL-018 only covers the link target: "with echo hidden the link scrolls to run #N's header".

**Refuter.** OUT-12 and TDD §6.4 remove the code from REPL and console headers ("the echo lines replace it"). The same patch lets echo be hidden: TDD §6.4 Filters says "Minimal hides `echo` and `info` lines", and the Echo input toggle "hides or shows only echo lines". With echo hidden, a REPL block in Output reads "▶ Run #15 REPL … ok" followed by "# Result: 12", with nothing showing what produced it. v1.1 kept the first line plus (+n lines) in the header. The REPL tab's history list partly offsets this inside the REPL tab, but not in Output.

**Fix applied.** Let REPL and console headers fall back to the v1.1 "first line plus (+n lines)" summary whenever that run's echo lines are hidden (toggle off, Minimal, or evicted). Add the case to the OUT-12 tests (T-OUT/T-REPL).

#### P1-ux-11

**Minor, missing piece the brief required (drawing).** Verdict: confirmed.

**Where:** images/01-main-window.png Run #13 and #14; images/02-run-and-cancel.png live run view; SPEC.md §9.5 row 01

**Problem.** No drawing shows output sitting between two statements' echoes, which is the line-by-line history the owner asked for. Every block shows a run of echo lines and then output at the end.

**Evidence.** Mockup 01 Run #13: echo 2, 3, 4, 5, then "12 assets selected". Run #14: echo 8, echo 9-14, then the transaction note and error. Mockup 02 live view: echo 33, echo 36-41 plus "… 3 more lines", then output. Brief: "a run block whose echo lines… are interleaved with output". SPEC OUT-12 acceptance: "echo 1, echo 2, the printed line, echo 3, in that order". SPEC 9.5 row 01 claims echo lines "interleaved with the output".

**Refuter.** In every drawn block the echo lines come first and the output after: Run #13 (echo 2-5, then "12 assets selected"), Run #14 (echo 8, echo 9-14, then the note and the error), and mockup 02's live view (echo 33, echo 36-41, then output). Each drawing would look the same if Sumi echoed the whole range before running, so none of them shows the per-statement interleaving the brief required for mockup 01 ("echo lines … interleaved with output"). SPEC §9.5 row 01 also claims "interleaved with the output". Only the TDD §6.4 text sample shows an output line followed by a later echo (Cube_02, then "20 › len(actors)").

**Fix applied.** Make one block in mockup 01 show output followed by a later echo. For example, let Run #13 print after line 3, or add a print after `renamed = 0` in cell [2], and keep the code view's line numbers in step.

#### P1-ux-12

**Minor, drawing contradicts the text.** Verdict: confirmed.

**Where:** images/01-main-window.png Run #14; TDD.md §6.4 Design: header and footer (Transaction notes)

**Problem.** Mockup 01 puts the transaction note after the echo lines. The TDD renders it from the run record as one muted line directly under the header.

**Evidence.** Mockup 01 HTML order: header #run14, echo 8, echo 9-14, then `<div class="info">asset renames and moves in this run are not undoable · undo history cleared</div>`. TDD §6.4: "Transaction notes (§5.9.2): one muted line directly under the header"; the TDD's sample block also puts nothing between the header and the first echo except that line.

**Refuter.** TDD §6.4: "Transaction notes (§5.9.2): one muted line directly under the header". Mockup 01 now orders Run #14 as header, echo 8, echo 9-14, then `<div class="info">asset renames and moves in this run are not undoable · undo history cleared</div>`. In v1.1 the note sat directly under the header, and the patch put the echo lines above it. Mockup 02 panel (a) keeps the note directly under the header, so the two drawings now disagree. (Mockup 07's note after the output is v1.1 and out of scope.)

**Fix applied.** Move the note directly under Run #14's header, above "8 › renamed = 0".

#### P1-ux-14

**Minor, drawing vs callout.** Verdict: confirmed.

**Where:** images/02-run-and-cancel.png code area behind; mockups/02-run-and-cancel.html #codeBehind, callout 8

**Problem.** The frozen code view behind the Run window highlights line 43 (current-line tint, bold number, "Ln 43, Col 5"), which is the executing line in the Location row. That reads as an execution marker, while callout 8 says the running line is shown in the Location row and not in the code view.

**Evidence.** HTML: `<div class="cur">43</div>` and `<div class="curline">    tools.import_asset_tasks([task])</div>`; status bar "Ln 43, Col 5". Location row: "Line 43 · tools.import_asset_tasks([task]) · batch_reimport.py · statement 7 of 12". Callout 8: "the code area as it was, so the running line shows in the Location row, not in the code view" (crop verify/m02_code.png).

**Refuter.** Mockup 02's frozen code view has `<div class="cur">43</div>`, `<div class="curline">    tools.import_asset_tasks([task])</div>` and status "Ln 43, Col 5". That is the line the Location row names ("Line 43 · tools.import_asset_tasks([task]) …"). The patch's new callout 8 says "the code area as it was, so the running line shows in the Location row, not in the code view", yet the drawing highlights the running line in the code view. v1.1 had the same coincidence (line 31), but the new callout sentence makes it contradict itself.

**Fix applied.** Put the pre-run caret on another line (for example 47 or 27) and update the status bar Ln/Col to match.

#### P1-ux-15

**Minor, visual quality (overlap).** Verdict: confirmed.

**Where:** images/07-snippets-and-shelves.png main window

**Problem.** Now that the output area sits between the code and the shelf, the dashed drag path from the code selection to the shelf crosses the output area. It cuts through Run #41's header timestamp and the "… 6 more" text.

**Evidence.** Crop verify/m07_top.png: the dashed arrow passes over "12:52:10 · 0.18 s" in the Run #41 header and next to "… 6 more" on the line below, before reaching the "+ New shelf button · 11 lines" chip.

**Refuter.** In images/07 (and the crop verify/m07_top.png), the dashed drag path from the selection at about (850,615) to the shelf chip now crosses the output area, which the patch moved between the code and the shelf. It passes through Run #41's header timestamp "12:52:10 · 0.18 s" and touches "… 6 more". In v1.1 the path crossed only the code area.

**Fix applied.** Route the path to the right of the run text (for example x ≥ 1000), or start the drag from the selection's right end.

#### P1-ux-M-02

**Minor, drawing contradicts the text.** Raised by the refuter.

**Where:** images/01-main-window.png code gutter line 15; mockups/01-main-window.html callout 4

**Problem.** Callout 4 now says line 15 was typed after the run, and that the gutter marks lines changed since the last save. Yet line 15 has no change bar, while lines 10-14, changed earlier and also unsaved, do. The tab's dirty dot shows the file was not saved after the typing.

**Evidence.** mockup 01 gutter: `<div class="chg">10</div><div class="chg">11</div><div class="chg">12</div><div class="mod">13</div><div class="mod rt">14</div><div class="cur">15</div>`. Callout 4 (patch text): "line 15 was typed after the run, and an edited line loses its mark … The gutter also marks lines changed since the last save." Etab: `batch_rename.py<span class="dot"></span>`. Crop verify/m01_gutter.png shows no bar on line 15.

**Fix applied.** Give line 15 the `chg` class (added-line bar), or reword the callout so line 15 is not described as new since the last save.

### Round 1 — refuted

| ID | Issue | Why it was refuted |
|---|---|---|
| P1-consistency-06 | The ran-line marks (EDT-18, M2) are written into §4.8.2. | The M1/M2 'TDD sections' column is section-granular. Bullet-level Lands tags govern inside a section the M1 row includes whole. For example, §6.4's 'Run filter (M2)' (TDD 3806), 'Presets (M2)' (3810), the regex box (M2) and 'Copy as script (OUT-10, M2)' (3831) all sit inside '§6', which the M1 row includes, while the M2 row lists no §6 entry. The ran-line marks follow the same convention. Every place that defines them says M2: layer 1 'the ran-line tint (EDT-18, M2)' (1497), the gutter row '(EDT-18, M2, below)' (1509) and the bullet 'Ran-line marks (EDT-18, M2; PO-02)' (1513). The M2 Scope column names them, and T-VIEW-020/021 land M2. No reader would build them in M1. |
| P1-consistency-15 | Each Alt+Enter press is a new run of the buffer, and the next run of a buffer replaces its whole mark set. | This is the ruling's explicit semantics, not a doc defect. The brief's PO-02d says 'Marks belong to the buffer's last run: the next run of that buffer replaces them'. 08 §10 PO-02(d) (line 1772) records 'the next run of the buffer re[places them]', and SPEC EDT-18 (234) and TDD 1516 implement it faithfully. The owner asked for line-by-line execution. That is delivered by RUN-26's caret advance and OUT-12's echo history, which keeps every statement of the walk in order. Interpretation (c) is still met per run: the code view shows the statement that just ran. Accumulating marks across runs would change a top-precedence ruling and needs an owner or architect decision, not a consistency fix. |
| P1-runtime-06 | `case` is a soft keyword, so a body line such as `case.append(x)` or `case = cases[i]` would be treated as a match clause. | PyLexer already separates the keyword from the identifier. TDD §4.3.1 (line 1158) makes a soft keyword a keyword only when it starts a logical line, the physical line ends with ':' at depth 0, and the next token is not '=', '.', ',' or ')'. So `case.append(x)` and `case = cases[i]` lex as identifiers, and T-LEX-005 pins 'Keyword exactly where ast has a match node'. The extent reads PyLexer line facts, and §4.3.3's per-line record holds packed tokens with their kinds, so rule 3's 'first token is case' is decided on a keyword token. A keyword `case` at module level is already invalid Python, so the undefined walk-up only affects code that cannot compile, and the single-line fallback there is harmless. |
| P1-ux-07 | SPEC's only non-colour signal for ran-line marks is a hover tooltip, which is neither keyboard-reachable nor exposed to the accessible widget. | Already handled in the design. TDD §4.8.2 Paint gives each outcome its own pattern ("solid, dashed and dotted"). T-VIEW-020 asserts "each bar with its tooltip and dash pattern", and T-A11Y-004 asserts "ran markers differ in pattern". So a sighted keyboard user gets the outcome without hovering, and colour is not the only signal (A11Y-05). The raised statement is also exposed through the accessible Help "diagnostic at the caret", because OUT-11 adds a runtime Problems item at the raising line, and run outcomes are announced (§4.11 "Run #23 error at line 43"). The SPEC naming only the tooltip is a level-of-detail choice that the TDD completes. It is not a contradiction or a gap that would make anyone build the wrong thing. |
| P1-ux-13 | The cancelled block drawn for the same whole-buffer statement-mode run #22 has no echo lines, while the live view above showed echo and LRS-01 says the output area afterwards shows the same block. | Panel (a) shows the tail of a cancelled whole-buffer run. Statement 7 (the `for` loop, lines 36-44) was running when Stop landed, and statements 8-12 never started. Under OUT-12, the only echo lines in the block are statements 1-7's echoes, all written before the loop's first iteration, so they sit 341 iterations (more than 1,000 output lines) above the tail. The tail therefore correctly has no echo rows. The panel compresses the block (header, the transaction note directly under it per §6.4, the last output lines, the cancel summary) exactly as the v1.1 panel did. That does not contradict LRS-01's hand-off, which is drawn in the main panel and stated in callout 4. |

## Round 2

Scope: the round-1 fix patch only, from an exact before/after diff, in two lenses: runtime and consistency, UX and mockups.

**Patch.** I applied all 31 confirmed round-2 issues to SPEC.md, TDD.md, research/08-decision-log.md (section 10), mockups/ue.css, mockups 01, 02, 04 and 07, and the re-rendered images 01, 02, 03, 04, 07 and 09. README.md needed no change. Nothing was skipped as wrong. I took the refuter fix in every case, and where a fix offered options I chose:
- A new getter, `_sumi.statement_counts`, for the cancel detail.
- Show-hook silencing instead of `catch_warnings` for the per-statement compiles.
- The run-file line for the docked arrow.
- Switching to the Output tab at run start, short runs included.
- Window growth when Notes appear.
- Outlined change bars instead of a new ok hue.
- `True` means hide for every `suppress` keyword.

One departure from a refuter fix (P2-runtime-13): the split partial line stays in the store, the view and the run log, so the run log still matches the view (OUT-08). Only FSumiLogForwarder joins it back into one LogPython line. The 08 section 10 refinement paragraph now records the departures from the brief: the latency wording, the growth direction and note growth, the 640 × 360 hidden-tab window, the scrolled-back fallback, fold-chip clicks, the tab switch at start, and entry points, reloads and task steps running through their own calls.

Backups: scratchpad/owner1/verify2/before-patch/. New probes: verify2/q5_show_swap.py and .out, verify2/q6_case_rule.py and .out.

**Checks after the patch.** - **Generation and self-check:** `python tools/gen.py --write` regenerated Contents, 14.x, Appendix A (A-081 now says 480 Slate units) and Appendix B. U35 was already 'not cited' before this patch. `python tools/check.py` printed `PROBLEMS 0 {}` (842 test rows, 1551 links).
- **Lands-terms check:** the scratch T-GAP-008 checker (verify/lands_terms_check.py) reports no hits.
- **Line endings:** all edited text files are LF (no CR bytes) and UTF-8.
- **Probes** (engine python.exe -I -B, 3.11.8):
  - verify2/q5_show_swap.out: with the swap, 1 SyntaxWarning over the whole and per-statement compiles (2 without it), 1 DeprecationWarning over 3 runs, warnings.filters unchanged, a worker's warning dropped.
  - verify2/q6_case_rule.out: the new `case` rule agrees with ast on all 426 statements in the engine Lib whose first token is `case`.
- **Engine lines opened (read-only):**
  - Lib/warnings.py:471-473 and :488-494
  - unreal_core.py:58
  - SWindow.h:245-260, 498, 513 and 527-528
  - SWindow.cpp:280-292 and 360-368
  - Lib/test/test_descr.py:4901 and Lib/unittest/suite.py:225
- **Stale-term greps (now clean):** "records `index`", "112 ms", "next pump or the one after", "centred as today", "switches no tab" / "never switch tabs", "Geo.GetAbsolutePosition", "480 px", "catch_warnings" and "badge and code echoed".
- **Renders:** re-rendered 01, 02, 03, 04, 07 and 09 and read each PNG. I checked 2x and 1x crops of the 01 gutter and prompt, the 02 panel (e) arrow, panel (a) note and bottom legend, and the 04 gutter. Two problems were found and fixed: 02's bottom legend was clipped (canvas enlarged) and 04's error mark touched '18' (gutter widened).

### Round 2 — Runtime, engine and consistency

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [P2-runtime-01](#p2-runtime-01) | minor | interface gap | confirmed | The fix moved the started and returned counts into C++ and removed `completed` from `report_outcome`, but the cancel detail is still built in Python and needs the returned count. |
| [P2-runtime-02](#p2-runtime-02) | minor | semantic difference | confirmed | The per-statement compiles now run inside `warnings.catch_warnings()`. |
| [P2-runtime-03](#p2-runtime-03) | minor | unworkable rule | confirmed | Two new rules from this patch conflict. |
| [P2-runtime-04](#p2-runtime-04) | minor | acceptance cannot pass / geometry | confirmed | The DPI clause and the new per-dimension minimum conflict. |
| [P2-runtime-05](#p2-runtime-05) | minor | inconsistency | confirmed with changes | H_min, 'the height of the other rows plus 5 lines', is only well defined for rows whose height is fixed. |
| [P2-runtime-06](#p2-runtime-06) | minor | wrong runtime claim | confirmed | The rewritten bound, 'one pump period plus the 5 ms window plus the GIL hand-off', assumes a pending call runs soon after it is posted. |
| [P2-runtime-07](#p2-runtime-07) | minor | test contradicts design | confirmed | The new T-SUP-022 step expects the docked arrow to be 'absent while that line is in the other tab's buffer'. |
| [P2-runtime-08](#p2-runtime-08) | minor | algorithm ambiguity | confirmed with changes | The patch defines a 'clause line' as a statement start whose first token is `case`, and NextStatementLine now skips clause lines. |
| [P2-runtime-09](#p2-runtime-09) | minor | stale text | confirmed | Two points in the top-precedence 08 §10 rows still carry wording that this patch corrected in SPEC and TDD. |
| [P2-runtime-10](#p2-runtime-10) | minor | inconsistency | confirmed | Fix A2 changed SPEC AI-03 to say that an agent run shows its full code in the run header and emits no echo lines. |
| [P2-runtime-11](#p2-runtime-11) | minor | ambiguity / DPI | confirmed | Step 3 now lists 'the placement arguments of step 4' and still ends 'The same settings apply to the Paused window (§8.3.1)'. |
| [P2-runtime-12](#p2-runtime-12) | minor | incomplete fix | confirmed | P1-ux-04 was confirmed for 'every short Alt+Enter run': with Problems active, or REPL active for a buffer route, the run's echo and output are not visible at all. |
| [P2-runtime-13](#p2-runtime-13) | minor | fidelity regression | confirmed | The P1-runtime-09 fix commits an open partial line as a line of its own when the next statement's echo arrives, and 'a later write to that stream starts a new line'. |
| [P2-runtime-M-01](#p2-runtime-m-01) | minor | inconsistency / off by one | added by refuter | Three places say `run_statement` records `index` (0-based) as started, and record `index + 1` as returned. |
| [P2-runtime-M-02](#p2-runtime-m-02) | minor | stale text / inconsistency | added by refuter | This patch made the pre-processor pass clicks on echo fold chips in the Run window's live view (TDD §5.6.2 row, §5.6.4, SPEC LRS-01, T-SUP-038). |

#### P2-runtime-01

**Minor, interface gap.** Verdict: confirmed.

**Where:** TDD.md §5.3.7 run() and _execute (lines 2376-2379); TDD.md §5.3.7 'Where a Stop lands in statement mode' (line 2399); TDD.md §5.3.9 cancelled row (line 2417); TDD.md §12.1.1 report_outcome row (line 8019)

**Problem.** The fix moved the started and returned counts into C++ and removed `completed` from `report_outcome`, but the cancel detail is still built in Python and needs the returned count. `_classify(prep, exc)` in the runner builds the outcome and its detail text, and `report_outcome(run_id, outcome, detail, location)` only passes that text on. The statement-mode detail is 'Stop arrived as the code finished' when returned == n and otherwise 'stopped after statement k of n' with k the returned count. No `_sumi` function returns the counts to Python, and the TDD does not say that C++ rewrites the detail. A between-statement landing has no user frame, so the Python side cannot work out k either.

**Evidence.** TDD 2376: `outcome = _classify(prep, exc)`. TDD 2379: `_report(prep, outcome, threads_before)  # report_outcome(...); the statement counts are already in C++ (run_statement)`. TDD 2399: 'the detail is "Stop arrived as the code finished" when the returned count is n, and otherwise "stopped after statement k of n" with k the returned count'. TDD 8019: '`report_outcome(run_id: int, outcome: int, detail: str, location: tuple | None)` ... detail text ...; in statement mode C++ already holds the started and returned counts'. The §12.1.1 table lists no getter for the counts.

**Refuter.** In TDD §5.3.7, `_classify(prep, exc)` builds the outcome. `report_outcome(run_id, outcome, detail, location)` (§12.1.1, TDD:8019) only passes the detail text on. The patch removed `completed` (diff line 513 → 514). No `_sumi` function returns the started or returned count; I checked every row of §12.1.1 (write, Writer, ..., run_statement, post_report, current_run_id, user_code_begin/done, report_outcome, check_cancel, stop_requested, ...). Python cannot work the count out from the traceback either. A Stop raised by `run_statement(i)`'s own check (returned = i) and a Stop delivered at the CALL's eval-breaker check just after `run_statement(i)` returned (returned = i+1) leave the same runner frame, line and local `index`, because C frames do not appear in tracebacks. So 'stopped after statement k of n' and 'Stop arrived as the code finished' (TDD:2399, §5.3.9 row, §5.14 row at 3187), and T-RUN-034 tier 3 (a)/(b), have no specified data path.

**Fix applied.** As proposed. Either C++ formats the statement-mode no-user-frame detail inside report_outcome (Python passes detail=None or a marker), or add `_sumi.statement_counts(run_id) -> tuple[int, int]` to §12.1.1 for `_classify`. Say which in §5.3.7, §5.3.9 and §12.1.1, and name the path in T-RUN-034 tier 3 (a) and (b).

#### P2-runtime-02

**Minor, semantic difference.** Verdict: confirmed.

**Where:** TDD.md §5.3.2 Statement mode row (line 2301); SPEC.md RUN-27 Prepare ('with compile-time warnings ignored'); TDD.md T-RUN-031 (line 3255)

**Problem.** The per-statement compiles now run inside `warnings.catch_warnings()`. Entering and leaving that context replaces the process-global `warnings.filters` and calls `_filters_mutated()`, which clears every once-per-location warning registry. So in statement mode, a warning that one exec shows once per Session (for example a DeprecationWarning from a deprecated `unreal` API, which PyGenUtil.cpp:1432 raises with PyErr_WarnEx) shows again on every run and on every Alt+Enter press. The context also swaps global state for up to about 100 ms of `Starting` while worker threads that earlier runs left alive (D24) may run. CPython's catch_warnings is not thread-safe: those threads lose SyntaxWarning and DeprecationWarning output for that window, and a concurrent catch_warnings in a worker can restore the wrong filter list. RUN-27 says statement mode behaves as with one exec. T-RUN-031 counts warnings only within one run, so it does not catch this.

**Evidence.** Engine Lib/warnings.py:471-473: `self._filters = self._module.filters` / `self._module.filters = self._filters[:]` / `self._module._filters_mutated()`. Probe verify2/q4_catch_warnings_registry.out: 'v1.1 one exec -> warnings shown over 3 runs: 1' and 'v1.2 prepare with catch_warnings -> warnings shown over 3 runs: 3'. TDD 2301: '...inside `warnings.catch_warnings()` with `SyntaxWarning` and `DeprecationWarning` ignored...'.

**Refuter.** Engine Lib/warnings.py:471-473 (`__enter__`) and the matching `__exit__` lines replace `warnings.filters` and call `_filters_mutated()`, which invalidates every `__warningregistry__`. I re-ran verify2/q4_catch_warnings_registry.py on the engine python.exe -I -B. It gives 1 warning over 3 runs with one exec and 3 with prepare-in-catch_warnings. The unreal deprecation path is real: PyGenUtil.cpp:1432 → PyUtil::SetPythonWarning → PyUtil.cpp:1579 `PyErr_WarnEx(InException, ..., 1)`, attributed to the user frame whose `__name__` is '__main__', so the default 'default::DeprecationWarning:__main__' once-per-location rule applies. My probe verify2/refuter_runtime/r1_probe.out confirms the TDD's premise ('1) SyntaxWarning shown over two compiles: 2') and adds a worse case. When a worker thread's catch_warnings overlaps prepare's, the filters are left as '[ignore DeprecationWarning, ignore SyntaxWarning, ...]' ('2) filters restored to base after interleaved catch_warnings: False'). Both categories then stay ignored for the rest of the session. T-RUN-031 checks only one run.

**Fix applied.** Do not mutate `warnings.filters` around the per-statement compiles. For example, swap `warnings.showwarning` and `warnings._showwarnmsg_impl` for a no-op only for those compiles. Compile-time warnings carry no registry, so nothing is lost. The residual effect is that other threads' warnings during that window (≤ ~100 ms) are dropped; state it. Otherwise, keep catch_warnings and document both the per-run registry reset and the filter race. Add a T-RUN-031 case: a statement that calls a deprecated API (or `warnings.warn(..., DeprecationWarning)`), run 3 times, shows the warning as many times as one exec does (once).

#### P2-runtime-03

**Minor, unworkable rule.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 Hand-off, Not following (line 2654); TDD.md §5.5.3 step 4 Minimum size (line 2620); SPEC.md LRS-01 ('the line at the top of the live view stays at the same place on screen'); TDD.md T-SUP-036 and T-SUP-037 (lines 3303-3304)

**Problem.** Two new rules from this patch conflict. The per-dimension minimum makes the Run window taller than the output area whenever the area is shorter than H_min (T-SUP-036 builds a 150 px case). The not-following hand-off draws the live view's first visible line at the same screen y in the Output view, clamped only at the start of the store. The live view begins below the Title, State, Location and Progress rows, about 141 px below the window top in mockup 02. In a 150 px top-layout area that y is below the Output tab body, which ends roughly 150 px minus the tab row minus the prompt strip from the area top. In the bottom layout the window grows up, and the y falls above the body, in the tab row. So the promised position does not exist, and the rule gives no fallback.

**Evidence.** TDD 2620: 'When the area is shorter than that, the window grows toward the code area: down from the area's top-left in the top layout, up from its bottom-left in the bottom layout.' TDD 2654: 'the view scrolls so that the live view's first visible line is drawn at the screen y where the live view drew it, clamped at the start of the store'. Round-1 measurement of mockup 02 (verify/measure_rw.py): #runWin [528,222,1575,602], #rwLive top 363, so the rows above the live view take 141 px. T-SUP-037 asserts 'within 2 px of the screen y it had' with no area-size condition.

**Refuter.** TDD §5.5.3 step 4 (2620) lets the window be H_min × s tall when the area is shorter, and the splitter allows areas down to 120 px (TDD:3732). T-SUP-036 builds a 150 px area. The not-following hand-off (2654) maps the live view's first visible line to the same screen y in the Output view and clamps only at the start of the store. In mockup 02 the rows above the live view take about 141 px (#runWin top 222, #rwLive top 363). In a 150 px top-layout area that y is below the Output body, which ends at roughly 150 minus the tab row and the prompt strip. In the bottom layout the window grows up and the y lands in the tab row. The rule can be computed, but the text being read is then off-screen, so the promise in SPEC LRS-01 ('the line at the top of the live view stays at the same place on screen') fails. The same holds when the window was centred because the area was not visible: the screen y then means nothing for the hidden view.

**Fix applied.** In §5.5.3 and LRS-01: when the anchor y is outside the Output (or REPL) view's visible rows, or the window was centred because the area was not visible, put the anchor line at the nearest visible row (or at the view's top). Add T-SUP-037 cases with the 150 px area in both layouts and with the tab hidden.

#### P2-runtime-04

**Minor, acceptance cannot pass / geometry.** Verdict: confirmed.

**Where:** SPEC.md LRS-01 acceptance ('At 125% and 150% DPI the window still matches the output area within 4 px'); TDD.md T-SUP-036 (line 3303); TDD.md §5.5.3 step 4 (lines 2619-2620); TDD.md §5.14 row 'narrower than 480 px' (line 3190)

**Problem.** The DPI clause and the new per-dimension minimum conflict. The minimum height is H_min × s, about 280 Slate units, which is 350 px at 125% and 420 px at 150%. The default output area is 40% of the centre column. Unless the centre column is at least about 875 px (125%) or 1,050 px (150%) tall, the default layout gives a window taller than the area. T-SUP-036 states no tab or monitor size and still expects 'within 4 px of the output area's ... at every scale', so on a 1080p agent at 150% correct code fails it. Related slips: step 4 says `.ScreenPosition(Geo.GetAbsolutePosition())`, which is wrong when the window grows up (bottom layout) or is moved into the work area. The §5.14/A-081 condition says '480 px' where step 4 uses 480 Slate units × s.

**Evidence.** TDD 2620: 'height = max(area height, H_min × s) ... H_min ... about 280 at 100% scale'. TDD 3732: 'in a fresh profile the output area takes 40% of the centre column's height'. T-SUP-036: 'The window's screen rect is within 4 px of the output area's, at the top by default and at the bottom after the move, at every scale'. TDD 2619: '`.ScreenPosition(Geo.GetAbsolutePosition())`, `.ClientSize(Size)`'. TDD 3190: 'The output area is narrower than 480 px'.

**Refuter.** TDD 2620 sets height = max(area height, H_min × s) with H_min ≈ 280 Slate units, so the window is 350 px at 125% and 420 px at 150%. The default area is 40% of the centre column (TDD:3732), and T-SUP-036 gives no tab or monitor size. On a 1080p monitor at 150% the default area is about 340–400 px, so correct code gives a taller window and fails 'within 4 px of the output area's ... at every scale'. The SPEC LRS-01 acceptance ('At 125% and 150% DPI the window still matches the output area within 4 px') fails the same way. Step 4 also passes `.ScreenPosition(Geo.GetAbsolutePosition())`, although the same bullet grows the rect up (bottom layout) and moves it into the work area, so the computed rect's top-left must be passed instead. §5.14 (3190) and A-081 say '480 px' where step 4 uses 480 Slate units × s.

**Fix applied.** As proposed. Require the 4 px match only when the area is at least 480·s × H_min·s, or give a tab size for the DPI cases. Pass the clamped rect's top-left as ScreenPosition. Write '480 Slate units' in §5.14 and A-081.

#### P2-runtime-05

**Minor, inconsistency.** Verdict: confirmed with changes.

**Where:** TDD.md §5.5.3 step 4 H_min (line 2620); TDD.md §5.5.3 Contents: Live run view, Notes, Buttons rows (lines 2642, 2644-2645)

**Problem.** H_min, 'the height of the other rows plus 5 lines', is only well defined for rows whose height is fixed. The Notes row now takes no height while it is empty, and Interrupt cleanup appears in the Buttons row only after 10 s in Stopping…. Both can appear mid-run: a swallow report, the cleanup timer, or 'Paused in VS Code…'. A window opened at the minimum height then shows fewer than the 5 live-view lines that the row table promises. The TDD does not say whether H_min includes these rows or whether the window grows when they appear.

**Evidence.** TDD 2620: '**H_min** is the height of the other rows plus 5 lines of the live run view'. TDD 2642: 'It takes the height the other rows leave, at least 5 lines (H_min, step 4; proposed)'. TDD 2644: 'Notes | Swallow report, cleanup timer, last native interval ...; "Paused in VS Code ..." ... The row takes no height while it is empty (proposed)'. TDD 2645: '**Interrupt cleanup** (visible only in `Stopping…` after 10 s, RT-06)'.

**Refuter.** The Notes part holds. TDD 2644 says the Notes row 'takes no height while it is empty', and its content (swallow report, cleanup timer, 'Paused in VS Code…') appears mid-run and can be several lines. A window opened at H_min, which is defined as 'the other rows plus 5 lines' (2620), then gives the live view fewer than the 5 lines that row 2642 promises. The Interrupt cleanup part is weaker: the table puts it in the Buttons row with Stop ('Buttons | **Stop**; **Interrupt cleanup** (visible only in `Stopping…` after 10 s)'), so it adds no row height. Mockup 02's #cleanC panel pairs it with cleanup text, which belongs to Notes.

**Fix applied.** Define H_min with one Notes line counted, or say that the window grows toward the code area, inside the work area, when Notes gains lines at the minimum height. Extend T-SUP-036's 5-line check to a run that shows a Notes line (the cleanup timer, for example) at the minimum height. Drop the Interrupt cleanup clause unless the button gets its own row.

#### P2-runtime-06

**Minor, wrong runtime claim.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 live run view first bullet (line 2649); TDD.md §5.13 'Live run view latency' row (line 3116); SPEC.md LRS-01 ('about 110 ms at the G2(3) floor of 10 Hz')

**Problem.** The rewritten bound, 'one pump period plus the 5 ms window plus the GIL hand-off', assumes a pending call runs soon after it is posted. That does not hold during a sliced sleep, which is exactly the shape of T-SUP-037 (print, then sleep 200 ms). Pumps run only at slice ends, every 50 ms. A wake that finds the previous callback still executing is skipped (at most one call outstanding, cleared on exit). So a line printed just after a pump waits up to two slices plus the pump cost, at any pump rate. At the 15 Hz target and 20 Hz the stated bound (about 79 ms and 62 ms) is exceeded. The 150 ms acceptance still holds in this model. SPEC says about 110 ms where TDD says about 112 ms.

**Evidence.** TDD 3054: sliced_sleep 'sleeps in slices of `min(remaining, 50 ms)` ... Pending calls, pumps and Stop run between slices'. TDD 2459/2468: post only 'if `bPendingOutstanding` is false'; 'The flag is cleared when the callback **exits**'. TDD 2559: 'Target rate 15 Hz. It rises to 20 Hz while output arrived'. Simulation verify2/q3_latency_sim.out (25 × print+sleep 0.2, 50 ms slices, 5 ms ingest, 6 ms pump cost, 2,000 phases): worst write-to-paint 106.0 ms at 50, 66.7 and 100 ms periods; worst case shown 'print at 4043.00, drained by pump at 4143.00'.

**Refuter.** TDD §5.10.3 (3054): 'sleeps in slices of min(remaining, 50 ms) ... Pending calls, pumps and Stop run between slices'. So in T-SUP-037's print-then-sleep shape, pumps are quantised to slice ends. With the 5 ms ingest window, a line printed at a slice end is missed by the pump at that same slice end and waits until the pump after the next watchdog post. I re-read verify2/q3_latency_sim.py: worst write-to-paint is 106 ms at 50, 66.7 and 100 ms periods, against the stated formula 'one pump period plus the 5 ms window plus the GIL hand-off' of about 79 ms at 15 Hz and 62 ms at 20 Hz (TDD 2649, 3116). At 20 Hz the skipped wake (flag cleared on callback exit, TDD 2468) also contributes. The 150 ms acceptance still holds. SPEC says 'about 110 ms', TDD 'about 112 ms'.

**Fix applied.** State the bound as: the first pump that runs at least 5 ms after the write, which is at most one pump period plus the wait for the game thread's next eval-breaker check (up to one 50 ms slice in a sliced sleep; the whole call in a native call) plus the 5 ms window. Keep about 110 ms as the 10 Hz T-SUP-037 figure and use the same number in SPEC LRS-01, TDD §5.5.3 and §5.13.

#### P2-runtime-07

**Minor, test contradicts design.** Verdict: confirmed.

**Where:** TDD.md T-SUP-022 (line 3290); TDD.md §5.4.7 line at the last pump (line 2567); TDD.md §5.5.4 Execution arrow (line 2670)

**Problem.** The new T-SUP-022 step expects the docked arrow to be 'absent while that line is in the other tab's buffer'. By §5.4.7, the line at the last pump is always taken from the innermost frame whose co_filename is the run's filename. While statement 2 runs the function defined in another tab, that line is statement 2's call line in the run's own buffer, so the arrow stays on it and is never absent. §5.5.4's condition 'when that line is in the run's buffer' can therefore never be false. Also, §5.4.7 now says the arrow 'follow[s] the same rule', which gives a range of lines between statements ('Lines 12–14'), but an arrow needs one line.

**Evidence.** TDD 2567: 'giving the innermost frame whose `co_filename` is the run's filename plus the innermost frame if different ... The cancel location (§5.3.9) and the Live (docked) execution arrow (§5.5.4) follow the same rule'. TDD 2670: 'an arrow ... at the line at the last pump (§5.4.7 ...), when that line is in the run's buffer'. T-SUP-022: 'a docked run of a buffer whose statement 2 loops for 3 s calling a function defined in another tab | ... the gutter arrow is on the line at the last pump and moves at each pump, is absent while that line is in the other tab's buffer'.

**Refuter.** TDD §5.4.7 (2567) takes the line at the last pump from 'the innermost frame whose co_filename is the run's filename'. While statement 2 runs a function from another tab, that frame is statement 2's own frame at its call line, which is in the run's buffer. So §5.5.4's 'when that line is in the run's buffer' is always true, and T-SUP-022's 'absent while that line is in the other tab's buffer' never applies, so its setup does not test what it intends. The same §5.4.7 sentence says the arrow 'follow[s] the same rule', which between statements gives a range ('Lines 12–14'), while an arrow needs one line.

**Fix applied.** Choose the arrow's frame. Either (a) the run-file line of §5.4.7, with T-SUP-022 expecting the arrow on statement 2's call line while the callee runs, or (b) the innermost user frame, drawn in whichever open tab holds it and absent otherwise. Say that between statements the arrow sits at the first line of the last started statement.

#### P2-runtime-08

**Minor, algorithm ambiguity.** Verdict: confirmed with changes.

**Where:** TDD.md §4.5 Terms 'clause line' and rule 3 (lines 1336, 1341); TDD.md §4.5 Next statement clause skipping (line 1344); TDD.md §4.3 soft keyword rule (line 1158); TDD.md T-CORE-037/038

**Problem.** The patch defines a 'clause line' as a statement start whose first token is `case`, and NextStatementLine now skips clause lines. `case` is a soft keyword, though. If 'first token is `case`' is read as text, an identifier named `case` counts as a clause. With the caret on `case(1)` inside `for case in ...:`, rule 3 runs the whole enclosing `for`, and a body walk skips such lines as clauses. If it is read as PyLexer's SoftKeyword kind, a one-line `case X: f()` is not a keyword, because §4.3 requires the physical line to end with `:`. The TDD does not say which reading applies, and no test uses `case` as an identifier.

**Evidence.** TDD 1336: 'A **clause line** is a statement start whose first token is `elif`, `else`, `except`, `finally` or `case`.' TDD 1341: 'if it is `case`, S moves up to the enclosing `match` header (the previous statement start with less indentation)'. TDD 1158: 'Soft keywords are keywords when all hold: they start a logical line; the physical line ends with `:` at depth 0; the next token is not `=`, `.`, `,` or `)`.' Probe verify2/q2_case_soft_kw.out on the engine Lib: 6 statements whose first token is the identifier `case`, e.g. unittest/suite.py:225 `case.doModuleCleanups()` and test/test_descr.py:4902 `case(1)` inside `for case in OverrideNew, OverrideInit, OverrideBoth:`.

**Refuter.** TDD §4.5 (1336) defines a clause line by 'first token is ... `case`', and §4.5 reads only PyLexer line facts (§4.3.3: IndentCols, BracketDelta, flags), which carry no keyword kind. Under the text reading, `case(1)` inside `for case in ...:` (test_descr.py:4902) or `case.doModuleCleanups()` (unittest/suite.py:225) triggers rule 3. That moves S to 'the previous statement start with less indentation', which is not checked to be a `match`, so the whole enclosing `for` runs, and NextStatementLine skips those lines. Under the §4.3 soft-keyword reading (1158: 'the physical line ends with `:` at depth 0'), both a one-line `case X: f()` and a case header whose pattern spans lines fail the test. verify2/q2's 'one-line' hits test_patma.py:2599 and 2623 are in fact multi-line headers (`case ((a, ...) |` continuing to `...)):`). Such a caret then runs the `case` block alone (a SyntaxError), and Alt+Enter lands on it. The patcher's simulation (verify/patcher/q2_extent_sim.py `is_case`) used a colon-checked rule and only top-level carets, so it never covered these.

**Fix applied.** Define `case` (and `match`) clause lines by the logical line. The previous statement start with less indentation is a `match` header. The first token `case` is not followed by `=`, `.`, `,`, `)` or `(`-call shapes at depth 0 that end the statement. A `:` at depth 0 ends the pattern on the logical line's last physical line (one-line bodies included). Add T-CORE-037/038 cases: `case` as an identifier in a loop body, a one-line `case X: f()`, and a multi-line case pattern.

#### P2-runtime-09

**Minor, stale text.** Verdict: confirmed.

**Where:** research/08-decision-log.md §10 PO-02 row (line 1772); research/08-decision-log.md §10 PO-03 row (line 1773)

**Problem.** Two points in the top-precedence 08 §10 rows still carry wording that this patch corrected in SPEC and TDD. (1) The P1-runtime-15 fix reworded RUN-27 because entry points, reloads and task steps are not an `exec`, but the 08 PO-02 row still says they 'keep one `exec`'. (2) The 08 PO-03 row still says the window for a hidden tab is 'centred as today', while SPEC LRS-01 and TDD §5.5.3 now give it a size, 640 × 360 (proposed). The new 'Refined when v1.2 was verified' paragraph mentions neither change.

**Evidence.** 08 line 1772: 'Run File, Isolated runs, shelf and snippet items, entry points, reload routes and task steps keep one `exec`.' SPEC RUN-27: 'do not use statement mode (one `exec`, or for entry points, reloads and task steps their own calls, TDD §5.3.7)'. 08 line 1773: 'with the Sumi tab not visible, the window is centred as today'. SPEC LRS-01: 'If the Sumi tab is not visible, the window is 640 × 360 and centred on the active window'. TDD 2621: '640 × 360 Slate units (proposed)'.

**Refuter.** 08 §10 PO-02 (line 1772, edited by this patch per diff lines 16/18) still says 'Run File, Isolated runs, shelf and snippet items, entry points, reload routes and task steps keep one `exec`'. SPEC RUN-27 now says 'do not use statement mode (one `exec`, or for entry points, reloads and task steps their own calls, TDD §5.3.7)', and TDD §5.2.1 says 'Off (reload and call, §9.4)'. PO-03 (1773) says 'with the Sumi tab not visible, the window is centred as today', while SPEC LRS-01 and TDD 2621 now give 640 × 360 (added in this patch, diff line 229). The refinement paragraph mentions neither change.

**Fix applied.** As proposed. Reword PO-02 to match RUN-27, and give the 640 × 360 size in PO-03 or in the 'Refined when v1.2 was verified' paragraph.

#### P2-runtime-10

**Minor, inconsistency.** Verdict: confirmed.

**Where:** SPEC.md AI-03 (line 531); TDD.md §11 MCP tier 3 bullet (line 7828); TDD.md T-MCP-004 (line 7973); TDD.md §6.4 header fields (line 3778)

**Problem.** Fix A2 changed SPEC AI-03 to say that an agent run shows its full code in the run header and emits no echo lines. The TDD still describes the agent run as 'an echo with the agent badge, the full code and Stop', and T-MCP-004 expects 'badge and code echoed'. 'Echo' now names the OUT-12 line type, which agent routes do not emit. The TDD §6.4 header fields (route badge; source and range; namespace; start time) have no slot for an agent run's full code.

**Evidence.** SPEC AI-03: 'its full code shown in the run header (agent routes are not in statement mode and emit no echo lines, OUT-12)'. TDD 7828: 'one transaction per call; an echo with the agent badge, the full code and Stop'. T-MCP-004: 'Agent namespace; one transaction per call; badge and code echoed'. TDD 3778: 'Header fields: chevron; run number; route badge (... `agent:<name>` ...); source and range; namespace; start time'.

**Refuter.** SPEC AI-03 (531) now reads 'its full code shown in the run header (agent routes are not in statement mode and emit no echo lines, OUT-12)'. TDD §11 tier 3 (7828) still says 'an echo with the agent badge, the full code and Stop', and T-MCP-004 (7973) 'badge and code echoed'. 'Echo' now names the OUT-12 line type, which the MCP route emits none of (§5.2.1: MCP 'Off (one exec)'). §6.4 header fields (3778: chevron; run number; route badge; source and range; namespace; start time) have no slot for the code.

**Fix applied.** As proposed. In §11 tier 3 and T-MCP-004, write 'the full code shown in the run header (no echo lines, OUT-12)', and add the agent-run code field to §6.4: the first line plus '(+n lines)' with an expander, or the code in full.

#### P2-runtime-11

**Minor, ambiguity / DPI.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 step 3 (line 2617); TDD.md §8.3.1 step 7 Window and geometry (line 5658)

**Problem.** Step 3 now lists 'the placement arguments of step 4' and still ends 'The same settings apply to the Paused window (§8.3.1)'. A reader cannot tell whether the Paused window takes the output-area placement, which is wrong because it has its own saved PausedWindowGeometry, or only the DPI-safe flags. §8.3.1 step 7 names neither AutoCenter nor AdjustInitialSizeAndPositionForDPIScale. Each debugger step destroys and reopens the Paused window 'at the same geometry'. With SWindow's defaults, a geometry kept in desktop pixels is multiplied by the DPI scale at every reopen (or centred by AutoCenter), so at 150% the window would grow 1.5× per F10. This is the defect P1-runtime-11 fixed for the Run window.

**Evidence.** TDD 2617: '... `SupportsMaximize(false)`, the placement arguments of step 4, and a `SetRequestDestroyWindowOverride` ... The same settings apply to the Paused window (§8.3.1).' TDD 5658: 'Geometry comes from `USumiUserSettings::PausedWindowGeometry` ... Geometry stays in memory across steps'. TDD §8.3.1 'Each step destroys and reopens the window ... at the same geometry'. Engine SWindow.h:252,255 (`_AutoCenter( EAutoCenter::PreferredWorkArea )`, `_AdjustInitialSizeAndPositionForDPIScale(true)`), SWindow.cpp:284-289 and :365 (position and ClientSize × DPI scale).

**Refuter.** This patch put 'the placement arguments of step 4' into step 3, and step 3 still ends 'The same settings apply to the Paused window (§8.3.1)' (diff 223 → 225). That reads as applying the output-area rect to the Paused window too. §8.3.1 step 7 (5658) names only IsPopupWindow and ActivationPolicy and says 'Geometry stays in memory across steps'. It names neither AutoCenter nor AdjustInitialSizeAndPositionForDPIScale. Engine SWindow.h:252,255 defaults are `_AutoCenter(EAutoCenter::PreferredWorkArea)` and `_AdjustInitialSizeAndPositionForDPIScale(true)`, and SWindow.cpp:284-289 and :365 multiply a non-zero position and the ClientSize by the DPI scale. With those defaults, a geometry read back from the window in pixels and reapplied at each F10 reopen (§8.3.1: 'Each step destroys and reopens the window ... at the same geometry') grows by s each step or gets centred.

**Fix applied.** As proposed. In step 3, limit the shared settings to the popup, activation, focus and close settings. In §8.3.1 step 7, give PausedWindowGeometry's units (desktop pixels) and the `.AutoCenter(EAutoCenter::None)` and `.AdjustInitialSizeAndPositionForDPIScale(false)` arguments. Add a 150% case to T-DBG-024 or the Paused-geometry test: ten F10 steps leave the size and position unchanged.

#### P2-runtime-12

**Minor, incomplete fix.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 Hand-off, Tab (line 2655); TDD.md T-SUP-037 ('the 100 ms run opens no window, switches no tab'); SPEC.md LRS-01 ('the area switches to Output then')

**Problem.** P1-ux-04 was confirmed for 'every short Alt+Enter run': with Problems active, or REPL active for a buffer route, the run's echo and output are not visible at all. Its fix said 'At run start (or at hand-off)'. The patch switches tabs only at hand-off, when a Run window existed, and adds that short runs never switch tabs. The Alt+Enter walk, the owner's line-by-line workflow, is made of short runs, so with the REPL tab or Problems showing, the user still sees no history of the statements just run. SPEC LRS-01 does not mention this exception.

**Evidence.** TDD 2655: 'If the active tab cannot show the run, Problems, or the REPL tab after a run whose route is not REPL ..., the area switches to the Output tab at hand-off ... Short runs, which never open the window, never switch tabs.' round1.json P1-ux-04 problem: 'With REPL or Problems active, a run's echo and output are not visible at all, including every short Alt+Enter run'; fix: 'At run start (or at hand-off), select the Output tab when the active tab cannot show the run'.

**Refuter.** P1-ux-04's problem explicitly covered 'every short Alt+Enter run', and its fix allowed 'At run start (or at hand-off)'. The patcher chose hand-off only (round1.json applied: 'Switch to the Output tab at hand-off'). TDD 2655 now adds 'Short runs, which never open the window, never switch tabs', and T-SUP-037 asserts 'the 100 ms run opens no window, switches no tab'. So with the REPL tab or Problems showing, an Alt+Enter walk, which is all short runs and is the owner's line-by-line workflow, still shows no history. The choice was within the fix's options, but SPEC LRS-01 ('the area switches to Output then') and OUT-12 do not state the short-run exception. LRS-01's 'their echo and output appear together ... on the next painted frame' is false in those tab states.

**Fix applied.** Switch to Output when a run that the active tab cannot show starts, at least for line, selection and RunLineAdvance runs, and update T-SUP-037's 100 ms case. Otherwise, state the exception in LRS-01 and OUT-12 and add a cue such as an unread badge on the Output tab.

#### P2-runtime-13

**Minor, fidelity regression.** Verdict: confirmed.

**Where:** TDD.md §6.1 'Partial lines and input echo' (line 3445); TDD.md §6.3 fidelity rule 1 (line 3686); SPEC.md OUT-12 ('the Output Log stays the echo of output, D19')

**Problem.** The P1-runtime-09 fix commits an open partial line as a line of its own when the next statement's echo arrives, and 'a later write to that stream starts a new line'. Committed lines are what FSumiLogForwarder sends to LogPython and what goes to the run log. So `print('Working...', end='')` followed by `print(' done')` in the next statement becomes two Output Log lines in statement mode. Stock UE and Sumi's own one-exec routes give one line, 'Working... done', because the engine's stdout is a line-buffered TextIOWrapper. Output Log content now depends on statement boundaries. That breaks §6.3 rule 1 for pipelines that parse Saved/Logs, and it also splits the line in the Output view when Echo input is hidden.

**Evidence.** TDD 3445: 'every open game-thread partial line of that run (stdout and stderr) is committed as a line of its own, flagged `Partial` (unterminated) ...; a later write to that stream starts a new line (proposed)'. TDD 3686: '1. Python output keeps category `LogPython` and the stock verbosity mapping, one log line per output line.' Engine unreal_core.py: `_sys.stdout = _io.TextIOWrapper(_Logger('utf-8', log, log_flush), encoding='utf-8', line_buffering=True, ...)`. TDD §6.2 table: SumiOutput '... splits lines ... echoes through FSumiLogForwarder, sends run-log appends'.

**Refuter.** TDD §6.1 (3445) commits an open game-thread partial line 'as a line of its own' before the next echo, and 'a later write to that stream starts a new line'. Committed lines are what the SumiOutput thread forwards through FSumiLogForwarder (§6.2 table at 3518; §6.3 rule 1 at 3686: 'one log line per output line') and what goes to the run log. The common top-level script shape `print('Loading...', end='')` / `assets = load()` / `print(' done')` therefore gives two LogPython lines in statement mode. Stock UE gives one: unreal_core.py:58 `TextIOWrapper(_Logger(...), line_buffering=True, ...)`, and _Logger.write (lines 41-43) logs each flushed buffer, which holds 'Loading... done\n'. Sumi's one-exec routes also give one line, from the per-key partial buffer. With Echo input hidden, the Output view also shows the split.

**Fix applied.** Make the break view-only: the view shows the partial text above the next echo, while the store, the forwarder and the run log keep joining the stream until its newline, as with one exec. Otherwise, state the LogPython and run-log effect in OUT-12 and §6.3. Extend T-OUT-041 or T-OUT-044 to check the Output Log line for the `end=''` case.

#### P2-runtime-M-01

**Minor, inconsistency / off by one.** Raised by the refuter.

**Where:** TDD.md §5.3.7 Statement mode bullet step (2) (line 2398); TDD.md §6.2 Design: input echo, The call (line 3571); TDD.md §12.1.1 run_statement row (line 8014); TDD.md §5.4.7 'statement k of n' (line 2569); TDD.md T-RUN-034 (c) and T-SUP-037

**Problem.** Three places say `run_statement` records `index` (0-based) as started, and record `index + 1` as returned. §5.4.7 then shows 'statement k of n' with k = 'the started count that `_sumi.run_statement` records', and §4.8.2 compares the started and returned counts. Read literally, the Location row shows 'statement 1 of 3' while statement 2 runs. A statement that raised has started == returned, so §4.8.2 would treat it as returned and give no raised or stopped mark. The tests assume a count (started = index + 1).

**Evidence.** TDD 2398: '(2) records `index` as started in the run record; ... (5) records `index + 1` as returned'. TDD 3571: '`run_statement` also stores `index` as started in the run record'. TDD 8014: 'records `index` as started; ... records `index + 1` as returned'. TDD 2569: 'k is the started count that `_sumi.run_statement` records'. T-RUN-034 (c): 'with started 2 and returned 1' for statement 2 (index 1). T-SUP-037: the Location row names 'statement 2 of 3' during statement 2.

**Fix applied.** In §5.3.7, §6.2 and §12.1.1, write 'sets the started count to `index + 1`' (and 'the returned count to `index + 1`'), so both fields are counts, as §4.8.2, §5.4.7 and the tests use them.

#### P2-runtime-M-02

**Minor, stale text / inconsistency.** Raised by the refuter.

**Where:** research/08-decision-log.md §10 PO-03 row (line 1773); TDD.md §5.6.4 Stop targets and the live run view bullet (lines 2719-2720); SPEC.md LRS-01 ('as docked mode passes the output view')

**Problem.** This patch made the pre-processor pass clicks on echo fold chips in the Run window's live view (TDD §5.6.2 row, §5.6.4, SPEC LRS-01, T-SUP-038). The top-precedence 08 PO-03 row, also edited in this patch, still says every other click outside the Stop targets stays swallowed and lists only wheel, scrollbar and the new-lines chip. TDD §5.6.4 justifies the pass with 'as docked mode's output view does', but its own docked Stop targets restrict the output view to 'wheel and scrollbar only'. The docked mouse-down rule is 'Pass only on a Stop target'. So in docked mode the new-lines chip and fold chips are swallowed, and the comparison is false. SPEC LRS-09 says the docked pre-processor passes 'the output view' with no restriction.

**Evidence.** 08 line 1773: '`FSumiInputPreProcessor` passes mouse-wheel events, scrollbar drags and clicks on the "↓ n new lines" chip aimed at the live run view, as docked mode passes the output view; every other key and click outside the Stop targets stays swallowed'. TDD 2699: 'Pass if inside our window (Stop, Interrupt cleanup, and the Run window's live run view: its scrollbar, its "↓ n new lines" chip and its echo fold chips, PO-03)'. TDD 2719: 'Stop targets: ... the output view (wheel and scrollbar only)'. TDD 2720: 'takes mouse-wheel events, scrollbar drags, and clicks on its "↓ n new lines" chip and on its echo fold chips ..., as docked mode's output view does'. after-r1 TDD and SPEC contain no 'fold chip' text (grep count 0), so the pass is new in this patch.

**Fix applied.** Add echo fold-chip clicks to the 08 PO-03 row, or to the refinement paragraph. Then pick one docked rule. Either extend the §5.6.4 docked Stop target to 'the output view (wheel, scrollbar, the new-lines chip and echo fold chips)', which matches LRS-09, or drop 'as docked mode's output view does' from §5.6.4 and SPEC LRS-01.

### Round 2 — UX and mockups

| ID | Severity | Kind | Verdict | Issue |
|---|---|---|---|---|
| [P2-ux-01](#p2-ux-01) | minor | drawing/text mismatch (fix half landed) | confirmed with changes | P1-ux-06 complained that 'neither colour nor pattern tells an ok mark from an added-line bar'. |
| [P2-ux-02](#p2-ux-02) | minor | drawing/text mismatch | confirmed with changes | The redrawn chip invites Alt+Enter on `print(f"renamed {rename_count}")`. |
| [P2-ux-03](#p2-ux-03) | minor | drawing/text mismatch | confirmed with changes | P1-ux-12 moved the note under Run #14's header, but it is still drawn as an ordinary output line (`.info`, the --text colour). |
| [P2-ux-04](#p2-ux-04) | minor | stale text | confirmed | The fix patch rewrote legend 4 but kept 'so a printed line shows within about 100 ms'. |
| [P2-ux-05](#p2-ux-05) | minor | contradiction | confirmed | The fix for P1-ux-08 makes the pre-processor pass clicks on echo fold chips in the Run window's live view. |
| [P2-ux-06](#p2-ux-06) | minor | incomplete fix / owner ask | confirmed | P1-ux-04's problem explicitly included 'every short Alt+Enter run': with the REPL tab (route-filtered to REPL) or Problems active, a buffer run's echo and output are not visible at all. |
| [P2-ux-07](#p2-ux-07) | minor | contradiction | confirmed | The live view is promised 'at least 5 lines', and H_min is 'the height of the other rows plus 5 lines', about 280 px. |
| [P2-ux-08](#p2-ux-08) | minor | unworkable rule | confirmed with changes | The fix added two rules that conflict. |
| [P2-ux-09](#p2-ux-09) | minor | wording/contradiction | confirmed | SPEC and 08 say 'A smaller output area gets a window as wide as the area and the minimum height'. |
| [P2-ux-10](#p2-ux-10) | minor | ambiguous API / test | confirmed | Fix A4 added an `echo` keyword to `sumi_editor.suppress`, which is 'modelled on Maya's scriptEditorInfo'. |
| [P2-ux-11](#p2-ux-11) | minor | drawing/text mismatch (pre existing, now load bearing) | confirmed with changes | The TDD lists the Run window rows as Title, State, Location, Progress, Live run view, Undo meter, Notes, Buttons, Footer. |
| [P2-ux-M-01](#p2-ux-m-01) | minor | test gap (incomplete fix) | added by refuter | The P1-ux-08 fix added Expand Echo and Fold Echo (context menu and palette) and Enter on a chip line, so that folding never needs the mouse (A11Y-01). |
| [P2-ux-M-02](#p2-ux-m-02) | minor | drawing/text mismatch (pre existing, in a legend this patch rewrote) | added by refuter | Legend 4, rewritten in this patch, still begins 'Each # %% header shows when it last ran'. |

#### P2-ux-01

**Minor, drawing/text mismatch (fix half landed).** Verdict: confirmed with changes.

**Where:** docs/mockups/ue.css lines 104 and 247-250; docs/images/01-main-window.png code gutter lines 2-15; docs/mockups/01-main-window.html legend 4 (line 220); docs/images/02-run-and-cancel.png panel (e); TDD.md §4.8.2 lane table (line 1510) and Paint (line 1517)

**Problem.** P1-ux-06 complained that 'neither colour nor pattern tells an ok mark from an added-line bar'. The fix only moved ok marks into a lane 3 px left of the change bars. Both are still solid 3 px green bars (#4caf50 and #2e7d32), so at 1x line 8's ok mark and the added bars on lines 10-12 and 15 look alike, and lines 10-12 (dashed red plus solid green) still read as 'raised and ok'. The refuter's 'change bars as the §4.6 wedges' part was not done. The new exited mark (dash-dot, --info #4fc3f7) will sit beside the blue modified bars (#1565c0) in the same way. Legend 4 lists only three patterns and never mentions the new exited dash-dot. Mockup 02 panel (e) draws the docked execution arrow left of the line numbers (breakpoint lane), although TDD §5.5.4 and §4.8.2 put it in the run lane between the numbers and the change bars.

**Evidence.** ue.css:104 `.gutter .chg { box-shadow: inset -3px 0 0 #2e7d32; }`; ue.css:247 `.gutter .rk-ok { background: linear-gradient(var(--ok), var(--ok)) right 6px top 0 / 3px 100% no-repeat; }`, --ok #4caf50 (ue.css:17); ue.css:250 rk-exit uses var(--info). The 1x crop verify2/c01_gut1x.png shows a red dashed bar and a green solid bar side by side on lines 10-12. TDD 1510: 'Change bars | Added, modified, deleted wedge'. Legend 4: 'a solid green bar for ok, a dashed red bar ..., a dotted amber bar for one that was stopped'. 02 html:111 `.dock .gutter .runmark::before { left: 6px; }`; crop verify2/c02_pe.png shows '▸ 43' with the arrow left of the number.

**Refuter.** The lane part of P1-ux-06 landed. Ok marks now sit in their own lane (ue.css:247 `right 6px`) and ok lines carry the tint (`.lines .rantint`), so 'lines 10-12 still read as raised and ok' is weaker than stated: those lines have no tint. The core round-1 complaint still holds, though. Colour and pattern do not tell an ok mark from an added-line bar. Both are solid 3 px green bars (#4caf50 at ue.css:17 against #2e7d32 at ue.css:104), and only a 3 px horizontal offset separates them; I checked this in verify2/c01_gut1x.png and c01_gutter.png. The marks also crowd the digits. `.gutter div` has padding-right 10px (ue.css:98) and the lane spans 6-9 px from the right, so the bar sits 1 px from the number: '18' touches its dashed bar in verify2/c04_gutter.png. I drop the 'wedge' demand. TDD §4.6 itself calls change bars 'bars' (TDD:1392 'Edited lines get provisional bars'), and the lane table's 'Added, modified, deleted wedge' (TDD:1510) reads as bars plus a deleted-line wedge, so solid added/modified bars match the TDD. Legend 4 (01 html:220) lists three patterns and omits the exited dash-dot. No exited mark is drawn, so this is a small omission. The panel (e) arrow is real: 02 html:111 `.dock .gutter .runmark::before { left: 6px; }`, while the numbers are right-aligned. TDD:2670 puts the arrow 'in the gutter's run lane (§4.8.2)'. It is not from this patch: after-r1/mockups/02-run-and-cancel.html:111 already had it, so round 1 missed it. Panel (e) also draws a blue `.running` left bar on line 43's text (02 html:406), which TDD §5.5.4 never describes.

**Fix applied.** Make ok marks and change bars differ in more than position. Either give Sumi.Gutter.RanOk a hue that is not green (or draw change bars thinner or hollow), and add one sentence to TDD §4.8.2 Paint: the ran-mark tokens must differ in hue from the change-bar colours. Widen the gap between the run lane and the digits (for example padding-right 13px, or the lane at right 4px with the change bar at right 0). Add 'a dash-dot blue bar for a statement that ended the run through SystemExit' to legend 4. In 02 panel (e), draw the arrow right of the line numbers in the run lane, and drop the `.running` bar or describe it in §5.5.4. Optionally delete the unused `.lines .ran` rule (ue.css:109). Re-render 01, 02 and 04.

#### P2-ux-02

**Minor, drawing/text mismatch.** Verdict: confirmed with changes.

**Where:** docs/images/01-main-window.png line 17 and the REPL prompt strip; docs/mockups/01-main-window.html lines 112 and 144; TDD.md §4.9.4 Hint chip (line 1717); SPEC.md RUN-26

**Problem.** The redrawn chip invites Alt+Enter on `print(f"renamed {rename_count}")`. That line carries callout 6's squiggle, and `rename_count` is defined nowhere: the variable is `renamed`, and the Variables panel has no rename_count. So following the hint raises NameError, the same class of problem P1-ux-09 raised for the old line. The drawing also contradicts the new chip rule: the chip shows only in 'a focused buffer tab' and 'not ... within 1 s of the last keystroke'. Yet the REPL prompt strip holds freshly typed text `[a.asset_name for a in sel_assets][:3]` with a solid caret, identical to the buffer caret. TDD layer 4 says carets are 'hollow when unfocused', so the image shows two focused inputs, one of them mid-typing.

**Evidence.** 01 html:144 `<div class="curline"><span class="cursor" id="cur1"></span>...<span class="sqw" id="squig">rename_count</span>...<span class="hintchip" id="hintChip">`; 01 html:112 prompt `[a.asset_name ... ][:3]<span class="cursor"></span>`. Both use `.cursor` (ue.css:119, solid #fff). TDD 1717: 'a focused buffer tab draws a chip ... not while a popup is open or within 1 s of the last keystroke'. Crops verify2/c01_l17.png and c01_prompt.png.

**Refuter.** The focus part holds. TDD:1500 layer 4 says carets are 'hollow when unfocused'. The prompt strip is an SCodeView in prompt mode (SPEC REPL-01, T-VIEW-017), so the rule applies to it. Mockup 01 draws both the prompt caret (01 html:112) and the buffer caret (html:144) with the solid `.cursor` (ue.css:119 background #fff), which makes two focused widgets. The chip rule needs 'a focused buffer tab' (TDD:1717), so the drawing does not show which state it depicts. The solid prompt caret beside another caret is a convention in every mockup (each of 01-10 draws one), so it predates this patch; the chip rule only made it matter in 01. I drop the NameError part as a defect. The chip rule sets no condition on whether the line would run cleanly. Line 17 was the round-1 refuter's own suggestion ('for example line 17'). The squiggle on `rename_count` deliberately demonstrates an inspection catching a bug before it runs. Moving the chip is optional polish. I also drop the 'within 1 s of the last keystroke' part: nothing shows that the prompt text was just typed, and the 1 s rule concerns the buffer.

**Fix applied.** In mockup 01, draw the prompt caret hollow (add a `.cursor.hollow` rule: transparent fill with a 1px #fff outline), or remove it, so the buffer is plainly the one focused widget. Optionally put the chip on a line without a squiggle. Re-render 01.

#### P2-ux-03

**Minor, drawing/text mismatch.** Verdict: confirmed with changes.

**Where:** docs/mockups/01-main-window.html line 96; docs/mockups/02-run-and-cancel.html line 312; docs/mockups/07-snippets-and-shelves.html lines 278-280; TDD.md §6.4 Transaction notes (line 3779)

**Problem.** P1-ux-12 moved the note under Run #14's header, but it is still drawn as an ordinary output line (`.info`, the --text colour). That is the same style as the printed '12 assets selected' two lines above, so directly under the header it reads as the run's first printed output. The TDD says 'one muted line'. Mockup 02 panel (a) draws its note the same way. Mockup 07, which this patch re-rendered, still puts the note after the output ('Renamed 9 assets ...' then the note). It is now the only drawing that contradicts 'directly under the header'. The round-1 refuter scoped 07 out as v1.1, but with 01 and 02 fixed it is an outlier.

**Evidence.** 01 html:96 `<div class="info">asset renames and moves in this run are not undoable · undo history cleared</div>`; ue.css:133 `.out .info { color: var(--text); }`. The same class is used for '12 assets selected'. TDD 3779: 'Transaction notes (§5.9.2): one muted line directly under the header'. 07 html:278-280: header, `<div class="ok">Renamed 9 assets ...</div>`, then the note.

**Refuter.** P1-ux-12 landed exactly as its fix asked: 01 html:96 now has the note directly under Run #14's header. The style mismatch is real but dates from v1.1. before/mockups/01-main-window.html:139 and before/mockups/02-run-and-cancel.html:279 already drew the note as `.info`, and `.out .info { color: var(--text); }` (ue.css:133) is the same style as printed output ('12 assets selected'). TDD:3779 says 'one muted line directly under the header'. Mockup 02 already uses a muted inline style for its footer lines (`style="color:var(--text-lo)"`, 02 html:316-317), so the drawings have a muted style available. Mockup 07's order (07 html:278-280: header, output, note) is v1.1, and round 1 scoped it out. It is cheap to align, but it was not made stale by this patch.

**Fix applied.** Add a muted note class in ue.css (for example `.out .txnote { color: var(--text-lo); font-style: italic; }`) and use it for the transaction notes in 01 (html:96), 02 (html:312) and 07 (html:280). Optionally move 07's note directly under Run #42's header. Re-render 01, 02 and 07.

#### P2-ux-04

**Minor, stale text.** Verdict: confirmed.

**Where:** docs/mockups/02-run-and-cancel.html legend 4 (line 437); docs/images/02-run-and-cancel.png legend 4

**Problem.** The fix patch rewrote legend 4 but kept 'so a printed line shows within about 100 ms'. P1-runtime-10 changed the latency rule everywhere else: painted by the next pump or the one after, about 110-112 ms at the 10 Hz floor, at most 150 ms, and lines written before the first pump appear when the window opens. The legend now promises a tighter bound than SPEC and TDD, and omits the before-first-pump case.

**Evidence.** 02 html:437 '<b>Live run view.</b> The run's echo and output, drained on every pump, so a printed line shows within about 100 ms.' SPEC LRS-01: 'a written line is painted by the next pump or the one after, about 110 ms at the G2(3) floor of 10 Hz; lines written before the window opens appear when it opens'. TDD 2649: 'about 112 ms at the G2(3) floor of 10 Hz'. A grep for 'about 100 ms' across SPEC, TDD, README, 08 and the mockups finds only this line in that sense.

**Refuter.** 02 html:437 still says 'drained on every pump, so a printed line shows within about 100 ms'. In the same patch, 08's PO-03 row had this exact claim reworded: the 08 diff replaces '(at the G2(3) ≥ 10 Hz repaint, a written line appears within about 100 ms)' with 'painted by the next pump or the one after, about 110 ms at the G2(3) floor of 10 Hz'. SPEC LRS-01 got the same rewording, plus 'lines written before the window opens appear when it opens' (SPEC:276). TDD:2649 says 'about 112 ms', and its §5.13 row says 'lines written before the first pump appear when it opens'. The legend was left behind, and a line printed before the window opens (about 300 ms in) is not shown 'within about 100 ms'.

**Fix applied.** Reword legend 4: 'drained on every pump, so a printed line shows by the next pump or the one after (about 110 ms at 10 Hz); lines printed before the window opens appear when it opens.' Re-render 02.

#### P2-ux-05

**Minor, contradiction.** Verdict: confirmed.

**Where:** research/08-decision-log.md §10 PO-03 row (line 1773); SPEC.md LRS-01; TDD.md §5.5.3, §5.6.2, §5.6.4, T-SUP-038

**Problem.** The fix for P1-ux-08 makes the pre-processor pass clicks on echo fold chips in the Run window's live view. SPEC LRS-01, TDD §5.5.3, §5.6.2 and §5.6.4 and T-SUP-038 all say so. 08 §10's PO-03 row was rewritten in this same patch, but still lists only wheel, scrollbar and new-lines-chip input, then says every other click stays swallowed. 08 §10 ranks first in the doc precedence, so the highest-ranking text forbids the fold-chip click that the fix adds and T-SUP-038 tests. The refinement paragraph does not mention the change either.

**Evidence.** 08:1773: '`FSumiInputPreProcessor` passes mouse-wheel events, scrollbar drags and clicks on the "↓ n new lines" chip aimed at the live run view, as docked mode passes the output view; every other key and click outside the Stop targets stays swallowed.' SPEC LRS-01: 'except Stop targets and the mouse-wheel events, scrollbar drags, "↓ n new lines" clicks and echo fold-chip clicks whose target is the live run view'. T-SUP-038: 'clicks an echo fold chip ... the fold chip expands that statement's echo in place'.

**Refuter.** This patch rewrote 08's PO-03 row (the 08 diff touches the same row), but the pass list still reads: 'passes mouse-wheel events, scrollbar drags and clicks on the "↓ n new lines" chip aimed at the live run view ...; every other key and click outside the Stop targets stays swallowed' (08:1773). The same patch added fold-chip clicks to SPEC LRS-01 ('"↓ n new lines" clicks and echo fold-chip clicks whose target is the live run view'), to TDD §5.5.3 (TDD:2652), §5.6.2 (TDD:2699) and §5.6.4 (TDD:2720), and to T-SUP-038 ('clicks an echo fold chip ... the fold chip expands that statement's echo in place'). SPEC:12 says §10 wins over everything else, so the top-precedence text forbids a click that the SPEC requires and a test checks. The 'Refined when v1.2 was verified' paragraph (08:1775) does not mention it.

**Fix applied.** In 08 PO-03, change the list to 'mouse-wheel events, scrollbar drags, clicks on the "↓ n new lines" chip and clicks on echo fold chips aimed at the live run view'. Add a clause to the refinement paragraph: 'echo fold chips in the live run view expand on a click'.

#### P2-ux-06

**Minor, incomplete fix / owner ask.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 Hand-off, Tab bullet (line 2655); TDD.md T-SUP-037 (line 3304); SPEC.md LRS-01; SPEC.md WS-02

**Problem.** P1-ux-04's problem explicitly included 'every short Alt+Enter run': with the REPL tab (route-filtered to REPL) or Problems active, a buffer run's echo and output are not visible at all. The fix switches to Output only at the Run-window hand-off, and adds 'Short runs, which never open the window, never switch tabs'. So the typical Alt+Enter walk, mostly runs under 300 ms, still shows nothing in the output area while REPL or Problems is showing. The tab behaviour now depends on run duration: a 0.2 s statement leaves the REPL tab up, a 0.4 s one flips to Output. Both are normal states for the line-by-line workflow. WS-02 keeps the prompt strip on the REPL tab, and F8 walks Problems. The narrowing is not listed among the skipped items.

**Evidence.** TDD 2655: 'the area switches to the Output tab at hand-off ... (proposed). Short runs, which never open the window, never switch tabs.' T-SUP-037: 'the 100 ms run opens no window, switches no tab'. TDD 3866: 'The REPL tab is a filtered view ... with the route filter fixed at `Repl`'. WS-02: the prompt strip 'shows for the Output and REPL tabs and hides for Problems'. Round-1 P1-ux-04 problem: 'a run's echo and output are not visible at all, including every short Alt+Enter run'.

**Refuter.** This was not a mis-landing. The P1-ux-04 fix offered 'At run start (or at hand-off)', and the round-1 refuter called short-run non-switching v1.1 behaviour. But the chosen option creates a new inconsistency. TDD:2655 says 'the area switches to the Output tab at hand-off ... Short runs, which never open the window, never switch tabs', and T-SUP-037 expects the 100 ms run to switch no tab. So with the REPL or Problems tab active, a 0.2 s Alt+Enter run leaves the tab alone and its output unseen, while a 0.4 s run flips to Output. The REPL tab cannot show buffer runs (TDD:3829 'Route filter: fixed at route = `Repl` for the REPL tab's view'). Line-by-line runs are mostly short, so the owner's main workflow gets the unswitched case. The patcher's applied/skipped lists do not record the narrowing.

**Fix applied.** Make the rule independent of duration. When any run whose route the active tab cannot show ends (Problems, or REPL for a non-REPL route), select Output: at hand-off for windowed runs, and when the run returns for short ones. Or state a deliberate exception for Problems and keep it for both short and long runs. Update TDD §5.5.3 Tab bullet, SPEC LRS-01 and T-SUP-037: the 100 ms case run with REPL active ends on Output.

#### P2-ux-07

**Minor, contradiction.** Verdict: confirmed.

**Where:** TDD.md §5.5.3 step 4 Minimum size (line 2620); TDD.md §5.5.3 Contents rows Live run view and Notes (lines 2642, 2644); docs/images/02-run-and-cancel.png panel (c)

**Problem.** The live view is promised 'at least 5 lines', and H_min is 'the height of the other rows plus 5 lines', about 280 px. But the Notes row 'takes no height while it is empty', so H_min is sized with Notes empty. Nothing resizes the window when a note appears, and notes are common: the native-call note (LRS-03), the swallow report, the cleanup timer, the 'Paused in VS Code' line. Mockup 02 panel (c) draws one note with Interrupt cleanup at about 80 px. At the minimum height (any output area under about 280 px) such a note takes the 5-line reserve, about 93 px after roughly 187 px of rows, and leaves the live view with about one line or none. That is the failure P1-consistency-M-01 and P1-ux-03 were meant to remove.

**Evidence.** TDD 2620: '**H_min** is the height of the other rows plus 5 lines of the live run view, about 280 at 100% scale'. TDD 2642: 'It takes the height the other rows leave, at least 5 lines (H_min, step 4; proposed)'. TDD 2644: 'The row takes no height while it is empty (proposed)'. Round-1 measurement (P1-ux-03 refuter): rows take 187 px. Panel (c) note box with 'Interrupt cleanup' (verify2 crop of 02, about y 1190-1262 at display scale).

**Refuter.** TDD:2620: '**H_min** is the height of the other rows plus 5 lines'. TDD:2642: the live view gets 'at least 5 lines (H_min ...)'. TDD:2644: Notes 'takes no height while it is empty (proposed)'. So H_min is effectively sized with Notes empty, and nothing grows the window when a note appears. Notes are common. LRS-03's native-call note shows after any pending-call gap over 1 s, and cleanup timers and the 'Paused in VS Code' line also go there. Mockup 02 panel (c) draws the cleanup note with its Interrupt cleanup button (02 html:341; `.cleanup` padding 7+7, two lines, plus a button with 6 px margin) at about 70-80 px, close to the whole 5-line reserve of about 85-93 px. The skip reason given for P1-runtime-12 ('the 5-line H_min already guarantees the live view room') holds only while Notes is empty. T-SUP-036 checks 5 lines only with no note showing.

**Fix applied.** Define H_min with the Notes row at its largest (two lines, plus Interrupt cleanup if that stays in the row), or say the window grows by the Notes row's height when a note appears (toward the code area, inside the work area). Add a T-SUP-036 case: a minimum-height window, then a native-call note and the cleanup note; the live view still shows at least 5 lines.

#### P2-ux-08

**Minor, unworkable rule.** Verdict: confirmed with changes.

**Where:** TDD.md §5.5.3 Hand-off, Not following (line 2654); TDD.md §5.5.3 step 4 growth toward the code area (line 2620); TDD.md T-SUP-037; SPEC.md LRS-01

**Problem.** The fix added two rules that conflict. The per-dimension minimum makes the window grow past a short output area. The scrolled-back hand-off then requires the live view's first visible line to be drawn 'at the screen y where the live view drew it'. With an output area under about 165 px (WS-02 allows 120 px; T-SUP-036 uses 150 px), the live view starts about 141 px below the window top. In the top layout that is below the Output view's last row (area top + about 24 to area bottom - prompt strip). In the bottom layout, where the window grows up, it lands about at the tab row. Either way the Output view cannot draw that line at that y, so the rule cannot be met and T-SUP-037's 2 px assertion would fail if run in that layout. No fallback is given.

**Evidence.** TDD 2654: 'the view scrolls so that the live view's first visible line is drawn at the screen y where the live view drew it, clamped at the start of the store'. TDD 2620: 'When the area is shorter than that, the window grows toward the code area: down from the area's top-left in the top layout, up from its bottom-left in the bottom layout.' T-SUP-037: 'the live view's first visible line is within 2 px of the screen y it had'. Rows above the live view measure about 141 px in mockup 02 (round-1 refuter measurement #runWin/#rwLive).

**Refuter.** The rule has no fallback, but the test claim is overstated. T-SUP-037 (TDD:3304) runs only in the default layout and has no small-area case, so its 2 px assertion passes as written. The rule itself cannot be met in small areas. Round-1 measurements of mockup 02 give #outArea [528,222,1575,602], the Output body [528,248,1575,572] (tab row 26 px, prompt strip 30 px) and #rwLive starting 141 px below the window top. With a 150 px area in the top layout, the window grows down to H_min, and the live view's first line is at area top + 141, below the Output view's bottom at area top + 120. In the bottom layout, the window grows up by about 130 px, and that line is at area top + 11, above the Output view's top at area top + 26. TDD:2654 ('drawn at the screen y where the live view drew it, clamped at the start of the store') gives no clamp for either case.

**Fix applied.** Add to TDD §5.5.3 Not following: 'When that y lies outside the Output view, the line is drawn at the nearest edge of the view, and follow mode stays off.' Add a T-SUP-037 (or T-SUP-036) case with a 150 px area in each layout: the anchor line is at the nearest edge and follow mode is off.

#### P2-ux-09

**Minor, wording/contradiction.** Verdict: confirmed.

**Where:** SPEC.md LRS-01 (line 276); research/08-decision-log.md §10 PO-03 row (line 1773); TDD.md §5.14 row A-081 (line 3190)

**Problem.** SPEC and 08 say 'A smaller output area gets a window as wide as the area and the minimum height'. Read literally, an area narrower than 480 px gets a window narrower than 480. That contradicts the same sentence's '480 wide' minimum, TDD's `max(area width, 480 × s)` and T-SUP-036's 'the narrow tab gives a window 480 Slate units wide'. The 5.14 outcome 'The Run window extends over the code area by the shortfall' is wrong for a width shortfall, where the window extends sideways past the tab or over a side bar. 08 still says the hidden-tab window is 'centred as today', while SPEC and TDD now give it a fixed 640 × 360.

**Evidence.** SPEC 276: 'Each dimension has its own minimum (proposed): 480 wide, and tall enough ... A smaller output area gets a window as wide as the area and the minimum height, growing toward the code area'. 08:1773: 'a smaller output area gets a window as wide as the area and the minimum height ... with the Sumi tab not visible, the window is centred as today'. TDD 3190: 'Each dimension clamped to its minimum ... | The Run window extends over the code area by the shortfall, or opens centred'.

**Refuter.** SPEC:276 says 'Each dimension has its own minimum (proposed): 480 wide ... A smaller output area gets a window as wide as the area and the minimum height'. Read literally, that sentence gives a 420 px area a 420 px window, against its own 480 minimum, TDD:2620 'Width = max(area width, 480 × s)' and T-SUP-036 'the narrow tab gives a window 480 Slate units wide'. 08:1773 repeats the sentence. In TDD:3190, the A-081 outcome 'extends over the code area by the shortfall' is wrong for a width shortfall, where the window extends right past the area. 08:1773 still says the hidden-tab window is 'centred as today', with no size, while SPEC and TDD give 640 × 360; the refinement paragraph does not mention it.

**Fix applied.** In SPEC LRS-01 and 08 PO-03, change 'A smaller output area' to 'A shorter output area', and add 'an area narrower than 480 gets a 480-wide window from its left edge'. Change A-081's outcome to 'extends past the area by the shortfall (over the code area for height, to the right for width), or opens centred'. In 08, write 'the window is 640 × 360, centred (proposed)'.

#### P2-ux-10

**Minor, ambiguous API / test.** Verdict: confirmed.

**Where:** TDD.md §12.2 suppress (lines 8090-8094); TDD.md T-OUT-043 (line 4091); TDD.md OI-41 (line 8950)

**Problem.** Fix A4 added an `echo` keyword to `sumi_editor.suppress`, which is 'modelled on Maya's scriptEditorInfo'. There, `suppressResults=True` hides results, so `suppress(results=True)` reads as 'hide results'. The new docstring says 'echo is the Echo input toggle', and T-OUT-043 treats `suppress(echo=False)` as turning echo off and `suppress(echo=True)` as turning it back on. That is the opposite polarity to the other keywords. A reader cannot tell whether `suppress(echo=True)` hides or shows echo, so T-OUT-043 can be implemented either way and fail. OI-41 still lists the keyword set to freeze as '(results, info, warnings, errors)', without echo.

**Evidence.** TDD 8090-8094: `def suppress(*, results ..., errors ..., echo: bool | None = None)` with '"""Hide or show output kinds, like Maya's scriptEditorInfo; echo is the Echo input toggle (OUT-12).'. TDD 4091 T-OUT-043: '`sumi_editor.suppress(echo=False)` and later `suppress(echo=True)` ... turning either off hides only echo lines'. TDD 8950: 'OI-41 | Freeze the keyword set of `sumi_editor.suppress` (results, info, warnings, errors)'.

**Refuter.** This patch's A4 added `echo` to `suppress` (TDD:8090-8094). The docstring says 'Hide or show output kinds, like Maya's scriptEditorInfo; echo is the Echo input toggle (OUT-12)'. The other keywords follow scriptEditorInfo's -suppressResults and similar flags, and TDD:7732 says 'the CE2 `-suppress*` flags', so True means hidden. The 'Echo input toggle' is True when echo is shown (SPEC OUT-12 'on by default, shows or hides'). T-OUT-043 (TDD:4091) runs 'suppress(echo=False) and later suppress(echo=True)' as off, then on, which is the toggle's polarity. So the docstring and the test give `echo` the opposite polarity to the other keywords, and the returned 'previous state' dict mixes the two. OI-41 (TDD:8950) still freezes '(results, info, warnings, errors)'.

**Fix applied.** State once in the docstring that True suppresses (hides) for every keyword, echo included. Rewrite T-OUT-043 as `suppress(echo=True)` (hides), then `suppress(echo=False)` (shows). Add echo to OI-41's keyword list.

#### P2-ux-11

**Minor, drawing/text mismatch (pre existing, now load bearing).** Verdict: confirmed with changes.

**Where:** TDD.md §5.5.3 Contents table (lines 2636-2646); docs/mockups/02-run-and-cancel.html lines 260-290; docs/images/02-run-and-cancel.png Run window

**Problem.** The TDD lists the Run window rows as Title, State, Location, Progress, Live run view, Undo meter, Notes, Buttons, Footer. Mockup 02 draws something else: the Stop button and the 'Esc or Shift+F5' hint in the State row, the Undo record meter beside Progress above the live view, no Buttons row, and a footer without the 'Esc or Shift+F5 stops the run' text. This predates the fix, but the fix's H_min 'about 280 at 100%' only holds for the drawn layout (about 187 px of rows plus 5 lines). With the TDD's own row list, a separate Undo row and a separate Buttons row add roughly 60 px, so a reader building from the table gets a different minimum and a different layout.

**Evidence.** TDD 2643-2646 rows 'Undo meter' and 'Buttons | **Stop**; **Interrupt cleanup**' after 'Live run view'. 02 html:260 `<span class="rw-hint" id="rwHint">Esc or Shift+F5</span><span class="stopbig" id="rwStop">■ Stop</span>` in the state row; html:264-266 rwProg and rwUndo in one row before rwLive (html:269); html:290 footer 'Viewports resume when the run ends. Clicks and keys outside this window are ignored until then.' Crop verify2/c02_rw.png.

**Refuter.** The mismatch is real but dates from v1.1. before/TDD.md:2570-2578 had the same rows (State, Location, Progress, Output tail, Undo meter, Notes, Buttons, Footer), and the v1.1 mockup had the same compact layout (`.rw-state` with Stop, side-by-side meters). 'Load-bearing' is overstated. H_min is defined as a formula ('the height of the other rows plus 5 lines', TDD:2620), so a build from the table gets a correct, larger minimum, and T-SUP-036 ('at the minimum height the live view shows at least 5 lines') passes for either layout. Only the 'about 280' figure is tied to the drawn layout. The reader still faces two layouts. The drawing puts Stop and 'Esc or Shift+F5' in the State row (02 html:260), Progress and Undo record side by side above the live view (html:264-266), and no Buttons row, and its footer (html:290) lacks the 'Esc or Shift+F5 stops the run' text the TDD Footer row lists.

**Fix applied.** In TDD §5.5.3 Contents, follow mockup 02: State holds Stop and the 'Esc or Shift+F5' hint; Progress and the Undo meter share a row above the live view; Notes, with Interrupt cleanup, sits above the footer; the footer holds the two RV-17/U11 lines that are left. Say that 'about 280' is measured for that arrangement. Or keep the table and redraw 02, restating the figure.

#### P2-ux-M-01

**Minor, test gap (incomplete fix).** Raised by the refuter.

**Where:** TDD.md §6.4 Echo folding (line 3835); TDD.md T-OUT-042 (line 4090); SPEC.md OUT-12 (line 310)

**Problem.** The P1-ux-08 fix added Expand Echo and Fold Echo (context menu and palette) and Enter on a chip line, so that folding never needs the mouse (A11Y-01). No test covers any of them. T-OUT-042 still checks only that 'the chip expands in place'. SPEC OUT-12 names Expand Echo but not Fold Echo, so the SPEC has no way to fold a statement back except the chevron on the whole block.

**Evidence.** TDD:3835: '**Expand Echo** and **Fold Echo** (context menu and palette, proposed) act on the statement at the caret or on every statement of the selected runs, and Enter on a chip line expands it, so folding never needs the mouse (A11Y-01).' T-OUT-042 (TDD:4090): 'Normal shows the first 6 lines and "… 34 more lines", and the chip expands in place and stays expanded after scrolling'. A grep for 'Expand Echo' and 'Fold Echo' finds only TDD:3835 and SPEC:310 ('on Enter, or with **Expand Echo** in the context menu and palette').

**Fix applied.** Extend T-OUT-042: Expand Echo from the palette and the context menu on the caret's statement and on two selected runs, then Fold Echo; Enter on a chip line with the keyboard only. Expected: each statement expands and folds as stated, and no mouse event is needed. Add 'and Fold Echo' to SPEC OUT-12.

#### P2-ux-M-02

**Minor, drawing/text mismatch (pre existing, in a legend this patch rewrote).** Raised by the refuter.

**Where:** docs/mockups/01-main-window.html lines 125 and 133 (cell tags); docs/mockups/01-main-window.html legend 4 (line 220); SPEC.md RUN-03 (line 244); TDD.md §4.8.2 layer 5 (line 1501)

**Problem.** Legend 4, rewritten in this patch, still begins 'Each # %% header shows when it last ran'. RUN-03 says headers show 'the last run number, time and duration', and its acceptance uses this mockup's own numbers: cell 2's header shows '#14 · ran hh:mm · 0.4 s'. Yet the drawing gives cell [2], which Run #14 ran at 12:41:07 for 0.42 s, the tag 'Ctrl+Alt+Shift+Enter runs this cell'. Cell [1]'s tag, 'ran 12:40 · 0.03 s', omits #13. The tags date from v1.1 (before/mockups/01 lines 97 and 104).

**Evidence.** 01 html:125 `<span class="cell-tag" id="cellTag">ran 12:40 · 0.03 s</span>`; 01 html:133 `<span class="cell-tag">Ctrl+Alt+Shift+Enter runs this cell</span>`; Output header html:95 'Run #14 batch_rename.py · cell [2] 12:41:07 · 0.42 s · error'. SPEC RUN-03: 'Cell headers show the last run number, time and duration. | M2 | Only cell 2 of three runs; its header shows "#14 · ran hh:mm · 0.4 s".' TDD:1501: 'Cell headers (M2): a hairline and `ran 14:03 · 0.4 s` from run records'.

**Fix applied.** Change cell [1]'s tag to '#13 · ran 12:40 · 0.03 s' and cell [2]'s to '#14 · ran 12:41 · 0.42 s · error'. Move the Ctrl+Alt+Shift+Enter hint into legend 1, where the chord is already listed. Align TDD:1501's example with RUN-03 ('#22 · ran 14:03 · 0.4 s'). Re-render 01.

