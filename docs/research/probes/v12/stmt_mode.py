import ast, __future__, sys
src = '''"""lead doc"""
from __future__ import annotations
x: int = 1
"""mid doc"""
def f(a: Undefined) -> int: return a
y = f.__annotations__
'''
tree = ast.parse(src, "<t>")
# whole-module reference
ns1 = {}
exec(compile(tree, "<t>", "exec", dont_inherit=True), ns1)
flags = 0
for s in tree.body:
    if isinstance(s, ast.ImportFrom) and s.module == "__future__":
        for a in s.names: flags |= getattr(__future__, a.name).compiler_flag
ns2 = {}
for i, s in enumerate(tree.body):
    if i > 0 and isinstance(s, ast.Expr) and isinstance(s.value, ast.Constant) and isinstance(s.value.value, str):
        continue
    exec(compile(ast.Module(body=[s], type_ignores=[]), "<t>", "exec", flags=flags, dont_inherit=True), ns2)
# what happens if the mid docstring is compiled alone
ns3 = {}
exec(compile(ast.Module(body=[tree.body[3]], type_ignores=[]), "<t>", "exec", dont_inherit=True), ns3)
print(sys.version.split()[0])
print("whole doc:", ns1["__doc__"], "| stmt doc:", ns2["__doc__"], "| mid alone sets __doc__:", ns3.get("__doc__"))
print("annotations whole:", ns1["y"], "| stmt:", ns2["y"])
print("module __annotations__ whole:", ns1["__annotations__"], "| stmt:", ns2["__annotations__"])
