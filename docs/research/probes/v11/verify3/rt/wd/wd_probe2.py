import ctypes, sys, time, threading, json, os
here = os.path.dirname(os.path.abspath(__file__))
C = ctypes.CDLL(os.path.join(here, 'wd.dll')); P = ctypes.PyDLL(os.path.join(here, 'wd.dll'))
LL = ctypes.c_longlong; C.wd_freq.restype = LL; freq = C.wd_freq()
main_tid = threading.get_native_id()
def run_case(mode, seconds, period, count):
    C.wd_start(mode, period, count)
    t0 = time.perf_counter(); t_end = t0 + seconds
    while time.perf_counter() < t_end:
        pass
    t1 = time.perf_counter()
    C.wd_join(); time.sleep(0.05)
    rows = []
    for i in range(count):
        p = LL(); r = LL(); w = LL(); tid = ctypes.c_ulong()
        C.wd_get(i, ctypes.byref(p), ctypes.byref(r), ctypes.byref(w), ctypes.byref(tid))
        if p.value: rows.append((p.value/freq, r.value/freq if r.value else None, w.value/freq))
    during = [r for r in rows if r[1] is not None and r[1] <= t1]
    lat = sorted((r[1]-r[0])*1000 for r in during)
    gaps = sorted((during[i+1][1]-during[i][1])*1000 for i in range(len(during)-1))
    return {'ran_during_loop': len(during), 'posts': len(rows),
            'post_to_run_ms_p50': round(lat[len(lat)//2],2) if lat else None, 'post_to_run_ms_max': round(lat[-1],2) if lat else None,
            'gap_between_callbacks_ms_p50': round(gaps[len(gaps)//2],2) if gaps else None, 'gap_ms_max': round(gaps[-1],2) if gaps else None,
            'first_run_after_start_ms': round((during[0][1]-t0)*1000,2) if during else None}
res = {}
res['default_timer_resolution'] = run_case(1, 2.0, 66, 30)
winmm = ctypes.WinDLL('winmm'); winmm.timeBeginPeriod(1)
res['timeBeginPeriod_1'] = run_case(1, 2.0, 66, 30)
winmm.timeEndPeriod(1)
P.wd_delete_ts()
print(json.dumps(res, indent=1))
