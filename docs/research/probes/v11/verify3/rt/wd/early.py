import ctypes, os, time, json
here = os.path.dirname(os.path.abspath(__file__))
C = ctypes.CDLL(os.path.join(here, 'wd.dll'))
L = ctypes.c_long
def counts():
    a = L(); b = L(); C.wd_counts(ctypes.byref(a), ctypes.byref(b)); return a.value, b.value
res = {}
C.wd_start(0, 50, 1)              # one post after 50 ms from a native thread, no hand-off
time.sleep(0.3)                   # GIL released; the post lands while the main thread is outside bytecode
def first_python_function():      # stands in for handle_request; eval_begin would be called inside it
    return counts()
res['inside_first_function'] = first_python_function()
C.wd_join()
print(json.dumps(res))
