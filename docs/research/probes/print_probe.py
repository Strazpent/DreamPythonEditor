import sys
class W:
    def __init__(self): self.calls = []
    def write(self, s): self.calls.append(s); return len(s)
    def flush(self): pass
w = W()
line = b"//depot/main/a.py#3 - edit default change\r\n".decode().split("\n")[0]
print(line, file=w)
print(repr(w.calls))
