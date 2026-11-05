import ctypes, sys
f = ctypes.pythonapi.PyErr_GetHandledException
f.restype = ctypes.c_void_p
class E(Exception): pass
e = E()
try:
    raise e
except E:
    before = sys.getrefcount(e)
    for _ in range(10):
        f()   # raw pointer, never decref'd
    after = sys.getrefcount(e)
print(before, after)
