import codeop
def classify(src, indented):
    if not src.endswith("\n") and indented: return "incomplete"
    try:
        r = codeop.compile_command(src, "<sumi:REPL>", "exec")
    except (SyntaxError, OverflowError, ValueError):
        return "invalid"
    return "incomplete" if r is None else "complete"
cases = [("else:",False),("if x:\n    pass\nelse:",False),("x = (1 2,",False),("for i in x:",False),
 ("match x:",False),("if x: pass  # note:",False),("async with x:",False),("try:",False),("while x: y = 1  # :",False),
 ("with a: b = {1:",False),("class A: x = lambda:",False),("def f(): return x[1:",False)]
for s,i in cases: print(repr(s), classify(s,i))
