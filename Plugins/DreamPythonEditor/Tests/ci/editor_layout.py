# SPDX-License-Identifier: Apache-2.0
"""(icons) Tier 4 runs on a fixed editor layout (coordinator ruling after the PO-06 bisect; review q1 ruling R-q1-3).

The editor reads and, on exit, rewrites the user's own layout file %LOCALAPPDATA%\\UnrealEngine\\Editor\\EditorLayout.json,
shared by every project. Where the Dream tab opens (docked, or a floating window the user once left it in) changes several
specs' outcomes: Dream.Shell.Chrome's routed clicks and splitter grip, Dream.Visual.Capture's pixel checks and
Dream.CodeView.TabFocus assume the docked placement. So every DreamHost test launch runs on Tests/ci/EditorLayout.tier4.json
(the Dream tab docked in the Level Editor's Details stack) and the user's file is put back byte-identical afterwards.

    python -I -B editor_layout.py run [--show minimized] -- <editor exe> <editor arguments ...>
        put, start the editor with exactly that command line, wait for it, restore (in a finally). The wrapper ignores
        Ctrl+C (the editor gets it and exits; the wrapper restores after it), and on a console close (CTRL_CLOSE, which
        Python sees as SIGBREAK) it waits up to 4 s for the editor and restores if it is gone; otherwise the backup stays
        and the next run restores it first. Exit code: the editor's, or 75 when the user's layout could not be restored.
    python -I -B editor_layout.py put | restore
        the two halves, for a caller that cannot use run (and as a safety net after it: restore without a backup is a
        no-op).

Where things live (review q1 m1): the backup sits next to the user's file, outside any project's Saved folder, as
EditorLayout.json.dream-backup (the user's bytes) or EditorLayout.json.dream-absent (the user had no file). The test layout
carries a marker layout key (MARKER) so that neither half ever mistakes it for the user's file: put never backs up a file
with the marker (a test layout a lost backup left behind), and the absent path only removes a file that has the marker.
put restores a leftover backup first, also one in the old location (<DreamHost>/Saved/Automation/LayoutBackup). restore
checks the copy byte-identical (SHA-256) before it removes the backup; one it cannot verify stays, and a loud warning says
where. Exit codes of put/restore: 0 done, 1 failed, 2 usage.
"""
import hashlib
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_LAYOUT = os.path.join(HERE, "EditorLayout.tier4.json")
MARKER = b'"DreamTier4TestLayout"'
OLD_BACKUP_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "DreamHost", "Saved", "Automation", "LayoutBackup"))
NOT_RESTORED_EXIT = 75


def user_layout():
    base = os.environ.get("LOCALAPPDATA")
    if not base:
        raise RuntimeError("LOCALAPPDATA is not set")
    return os.path.join(base, "UnrealEngine", "Editor", "EditorLayout.json")


def backup_path():
    return user_layout() + ".dream-backup"


def absent_path():
    return user_layout() + ".dream-absent"


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def is_test_layout(path):
    return os.path.isfile(path) and MARKER in read(path)


def copy_verified(source, target):
    """Copies source over target through a temporary file next to it, then checks the bytes. True when identical."""
    temp = target + ".dream-tmp"
    data = read(source)
    try:
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(temp, "wb") as handle:
            handle.write(data)
        os.replace(temp, target)
    finally:
        if os.path.exists(temp):
            os.remove(temp)   # review q1 m2: a failed replace leaves no stray file
    return hashlib.sha256(read(target)).digest() == hashlib.sha256(data).digest()


def warn(text):
    print("editor_layout: WARNING: " + text, flush=True)


def say(text):
    print("editor_layout: " + text, flush=True)


def restore_from(owner, absent, why):
    """Puts the user's layout back from a backup pair; returns 0, or 1 with the backup kept."""
    target = user_layout()
    if os.path.isfile(owner) and os.path.isfile(target):
        say("the file being replaced %s the marker" % ("still has" if is_test_layout(target) else "does NOT have"))
    if os.path.isfile(owner):
        if MARKER in read(owner):
            # Never write a test layout back as the user's: a backup taken of one is worthless.
            warn("the backup %s is a test layout, not the user's; discarding it" % owner)
            os.remove(owner)
        elif not copy_verified(owner, target):
            warn("could not verify the restore of %s; the user's layout stays in %s" % (target, owner))
            return 1
        else:
            os.remove(owner)
            say("restored the user's layout %s byte-identical (%s)" % (target, why))
    if os.path.isfile(absent):
        if is_test_layout(target):
            os.remove(target)
            say("removed the test layout: the user had no %s (%s)" % (target, why))
        elif os.path.isfile(target):
            say("kept %s: it is not the test layout (written by another editor meanwhile)" % target)
        os.remove(absent)
    return 0


