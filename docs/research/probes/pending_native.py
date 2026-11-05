# Probe: does Py_AddPendingCall from a NON-Python native thread (no GIL, no tstate)
# interrupt a pure-Python loop on the main thread in CPython 3.11.8?
import ctypes, ctypes.wintypes as wt, sys, time, threading, json
out = {'version': sys.version}
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
WAITORTIMERCALLBACK = ctypes.c_void_p
k32.CreateTimerQueueTimer.argtypes = [ctypes.POINTER(wt.HANDLE), wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.ULONG]
k32.CreateTimerQueueTimer.restype = wt.BOOL
k32.DeleteTimerQueueTimer.argtypes = [wt.HANDLE, wt.HANDLE, wt.HANDLE]
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
fired = {}
def cb(arg):
    fired['t'] = time.perf_counter()
    fired['main'] = threading.get_ident() == main_ident
    return 0
cbp = CB(cb)
main_ident = threading.get_ident()
addr_pending = ctypes.cast(ctypes.pythonapi.Py_AddPendingCall, ctypes.c_void_p).value
addr_cb = ctypes.cast(cbp, ctypes.c_void_p).value
WT_EXECUTEONLYONCE = 0x8

def case(label, loop_body, seconds=2.0, delay_ms=300):
    fired.clear()
    h = wt.HANDLE()
    ok = k32.CreateTimerQueueTimer(ctypes.byref(h), None, addr_pending, addr_cb, delay_ms, 0, WT_EXECUTEONLYONCE)
    start = time.perf_counter()
    while time.perf_counter() - start < seconds:
        loop_body()
    end = time.perf_counter()
    # give the pending call a chance once the GIL is released and re-taken
    time.sleep(0.05)
    after = time.perf_counter()
    k32.DeleteTimerQueueTimer(None, h, None)
    t = fired.get('t')
    out[label] = {
        'timer_created': bool(ok),
        'fired_during_loop_at_s': (round(t - start, 3) if (t is not None and t <= end) else None),
        'fired_after_loop': (t is not None and t > end),
        'never_fired': t is None,
        'ran_on_main': fired.get('main'),
    }

def nothing():
    pass
def sleep0():
    time.sleep(0)   # releases and re-takes the GIL

case('pure_python_loop_native_poster', nothing)
case('loop_with_gil_release_native_poster', sleep0)
print(json.dumps(out, indent=1))
