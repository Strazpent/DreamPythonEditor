# Dream Python Editor: design documents

_The product was called Sumi until owner ruling PO-06 phase 3 renamed it; the research notes under [research/](research/) keep the old name._

Dream Python Editor (Dream for short) is a Python script editor for Unreal Engine 5.8. It combines the run-in-place speed of Maya's Script Editor and the Charcoal Editor 2 plug-in with PyCharm-class code intelligence, inside the Unreal Editor.

## Start here

| Document | What it is |
|---|---|
| [SPEC.md](SPEC.md) | Product requirements, v1.2: problems, users, journeys, functional and non-functional requirements, UX, release plan |
| [TDD.md](TDD.md) | Technical design, v1.2: architecture, threading, every component's design, interfaces, data formats, test plan, delivery plan, risks |
| [images/](images/) | UI and UX mockups (rendered from [mockups/](mockups/)) |

Version 1.1 of both documents applies the 108 accepted findings of the v1.0 review ([research 13](research/13-v1-review.md)) and the rulings RV-01 to RV-24 ([08 §9](research/08-decision-log.md#9-amendments-after-the-v10-review-rv-01--rv-24)). Five verification rounds then checked that work, each round checking the previous round's fixes; every issue they confirmed (153 in all, none open) is applied too ([research 14](research/14-v11-verification.md)).

Version 1.2 of both documents applies the owner rulings PO-01 to PO-03 ([08 §10](research/08-decision-log.md#10-owner-rulings-after-v11-po-01--po-03)): output on top, line-by-line execution with input echo, and the Run window as the live output area. Two verification rounds then checked that patch (51 and 28 issues, none open; [research 15](research/15-v12-verification.md)).

Some parts of TDD.md are generated: the Contents list, the 14.1 area table, the 14.3 gate suites, the 14.7 traceability summary, the 17.3 gap paragraph and Appendices A and B. After editing test tables or SPEC IDs, regenerate them and run the self-checks (numbering, references, test and SPEC IDs, table shape, links, coverage, Lands):

```bash
python docs/tools/gen.py --write
```

```bash
python docs/tools/check.py
```

## Mockups

| Image | Shows |
|---|---|
| [01 Main window](images/01-main-window.png) | The docked editor with the output area on top: run blocks with each statement's echo above its output, the REPL prompt strip, then tabs and the code view with ran-line markers and the Alt+Enter hint; sidebar, status bar; a run's uncaught error as a runtime item in Problems and the gutter |
| [02 Run and cancel](images/02-run-and-cancel.png) | A long run in the Run window, which covers the output area at the top and shows the live run view (echo and streaming output) and the executing line, over the editor's last painted frame (undimmed, still showing Starting), Stop, a cancelled run, task mode |
| [03 Completion and docs](images/03-completion-and-docs.png) | Live and static completion, signature help, quick docs, Unreal lints such as SUM102 on an editor-only property |
| [04 Traceback to source](images/04-traceback-to-source.png) | Clickable tracebacks, error navigation, Problems and when a runtime item clears |
| [05 Debugger paused](images/05-debugger-paused.png) | The Paused window: stack, variables, watches, evaluate, condition editing, and a footer saying the REPL is suspended; the editor behind as it was before the pause |
| [06 Crash recovery](images/06-crash-recovery.png) | Recovered buffers held read-only until you choose, orphans listed apart, conflict handling, local history |
| [07 Snippets and shelves](images/07-snippets-and-shelves.png) | Library tiers (built-in, user, project, studio), parameter forms on runs, shelves and the Dream Shelf tab, the Modified since publish prompt |
| [08 Reload and environment](images/08-reload-and-environment.png) | Deep reload report (parent packages first), environment doctor, effective policy |
| [09 Unreal integration](images/09-unreal-integration.png) | Drag and drop into code, Copy as Python, context menus, status-bar item |
| [10 Settings and trust](images/10-settings-and-trust.png) | Settings layers, policy locks with their keys, trusted folders, shortcuts and the shadowed Unreal shortcuts |

To re-render the images after editing a mockup:

```bash
powershell -ExecutionPolicy Bypass -File docs/mockups/render.ps1
```

## Research and decisions

| # | Document | Contents |
|---|---|---|
| 01 | [Maya, Charcoal Editor 2 and Autodesk tools](research/01-maya-charcoal-and-autodesk.md) | What Maya's Script Editor and CE2 do well and badly |
| 02 | [Other DCC and engine editors](research/02-other-dcc-and-engine-editors.md) | Godot, Gaffer, Nuke KnobScripter, Omniverse, Houdini and others |
| 03 | [IDE features, LSP and DAP](research/03-ide-features-lsp-dap.md) | PyCharm feature set, language servers, stubs, debuggers |
| 04 | [Unreal Python internals](research/04-unreal-python-internals.md) | How UE 5.8 embeds Python |
| 05 | [Architecture and robustness](research/05-architecture-and-robustness.md) | Architecture options, robustness designs, budgets, risks |
| 06 | [User voice and landscape](research/06-user-voice-and-landscape.md) | Pain points, competitors, personas, journeys |
| 07a–07e | [Engine-source verification](research/07a-verify-python-runtime.md) | Claims checked against the local UE 5.8 source: [runtime](research/07a-verify-python-runtime.md), [Slate re-entrancy](research/07b-verify-slate-reentrancy.md), [text widgets](research/07c-verify-text-widgets.md), [stubs and packaging](research/07d-verify-stubs-tests-packaging.md), [editor APIs](research/07e-verify-editor-apis.md) |
| 08 | [Decision log](research/08-decision-log.md) | ADRs D1–D30, gates G1–G7, red-team amendments (§7), UX rulings (§8), review amendments (§9) and owner rulings (§10) |
| 09 | [Red team](research/09-red-team.md) | Objections to the decision log, all resolved in 08 §7 |
| 10 | [Mockup gap review](research/10-mockup-gap-review.md) | Per-page gap reports on the draft mockups (SPEC v0.9), closed by the UX rulings U1–U41 |
| 11 | [Mockup QA pass](research/11-mockup-qa.md) | The cross-mockup QA pass on the draft mockups (SPEC v0.9) |
| 12 | [Mockup v2 change report](research/12-mockup-v2-report.md) | The mockup update to the v1.0 rulings; its open items are ruled in 08 §9 (RV-16 to RV-18) |
| 13 | [Review of v1.0](research/13-v1-review.md) | The five-lens review of SPEC v1.0, TDD v1.0 and the mockups: 108 accepted findings, applied in v1.1 |
| 14 | [Verification of v1.1](research/14-v11-verification.md) | Five verification rounds of v1.1 after the review fixes (57, 54, 22, 11 and 9 issues), all applied |
| 15 | [Verification of v1.2](research/15-v12-verification.md) | Two verification rounds of the owner-rulings patch PO-01–PO-03 (51 and 28 issues), all applied |
| — | [probes/](research/probes/) | The probe scripts that reviews 13 and 14 ran on the engine's `python.exe -I -B` as evidence; those of 14 are in [probes/v11/](research/probes/v11/), and the statement-mode probe for the owner rulings (08 §10) is in [probes/v12/](research/probes/v12/) |

The decision log is authoritative. Where documents disagree, 08 §10 wins, then 08 §9, then 08 §7, then 08 §8, then the ADRs, then the verification files, then the research.
