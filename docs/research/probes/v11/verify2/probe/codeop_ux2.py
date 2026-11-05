import codeop
cases = ["else:", "elif x:", "except:", "finally:", "case 1:", "x = 1\nelse:", "if x:\n    pass\nelse:", "try:\n    pass\nexcept:", "async def f():", "x = 1 + \\", "def f(): pass", "class A: pass", "with a:", "match x:", "for i in x: print(i)"]
for c in cases:
    try:
        r = codeop.compile_command(c, "<sumi:REPL>", "exec")
        res = "incomplete" if r is None else "complete"
    except (SyntaxError, OverflowError, ValueError) as e:
        res = "invalid " + type(e).__name__
    print(repr(c), res)
