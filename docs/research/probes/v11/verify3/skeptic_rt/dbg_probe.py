import sys, os, io, time, json, threading
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, here)
import debugpy
out = {'debugpy': debugpy.__version__}
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
debugpy.trace_this_thread(False)
import pydevd
py_db = pydevd.get_global_debugger()
out['identity_after_listen'] = threading.gettrace() is py_db.trace_dispatch
out['sys_settrace_replaced'] = getattr(sys.settrace, '__name__', repr(sys.settrace))
cap = io.StringIO()
old_err = sys.stderr
sys.stderr = cap
import sumi_backend
old = sumi_backend.install()
seen = {}
def worker():
    seen['worker'] = repr(sys.gettrace())[:60]
t = threading.Thread(target=worker); t.start(); t.join()
import _thread
done = threading.Event()
def raw():
    seen['raw'] = repr(sys.gettrace())[:60]
    done.set()
_thread.start_new_thread(raw, ())
done.wait(2)
sumi_backend.uninstall(old)
sys.stderr = old_err
out['stderr_during_backend'] = cap.getvalue()[:300]
out['threads'] = seen
out['identity_after_restore'] = threading.gettrace() is py_db.trace_dispatch
out['sys_gettrace_end'] = repr(sys.gettrace())
open(os.path.join(here, "dbg_probe_out.json"), "w").write(json.dumps(out, indent=1))
os._exit(0)
