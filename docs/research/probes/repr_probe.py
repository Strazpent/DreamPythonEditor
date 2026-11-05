import time
calls = []
class Job:
    def __repr__(self):
        calls.append(1)
        return "Job()"
v = [Job(), {"k": Job()}]
print(type(v) is list, repr(v), "user __repr__ calls:", len(calls))
big = list(range(2_000_000))
t = time.perf_counter(); s = repr(big); dt = (time.perf_counter() - t) * 1000
print("repr of 2M-int list: %.1f ms, %d chars before any cap" % (dt, len(s)))
