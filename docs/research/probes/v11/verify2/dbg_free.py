import sys, time, json, threading, os
sys.path.insert(0, sys.argv[1])
import debugpy
out = {'debugpy': debugpy.__version__, 'before_sys': repr(sys.gettrace()), 'before_threading': repr(threading.gettrace())}
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
out['after_listen_sys'] = repr(sys.gettrace())[:80]
out['after_listen_threading'] = repr(threading.gettrace())[:80]
debugpy.trace_this_thread(False)
out['after_trace_off_sys'] = repr(sys.gettrace())
out['after_trace_off_threading'] = repr(threading.gettrace())[:80]
seen = {}
def worker():
    seen['worker_sys_gettrace'] = repr(sys.gettrace())[:80]
t = threading.Thread(target=worker); t.start(); t.join()
out.update(seen)
out['connected'] = debugpy.is_client_connected()
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "dbg_free_out.json"), "w").write(json.dumps(out, indent=1))
os._exit(0)
