import ctypes, sys, time, threading, json, os
here = os.path.dirname(os.path.abspath(__file__))
dllpath = os.path.join(here, 'wd.dll')
C = ctypes.CDLL(dllpath)      # GIL released around calls
P = ctypes.PyDLL(dllpath)     # GIL held around calls
LL = ctypes.c_longlong
freq = C.wd_freq.__call__() if False else None
C.wd_freq.restype = LL
freq = C.wd_freq()
main_tid = threading.get_native_id()
out = {'version': sys.version, 'switchinterval': sys.getswitchinterval(), 'main_tid': main_tid}

def run_case(mode, seconds, period, count):
    C.wd_start(mode, period, count)
    t0 = time.perf_counter()
    t_end = t0 + seconds
    while time.perf_counter() < t_end:
        pass
    t1 = time.perf_counter()
    C.wd_join()          # releases the GIL; pending calls may run after
    time.sleep(0.05)
    np = ctypes.c_long(); nr = ctypes.c_long()
    C.wd_counts(ctypes.byref(np), ctypes.byref(nr))
    rows = []
    for i in range(count):
        p = LL(); r = LL(); w = LL(); tid = ctypes.c_ulong()
        C.wd_get(i, ctypes.byref(p), ctypes.byref(r), ctypes.byref(w), ctypes.byref(tid))
        if p.value == 0:
            continue
        rows.append((p.value / freq, r.value / freq if r.value else None, w.value / freq, tid.value))
    during = [r for r in rows if r[1] is not None and r[1] <= t1]
    lat = sorted((r[1] - r[0]) * 1000 for r in during)
    waits = sorted(r[2] * 1000 for r in rows)
    res = {
        'posts': np.value, 'runs': nr.value, 'ran_during_loop': len(during),
        'first_run_after_loop_start_ms': round((during[0][1] - t0) * 1000, 2) if during else None,
        'latency_ms_p50': round(lat[len(lat)//2], 2) if lat else None,
        'latency_ms_max': round(lat[-1], 2) if lat else None,
        'handoff_wait_ms_max': round(waits[-1], 2) if waits else None,
        'callback_on_main': all(r[3] == main_tid for r in during) if during else None,
    }
    return res

out['no_handoff'] = run_case(0, 2.0, 66, 25)
out['handoff'] = run_case(1, 3.0, 66, 40)
out['wd_ts_thread_id'] = C.wd_ts_thread_id()
out['wd_tid'] = C.wd_tid()
out['ts_id_is_watchdog'] = out['wd_ts_thread_id'] == out['wd_tid']

# PyThreadState_SetAsyncExc return value with a second thread state that has the main thread's id
api = ctypes.pythonapi
api.PyInterpreterState_Main.restype = ctypes.c_void_p
api.PyThreadState_New.argtypes = [ctypes.c_void_p]; api.PyThreadState_New.restype = ctypes.c_void_p
api.PyThreadState_SetAsyncExc.argtypes = [ctypes.c_ulong, ctypes.py_object]; api.PyThreadState_SetAsyncExc.restype = ctypes.c_int
api.PyThreadState_Clear.argtypes = [ctypes.c_void_p]; api.PyThreadState_Delete.argtypes = [ctypes.c_void_p]
ident = threading.get_ident()
r_single = api.PyThreadState_SetAsyncExc(ident, ctypes.py_object())  # NULL clears
dup = api.PyThreadState_New(api.PyInterpreterState_Main())
r_dup = api.PyThreadState_SetAsyncExc(ident, ctypes.py_object())
# Now post a real exception to the id; does the main thread see it?
api.PyThreadState_SetAsyncExc(ident, ctypes.py_object(KeyError))
raised = False
try:
    for _ in range(100000):
        pass
except KeyError:
    raised = True
api.PyThreadState_SetAsyncExc(ident, ctypes.py_object())
api.PyThreadState_Clear(dup); api.PyThreadState_Delete(dup)
out['setasyncexc_return_single'] = r_single
out['setasyncexc_return_with_duplicate_id'] = r_dup
out['main_raised_with_duplicate_present'] = raised
out['deleted_wd_ts'] = P.wd_delete_ts()
print(json.dumps(out, indent=1))
