import sys, threading
def tracer(frame, event, arg):
    return None
def install():
    old_t = threading.gettrace()
    threading.settrace(tracer)
    sys.settrace(tracer)
    return old_t
def uninstall(old_t):
    sys.settrace(None)
    threading.settrace(old_t)
