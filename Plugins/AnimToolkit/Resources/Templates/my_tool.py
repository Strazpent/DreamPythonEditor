"""My Tool: a starting point for an Anim Toolkit tool. Edit, save, and press its shelf button again.

Everything a tool needs:
  * subclass ui.Tool, give it a panel_id and a title, return widgets from build()
  * handlers are on_<event>= keywords; they get an event (event.value, event.shift, event.item, ...)
  * use animtk.rig / animtk.seq / animtk.ops / animtk.pose for the animation work
  * raise core.AnimTKError("message") to tell the animator something went wrong (shown as a toast)
"""

import animtk.core as core
import animtk.ops as ops
import animtk.rig as rig
import animtk.seq as seq
import animtk.ui as ui


class MyTool(ui.Tool):
    panel_id = "my_tool"
    title = "My Tool"
    tick_interval = 0.25          # tick() runs this often while the panel is open; None turns it off

    def build(self):
        return ui.VBox(
            ui.Label("My Tool", style="title"),
            ui.Label("", id="info", style="small"),
            ui.HBox(
                ui.Label("Offset"),
                ui.Spin(2, -100, 100, integer=True, id="offset"),
            ),
            ui.Button("Offset Selected Keys", color="#2E86C1", on_clicked=self.offset),
            ui.Button("Key Selected", on_clicked=lambda: ops.key_selected()),
        )

    def tick(self):
        selection = rig.selected_controls()
        frame = seq.current_frame() if seq.current() else "-"
        self.set("info", "text", "Frame {}   |   {} control(s) selected".format(frame, len(selection)))

    def offset(self, event):
        frames = int(self.get("offset"))
        if event.shift:
            frames = -frames          # Shift+click goes the other way
        moved = ops.nudge(frames)
        core.notify("Moved {} key(s) by {} frame(s)".format(moved, frames))


# Running this file (from the shelf) reopens the tool with your latest edits.
if __name__ == "__main__":
    MyTool._instance = None
    MyTool.show()
