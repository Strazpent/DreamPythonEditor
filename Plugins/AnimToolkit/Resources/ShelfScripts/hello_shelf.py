"""A minimal shelf script: shows a toast. Copy it to start your own."""
import animtk.core as core
import animtk.seq as seq

frame = seq.current_frame() if seq.current() else "no sequence open"
core.notify("Hello from the shelf! Frame: {}".format(frame), "success")
