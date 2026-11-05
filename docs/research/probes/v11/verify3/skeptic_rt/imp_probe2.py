import sys, io, os, json
os.environ["PYDEVD_DISABLE_FILE_VALIDATION"] = "1"
sys.path.insert(0, sys.argv[1])
cap = io.StringIO(); old = sys.stderr; sys.stderr = cap
import debugpy, time
debugpy.listen(("127.0.0.1", 0), in_process_debug_adapter=True)
time.sleep(0.5)
sys.stderr = old
json.dump({"captured": cap.getvalue()[:600]}, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "imp_probe2_out.json"), "w"), indent=1)
os._exit(0)
