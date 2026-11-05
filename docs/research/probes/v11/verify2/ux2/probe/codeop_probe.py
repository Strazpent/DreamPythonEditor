import codeop, sys, warnings
warnings.simplefilter("ignore")
print(sys.version)
BS = chr(92)
cases = ['else:', 'elif x:', 'except:', 'finally:', 'case 1:', 'x = 1\nelse:', 'if x:\n    pass\nelse:', 'try:\n    pass\nexcept:',
 'match x:', 'async with x:', 'async for a in b:', 'async def f():', 'x = = 1\nfor i in y:', 'x = (1 2,', '(]', 'x = [1,\n  2 3,', 'x = 1 ' + BS,
 '@dec', 'def f(:', 'class A(:', 'while True:', 'with open(f) as g:', 'for i in x:', 'def f(x)::', 'if x = 1:', 'return 1\nfor i in x:', "'''abc", 'f"{',
 'try:\n    pass\nelse:', 'case _:', 'match x:\n    case 1:', 'x = 1 = 2\nif y:', '1 +\nif y:']
for c in cases:
    try:
        r = codeop.compile_command(c, "<sumi:REPL>", "exec")
        res = 'incomplete' if r is None else 'complete'
    except (SyntaxError, OverflowError, ValueError) as e:
        res = 'invalid: ' + type(e).__name__ + ': ' + str(getattr(e, 'msg', e))
    print(repr(c), '->', res)
