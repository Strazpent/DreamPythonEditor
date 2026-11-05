"""Key every animatable control of the selected rig on the current frame."""
import animtk.core as core
import animtk.rig as rig

r = rig.require_rig()
names = r.controls()
r.key(names)
core.notify("Keyed {} controls on {}".format(len(names), r.name))
