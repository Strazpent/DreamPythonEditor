# SPDX-License-Identifier: Apache-2.0
# Golden data for T-CORE-039 (TDD §4.5): build.bat runs this on the engine's python.exe -I -B with the Lib root and an
# output path. For every top-level statement of every Lib/**/*.py that compiles on the engine's interpreter it writes
# "file<TAB>caret<TAB>first<TAB>last" (0-based lines; first is the first decorator or lineno, last is end_lineno), for
# carets on the first line, each decorator line and the def/class line of a decorated definition, and each line of a
# simple statement. Extent_AgainstAstGolden reads the file named by DREAM_EXTENT_GOLDEN.
import ast
import os
import sys

root = sys.argv[1]
out = open(sys.argv[2], "w", encoding="utf-8", newline="\n")
files = 0
rows = 0
compound = (ast.If, ast.For, ast.AsyncFor, ast.While, ast.Try, ast.With, ast.AsyncWith, ast.FunctionDef,
            ast.AsyncFunctionDef, ast.ClassDef, ast.Match)
if hasattr(ast, "TryStar"):
    compound = compound + (ast.TryStar,)
for dirpath, dirnames, filenames in os.walk(root):
    dirnames.sort()
    for name in sorted(filenames):
        if not name.endswith(".py"):
            continue
        path = os.path.join(dirpath, name)
        try:
            with open(path, "rb") as f:
                data = f.read()
            tree = ast.parse(data, path)
            compile(data, path, "exec")
        except Exception:
            continue
        files += 1
        lines = []
        for node in tree.body:
            first = node.lineno
            decos = getattr(node, "decorator_list", [])
            if decos:
                first = min(d.lineno for d in decos)
            last = node.end_lineno
            carets = {first, node.lineno}
            for d in decos:
                carets.add(d.lineno)
            if not isinstance(node, compound):
                carets.update(range(node.lineno, node.end_lineno + 1))
            for c in sorted(carets):
                lines.append("%s\t%d\t%d\t%d\n" % (path, c - 1, first - 1, last - 1))
        out.writelines(lines)
        rows += len(lines)
out.close()
print("files", files, "rows", rows)