def restore(why="this run"):
    status = restore_from(os.path.join(OLD_BACKUP_DIR, "EditorLayout.json.owner"),
                          os.path.join(OLD_BACKUP_DIR, "EditorLayout.json.absent"), why + ", old backup location")
    if os.path.isdir(OLD_BACKUP_DIR) and not os.listdir(OLD_BACKUP_DIR):
        os.rmdir(OLD_BACKUP_DIR)
    status |= restore_from(backup_path(), absent_path(), why)
    if is_test_layout(user_layout()) and not os.path.exists(backup_path()):
        warn("%s is still the test layout and no backup is left; the user's layout was lost earlier" % user_layout())
    return status


def put():
    if os.path.exists(backup_path()) or os.path.exists(absent_path()) or os.path.isdir(OLD_BACKUP_DIR):
        say("an earlier run did not finish; restoring its layout backup first")
        if restore("left by a run that was stopped before it finished") != 0:
            return 1
    if not is_test_layout(TEST_LAYOUT):
        say("the test layout %s is missing or has no %s marker" % (TEST_LAYOUT, MARKER.decode()))
        return 1
    target = user_layout()
    if is_test_layout(target):
        # A test layout without a backup (the backup was deleted by hand): there is no user file to keep.
        warn("%s is a test layout with no backup; treating the user's file as absent" % target)
        open(absent_path(), "w").close()
    elif os.path.isfile(target):
        try:
            ok = copy_verified(target, backup_path())
        except OSError:
            ok = False
        if not ok:
            if os.path.exists(backup_path()):
                os.remove(backup_path())
            say("could not back up %s; the test layout was not put in place" % target)
            return 1
    else:
        open(absent_path(), "w").close()
    if not copy_verified(TEST_LAYOUT, target):
        say("could not write the test layout; restoring the user's layout")
        restore("the test layout could not be written")
        return 1
    say("test layout in place (the user's layout is kept in %s)" % backup_path())
    return 0


def raw_command_after_separator():
    """The editor's command line exactly as the caller wrote it after ' -- ' (Windows: GetCommandLineW, so quoting such as
    -ExecCmds="Automation RunTests X;Quit" reaches the editor unchanged; re-quoting argv would break FParse::Value)."""
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.GetCommandLineW.restype = ctypes.c_wchar_p
        line = ctypes.windll.kernel32.GetCommandLineW()
        at = line.find(" -- ")
        if at >= 0:
            return line[at + 4:].strip()
    index = sys.argv.index("--")
    return subprocess.list2cmdline(sys.argv[index + 1:])


def run(show_minimized):
    command = raw_command_after_separator()
    if not command:
        print(__doc__)
        return 2
    closing = {"flag": False}
    signal.signal(signal.SIGINT, signal.SIG_IGN)   # Ctrl+C: the editor exits on it; the wrapper outlives it and restores
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, lambda *_: closing.__setitem__("flag", True))
    if put() != 0:
        return 1
    code = 1
    child = None
    restored = False
    try:
        startup = None
        if show_minimized and os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 7   # SW_SHOWMINNOACTIVE
        # (rename, owner instruction) Test editors run at BelowNormal priority so the desktop stays responsive;
        # DREAM_EDITOR_PRIORITY=normal starts them at Normal.
        flags = 0
        if os.name == "nt" and os.environ.get("DREAM_EDITOR_PRIORITY", "").lower() != "normal":
            flags = subprocess.BELOW_NORMAL_PRIORITY_CLASS
        child = subprocess.Popen(command, startupinfo=startup, creationflags=flags)
        while child.poll() is None:
            if closing["flag"]:
                try:
                    child.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    warn("the console is closing while the editor runs; the backup stays for the next run to restore")
                    return 1
                break
            time.sleep(0.2)   # a polling wait, so the signal handlers run
        code = child.returncode if child.returncode is not None else 1
    finally:
        if child is None or child.poll() is not None:
            restored = restore() == 0
    if not restored:
        warn("the user's editor layout was NOT restored; it is kept in %s" % backup_path())
        return NOT_RESTORED_EXIT
    return code


def main(argv):
    try:
        if len(argv) >= 2 and argv[1] == "run" and "--" in argv:
            return run(show_minimized=argv[2:4] == ["--show", "minimized"])
        if len(argv) == 2 and argv[1] == "put":
            return put()
        if len(argv) == 2 and argv[1] == "restore":
            return restore()
    except (OSError, RuntimeError) as error:
        warn("%s failed: %s (a backup, if any, is kept in %s)" % (argv[1], error, backup_path()))
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
