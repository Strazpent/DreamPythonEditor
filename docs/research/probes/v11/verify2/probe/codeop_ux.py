import codeop, sys
print(sys.version)
cases = ["for i in x:", "x = (1,", "@dec", "def f():\n pass", "def f():\n pass\n",
         "for i in range(3):\n    print(i)", "for i in range(3):\n    print(i)\n",
         "x = [\n    1,\n    2]", "x=1\ny=2", "lambda:", "if x:\n    y = 1\nelse:\n    y = 2", "if x:\n    y = 1\nelse:\n    y = 2\n"]
for c in cases:
    try:
        r = codeop.compile_command(c, "<sumi:REPL>", "exec")
        res = "incomplete" if r is None else "complete"
    except (SyntaxError, OverflowError, ValueError) as e:
        res = "invalid " + type(e).__name__
    print(repr(c), res)
