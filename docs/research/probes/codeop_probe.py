import codeop, sys
print(sys.version)
cases = ["for i in x:", "x = (1,", "@dec", "def f():\n pass", "def f():\n pass\n", "x=1\ny=2", "lambda:",
         "for i in range(3):\n    print(i)", "for i in range(3):\n    print(i)\n", "if x:\n  pass\nelse:", "x = 1"]
for sym in ("exec", "single"):
    print("== symbol", sym)
    for c in cases:
        try:
            r = codeop.compile_command(c, "<sumi:REPL>", sym)
            res = "incomplete" if r is None else "complete"
        except (SyntaxError, OverflowError, ValueError) as e:
            res = "invalid: %s" % e
        print(repr(c), "->", res)
