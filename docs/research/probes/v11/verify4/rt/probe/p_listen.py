import sys, os, io, time, json, threading
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, sys.argv[1]); sys.path.insert(0, here)
mode = sys.argv[2]
out = {'mode': mode, 'frozen': os.path.realpath.__code__.co_filename}
if mode == 'env':
    os.environ["PYDEVD_DISABLE_FILE_VALIDATION"] = "1"
cap = io.StringIO()
real_err = sys.stderr
sys.stderr = cap
import debugpy
out['import_debugpy_stderr'] = cap.getvalue()
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
debugpy.trace_this_thread(False)
out['listen_stderr'] = cap.getvalue()
import pydevd_tracing
out['warn_attr'] = pydevd_tracing.TracingFunctionHolder._warn
out['settrace_name'] = getattr(sys.settrace, '__name__', None)
cap2 = io.StringIO(); sys.stderr = cap2
import caller_mod
def tr(*a): return None
caller_mod.set_it(tr); caller_mod.set_it(None)
out['warn_true_stderr'] = cap2.getvalue()[:120]
pydevd_tracing.TracingFunctionHolder._warn = False
cap3 = io.StringIO(); sys.stderr = cap3
def other(): caller_mod.set_it(tr); caller_mod.set_it(None)
other()
import bdb
b = bdb.Bdb(); b.set_trace(); b.set_continue()
sys.settrace(None)
out['warn_false_stderr'] = cap3.getvalue()
sys.stderr = real_err
open(os.path.join(here, "p_listen_%s.json" % mode), "w").write(json.dumps(out, indent=1))
os._exit(0)
