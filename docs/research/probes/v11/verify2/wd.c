// Probe of the RV-01 watchdog mechanism: a native thread with no thread state posts
// Py_AddPendingCall and then (mode 1) does PyEval_RestoreThread/PyEval_SaveThread on its
// own PyThreadState created on that thread with PyThreadState_New(PyInterpreterState_Main()).
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <windows.h>

#define MAXN 4096
static volatile LONG g_outstanding = 0;
static volatile LONG g_stop = 0;
static LONGLONG g_post[MAXN], g_run[MAXN], g_wait[MAXN];
static volatile LONG g_nposts = 0, g_nruns = 0;
static int g_mode = 0, g_period = 66, g_count = 0;
static PyThreadState* g_ts = NULL;
static unsigned long g_ts_thread_id = 0, g_wd_tid = 0;
static HANDLE g_thread = NULL;
static unsigned long g_cb_tid[MAXN];

static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static int cb(void* arg) {
    LONG i = (LONG)(LONG_PTR)arg;
    if (i >= 0 && i < MAXN) { g_run[i] = qpc(); g_cb_tid[i] = GetCurrentThreadId(); }
    InterlockedIncrement(&g_nruns);
    InterlockedExchange(&g_outstanding, 0);
    return 0;
}

static DWORD WINAPI wd(LPVOID p) {
    g_wd_tid = GetCurrentThreadId();
    for (LONG i = 0; i < g_count && !g_stop; ++i) {
        Sleep(g_period);
        if (InterlockedCompareExchange(&g_outstanding, 1, 0) != 0) { --i; continue; }
        g_post[i] = qpc();
        if (Py_AddPendingCall(cb, (void*)(LONG_PTR)i) != 0) { InterlockedExchange(&g_outstanding, 0); continue; }
        InterlockedIncrement(&g_nposts);
        if (g_mode == 1) {
            if (!g_ts) { g_ts = PyThreadState_New(PyInterpreterState_Main()); g_ts_thread_id = g_ts->thread_id; }
            LONGLONG a = qpc();
            PyEval_RestoreThread(g_ts);
            LONGLONG b = qpc();
            PyEval_SaveThread();
            g_wait[i] = b - a;
        }
    }
    return 0;
}

__declspec(dllexport) int wd_start(int mode, int period_ms, int count) {
    g_mode = mode; g_period = period_ms; g_count = count < MAXN ? count : MAXN;
    g_stop = 0; g_nposts = 0; g_nruns = 0; g_outstanding = 0;
    memset(g_post, 0, sizeof g_post); memset(g_run, 0, sizeof g_run); memset(g_wait, 0, sizeof g_wait);
    g_thread = CreateThread(NULL, 0, wd, NULL, 0, NULL);
    return g_thread != NULL;
}
// called by Python with the GIL released (ctypes.CDLL)
__declspec(dllexport) int wd_join(void) {
    g_stop = 1; WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; return 0;
}
__declspec(dllexport) int wd_get(int i, LONGLONG* post, LONGLONG* run, LONGLONG* wait, unsigned long* cbtid) {
    *post = g_post[i]; *run = g_run[i]; *wait = g_wait[i]; *cbtid = g_cb_tid[i]; return 0;
}
__declspec(dllexport) int wd_counts(LONG* nposts, LONG* nruns) { *nposts = g_nposts; *nruns = g_nruns; return 0; }
__declspec(dllexport) unsigned long wd_ts_thread_id(void) { return g_ts_thread_id; }
__declspec(dllexport) unsigned long wd_tid(void) { return g_wd_tid; }
__declspec(dllexport) LONGLONG wd_freq(void) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }
// GIL held by caller (ctypes.PyDLL): delete the watchdog thread state as HandleShutdown would.
__declspec(dllexport) int wd_delete_ts(void) {
    if (!g_ts) return 0;
    PyThreadState_Clear(g_ts); PyThreadState_Delete(g_ts); g_ts = NULL; return 1;
}
