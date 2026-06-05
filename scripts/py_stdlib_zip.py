import os
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
skip = {"test", "tkinter", "turtledemo", "idlelib", "lib2to3",
        "ensurepip", "distutils", "pydoc_data", "site-packages"}

with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        if rel.split(os.sep)[0] in skip:
            dirs[:] = []
            continue
        dirs[:] = [d for d in dirs if d not in ("__pycache__", "test", "tests")]
        for f in files:
            if f.endswith(".py"):
                fp = os.path.join(root, f)
                z.write(fp, os.path.relpath(fp, src))
