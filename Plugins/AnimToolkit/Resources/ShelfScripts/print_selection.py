"""Print the selected controls and their values at the playhead to the Output Log."""
import animtk.core as core
import animtk.rig as rig

for r, names in rig.by_rig(rig.require_selection()):
    core.log("{} ({})".format(r.name, r.asset_path))
    for name in names:
        core.log("  {:<28} {:<16} {}".format(name, r.kind(name), r.get(name)))
