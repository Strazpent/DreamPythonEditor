import sys, os, time, json, threading
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, sys.argv[1])
import debugpy
seen = {}
stop = threading.Event()
def pre_existing():
    seen['pre_before'] = repr(sys.gettrace())
    while not stop.is_set():
        time.sleep(0.05)
        seen['pre_after'] = repr(sys.gettrace())[:70]
t = threading.Thread(target=pre_existing, daemon=True); t.start()
time.sleep(0.2)
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.8)
debugpy.trace_this_thread(False)
time.sleep(0.3)
stop.set(); t.join(1)
import pydevd
from _pydevd_bundle import pydevd_constants
seen['py_db_trace_dispatch_is_threading'] = threading.gettrace() is pydevd.get_global_debugger().trace_dispatch
open(os.path.join(here, "dbg_probe2_out.json"), "w").write(json.dumps(seen, indent=1))
os._exit(0)
