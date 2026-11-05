# Control: a native timer thread that TAKES THE GIL (ctypes callback = PyGILState_Ensure/Release)
# and posts Py_AddPendingCall. Does the pending call now fire inside a pure-Python loop?
import ctypes, ctypes.wintypes as wt, sys, time, threading, json
k32 = ctypes.WinDLL('kernel32')
k32.CreateTimerQueueTimer.argtypes = [ctypes.POINTER(wt.HANDLE), wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.ULONG]
k32.DeleteTimerQueueTimer.argtypes = [wt.HANDLE, wt.HANDLE, wt.HANDLE]
CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
TCB = ctypes.WINFUNCTYPE(None, ctypes.c_void_p, ctypes.c_ubyte)
fired = {}
def cb(arg):
    fired['t'] = time.perf_counter(); return 0
cbp = CB(cb)
post = ctypes.pythonapi.Py_AddPendingCall
post.argtypes = [CB, ctypes.c_void_p]; post.restype = ctypes.c_int
def timer_cb(param, fired_flag):        # runs on a pool thread; ctypes takes the GIL around it
    fired['posted'] = time.perf_counter(); post(cbp, None)
tcbp = TCB(timer_cb)
h = wt.HANDLE()
k32.CreateTimerQueueTimer(ctypes.byref(h), None, ctypes.cast(tcbp, ctypes.c_void_p).value, None, 300, 0, 0x8)
start = time.perf_counter()
while time.perf_counter() - start < 2.0:
    pass
end = time.perf_counter()
time.sleep(0.05)
k32.DeleteTimerQueueTimer(None, h, None)
t = fired.get('t')
print(json.dumps({'fired_during_loop_at_s': round(t - start, 3) if t and t <= end else None,
                  'posted_at_s': round(fired['posted'] - start, 3) if 'posted' in fired else None,
                  'switchinterval': sys.getswitchinterval()}))
