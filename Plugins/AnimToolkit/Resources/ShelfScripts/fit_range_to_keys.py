"""Set the playback range to the first and last key of the selected controls (all controls when none is selected)."""
import animtk.core as core
import animtk.rig as rig
import animtk.seq as seq

sequence = seq.require()
selection = rig.selected_controls()
times = set()
if selection:
    for r, names in rig.by_rig(selection):
        times.update(r.key_times(names))
else:
    for r in rig.rigs(sequence):
        times.update(r.key_times())
if not times:
    raise core.AnimTKError("No keys found")
sequence.set_playback_start(min(times))
sequence.set_playback_end(max(times) + 1)
core.notify("Playback range {} - {}".format(min(times), max(times)))
