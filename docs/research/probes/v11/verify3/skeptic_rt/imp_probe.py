import sys, io, os, json
sys.path.insert(0, sys.argv[1])
cap = io.StringIO(); old = sys.stderr; sys.stderr = cap
import debugpy
after_import = cap.getvalue()
import time
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
after_listen = cap.getvalue()[len(after_import):]
sys.stderr = old
json.dump({"after_import": after_import, "after_listen": after_listen[:600]}, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "imp_probe_out.json"), "w"), indent=1)
os._exit(0)
