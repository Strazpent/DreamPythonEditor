import sys, time, json, threading, os
sys.path.insert(0, sys.argv[1])
import debugpy
out = {'before': repr(sys.gettrace())}
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
out['after_listen_same_thread'] = repr(sys.gettrace())
out['connected'] = debugpy.is_client_connected()
def later_call():
    return sys.gettrace()
out['inside_new_call'] = repr(later_call())
def f(n):
    s = 0
    for i in range(n): s += i
    return s
t0 = time.perf_counter(); 
for _ in range(200000): f(1)
out['200k_calls_s'] = round(time.perf_counter() - t0, 3)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"dbg_out.json"),"w").write(json.dumps(out, indent=1))
os._exit(0)
