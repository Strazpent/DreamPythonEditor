import codeop
def cls(src):
    try:
        r = codeop.compile_command(src, "<sumi:REPL>", "exec")
    except (SyntaxError, OverflowError, ValueError):
        return "invalid"
    return "incomplete" if r is None else "complete"
for s in ["if x: pass  # note:", "while x: y = 1  # :", "if x:  # c", "if x: f = lambda:", "if x: y:", "for i in x: pass # a: b:", "class A: x: int  # :"]:
    print(repr(s), cls(s))
