# SPDX-License-Identifier: Apache-2.0
"""Declarative Slate for Python.

Describe the widget tree with plain Python objects, pass handlers as ``on_<event>=`` keywords, and the C++ side
(``unreal.AnimTKPanel``) builds real Slate widgets and calls the handlers back::

    from animtk import ui

    class Hello(ui.Tool):
        panel_id = "hello"
        title = "Hello"

        def build(self):
            return ui.VBox(
                ui.Label("Pick a value", style="title"),
                ui.Slider(id="amount", min=0, max=10, on_changed=self.changed),
                ui.Button("Print", on_clicked=lambda e: print(self.get("amount"))),
            )

        def changed(self, event):
            self.set("readout", "text", "{:.1f}".format(event.value))

    Hello.show()

Events reach the handler as an ``Event`` (``event.value``, ``event.item``, ``event.items``, ``event.shift``, ...);
a handler that takes no argument is called without it. ``docs/UI_REFERENCE.md`` lists every widget, its keys and events.
"""

import inspect
import json

from . import core

unreal = core.unreal


# ------------------------------------------------------------------------------------------------------------- widgets

class Widget(object):
    """One node of the widget tree. ``props`` go to C++ as JSON; ``handlers`` stay in Python."""

    def __init__(self, type_, *children, **props):
        self.type = type_
        self.children = [c for c in _flatten(children) if c is not None]
        self.handlers = {}
        for key in list(props):
            if key.startswith("on_") and callable(props[key]):
                self.handlers[key[3:]] = props.pop(key)
        self.id = props.pop("id", None)
        self.props = props

    def to_spec(self, ids):
        """The JSON-ready dict. ``ids`` hands out ids to widgets that need one (handlers) but have none."""
        if self.id is None and self.handlers:
            self.id = ids.next(self.type)
        spec = {"type": self.type}
        if self.id is not None:
            spec["id"] = self.id
        for key, value in self.props.items():
            if value is not None:
                spec[key] = _jsonable(value)
        if self.children:
            spec["children"] = [child.to_spec(ids) for child in self.children]
        return spec

    def walk(self):
        yield self
        for child in self.children:
            for node in child.walk():
                yield node


class _Ids(object):
    def __init__(self):
        self.count = 0

    def next(self, type_):
        self.count += 1
        return "{}_{}".format(type_, self.count)


def _flatten(items):
    for item in items:
        if isinstance(item, (list, tuple)):
            for sub in _flatten(item):
                yield sub
        else:
            yield item


def _jsonable(value):
    """Turns tuples, unreal colors and vectors into JSON lists; leaves everything else alone."""
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, dict):
        return dict((k, _jsonable(v)) for k, v in value.items())
    if unreal is not None:
        if isinstance(value, unreal.LinearColor):
            return [value.r, value.g, value.b, value.a]
        if isinstance(value, unreal.Color):
            return "#{:02X}{:02X}{:02X}{:02X}".format(value.r, value.g, value.b, value.a)
    return value


# Containers ----------------------------------------------------------------------------------------------------------

def VBox(*children, **props):
    """Stacks children vertically. Child slot keys: stretch, padding, halign, valign."""
    return Widget("vbox", *children, **props)


def HBox(*children, **props):
    """Lays children out left to right."""
    return Widget("hbox", *children, **props)


def Group(*children, **props):
    """A vertical box drawn on a panel background (``background=color`` tints it)."""
    return Widget("panel", *children, **props)


def Scroll(*children, **props):
    """A scrolling vertical (or ``orientation="horizontal"``) list of children."""
    return Widget("scroll", *children, **props)


def Wrap(*children, **props):
    """Flows children left to right and wraps them, like a shelf. ``spacing`` in pixels."""
    return Widget("wrap", *children, **props)


def Grid(*children, **props):
    """Uniform grid with ``columns`` columns."""
    return Widget("grid", *children, **props)


def Splitter(*children, **props):
    """Resizable panes; ``orientation`` "horizontal" (default) or "vertical"; child key ``size`` (relative)."""
    return Widget("splitter", *children, **props)


def Section(title, *children, **props):
    """A collapsible area. ``collapsed=True`` starts it closed. Event: changed (expanded bool)."""
    return Widget("section", *children, text=title, **props)


def Tabs(*children, **props):
    """One child per tab, each with a ``label`` prop. ``value`` is the active index. Event: changed (index)."""
    return Widget("tabs", *children, **props)


def Tab(label, *children, **props):
    """A tab page for Tabs: a VBox with a label."""
    return Widget("vbox", *children, label=label, **props)


# Leaves --------------------------------------------------------------------------------------------------------------

def Label(text="", **props):
    """Text. ``style`` "title" or "small", ``bold``, ``size``, ``color``, ``wrap``, ``code``."""
    return Widget("label", text=text, **props)


def Button(text="", **props):
    """A push button. ``icon`` (style name or .png/.svg file), ``icon_above``, ``color``, ``text_color``, ``flat``.
    Event: clicked (with shift/ctrl/alt)."""
    return Widget("button", text=text, **props)


def Toggle(text="", value=False, **props):
    """A button that stays pressed. Event: changed (bool)."""
    return Widget("toggle", text=text, value=value, **props)


def CheckBox(text="", value=False, **props):
    """Event: changed (bool)."""
    return Widget("checkbox", text=text, value=value, **props)


def TextField(value="", hint="", **props):
    """Single line text. Events: changed (every edit), committed (Enter or focus loss)."""
    return Widget("text", value=value, hint=hint, **props)


def TextArea(value="", hint="", **props):
    """Multi-line text; ``code=True`` uses a monospace font. Event: changed."""
    return Widget("multiline", value=value, hint=hint, **props)


def Slider(value=0.0, min=0.0, max=1.0, **props):  # noqa: A002 - mirrors the widget's keys
    """Events: begin (press), changed (while dragging), end (release)."""
    return Widget("slider", value=value, min=min, max=max, **props)


def Spin(value=0.0, min=None, max=None, **props):  # noqa: A002
    """Number box with drag-to-change. ``integer=True``, ``step``. Events: changed, committed."""
    return Widget("spin", value=value, min=min, max=max, **props)


def Combo(options, value=None, **props):
    """Drop-down of strings. Event: changed (value, index)."""
    options = [str(o) for o in options]
    if value is None and options:
        value = options[0]
    return Widget("combo", options=options, value=value, **props)


def ListView(items=(), multi=True, **props):
    """Rows of ``items`` (strings or dicts with id, label, icon, color, tooltip).
    Events: selection (items), double_click (item), menu (action, item, items) with ``menu=[...]``."""
    return Widget("list", items=list(items), multi=multi, **props)


def TileView(items=(), tile_size=96, multi=False, **props):
    """Thumbnails: items take an ``icon`` image file. Same events as ListView."""
    return Widget("tiles", items=list(items), tile_size=tile_size, multi=multi, **props)


def Image(icon, size=32, **props):
    return Widget("image", icon=icon, size=size, **props)


def Separator(**props):
    return Widget("separator", **props)


def Spacer(size=8, **props):
    return Widget("spacer", size=size, **props)


def Progress(value=0.0, **props):
    return Widget("progress", value=value, **props)


def Canvas(items=(), canvas_size=(400, 600), **props):
    """The picker surface. Items: dicts with id, x, y (centre), w, h, color, label, shape ("rect"/"circle").
    Keys: selected, edit, background (image file), snap, menu.
    Events: click (item, shift, ctrl, alt), marquee (items, shift, ctrl), double_click (item), moved (items with x/y),
    menu (action, item, items), key (key)."""
    return Widget("canvas", items=list(items), canvas_size=list(canvas_size), **props)


# Menus ---------------------------------------------------------------------------------------------------------------

def menu_item(label, action=None, checked=None, icon=None, tooltip=None):
    entry = {"label": label, "action": action or label}
    if checked is not None:
        entry["checked"] = bool(checked)
    if icon:
        entry["icon"] = icon
    if tooltip:
        entry["tooltip"] = tooltip
    return entry


def menu_separator():
    return {"separator": True}


def menu_header(text):
    return {"header": text}


def submenu(label, entries):
    return {"label": label, "submenu": list(entries)}


# ------------------------------------------------------------------------------------------------------------- events

class Event(object):
    """What a handler receives. Unknown attributes read as None, so ``event.shift`` works for every event."""

    def __init__(self, widget_id, name, data, panel):
        self.widget_id = widget_id
        self.name = name
        self.data = data or {}
        self.panel = panel

    def __getattr__(self, key):
        if key.startswith("__"):
            raise AttributeError(key)
        return self.data.get(key)

    def __repr__(self):
        return "Event({!r}, {!r}, {!r})".format(self.widget_id, self.name, self.data)


def _call(handler, event):
    try:
        params = [p for p in inspect.signature(handler).parameters.values()
                  if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD, p.VAR_POSITIONAL)]
    except (TypeError, ValueError):
        params = [None]
    if params:
        return handler(event)
    return handler()


# ------------------------------------------------------------------------------------------------------------- panel

class Panel(object):
    """A dockable tab holding one widget tree. Most tools use ``Tool`` instead, which owns a Panel."""

    def __init__(self, panel_id, title):
        if unreal is None or not hasattr(unreal, "AnimTKPanel"):
            raise core.AnimTKError("The AnimToolkit plugin is not loaded (enable it in Edit > Plugins)")
        self.panel_id = panel_id
        self.title = title
        self.native = unreal.AnimTKPanel.find_or_create_panel(panel_id, title)
        # A reloaded tool makes a new Panel for the same native panel: drop the old Python handlers.
        self.native.on_event.clear()
        self.native.on_event.add_callable(self._dispatch)
        self.native.on_closed.clear()
        self.native.on_closed.add_callable(self._closed)
        self.handlers = {}
        self.widgets = {}
        self.closed_callbacks = []
        self.event_filter = None

    # Building ----------------------------------------------------------------------------------------------------
    def set_root(self, root, title=None):
        ids = _Ids()
        spec = root.to_spec(ids)
        self.handlers = {}
        self.widgets = {}
        for node in root.walk():
            if node.id is not None:
                self.widgets[node.id] = node
                for event_name, handler in node.handlers.items():
                    self.handlers[(node.id, event_name)] = handler
        if title:
            self.title = title
        payload = {"title": self.title, "root": spec}
        if not self.native.set_spec(json.dumps(payload)):
            raise core.AnimTKError("Panel {}: the widget spec was rejected (see the Output Log)".format(self.panel_id))

    def register_tab(self, factory_command):
        self.native.register_tab(factory_command)

    # Showing -----------------------------------------------------------------------------------------------------
    def show(self, floating=False):
        self.native.show(floating)

    def close(self):
        self.native.close()

    def is_open(self):
        return self.native.is_open()

    def destroy(self):
        self.native.destroy()

    # Live updates ------------------------------------------------------------------------------------------------
    def set(self, widget_id, prop, value):
        """Changes one property of one widget, for example ``set("status", "text", "Done")``."""
        node = self.widgets.get(widget_id)
        if node is not None:
            node.props[prop] = value
        return self.native.set_prop(widget_id, prop, json.dumps(_jsonable(value)))

    def get(self, widget_id):
        """The widget's value: text, number, bool, selected ids (lists, tiles, canvas)."""
        return json.loads(self.native.get_value(widget_id))

    def on(self, widget_id, event_name, handler):
        self.handlers[(widget_id, event_name)] = handler

    # Dispatch ----------------------------------------------------------------------------------------------------
    def _dispatch(self, widget_id, event_name, payload):
        try:
            data = json.loads(payload) if payload else {}
        except ValueError:
            data = {}
        event = Event(widget_id, event_name, data, self)
        node = self.widgets.get(widget_id)
        if node is not None and "value" in data:
            node.props["value"] = data["value"]
        if self.event_filter is not None and core.safe_call(self.event_filter, event) is True:
            return
        handler = self.handlers.get((widget_id, event_name))
        if handler is not None:
            core.safe_call(_call, handler, event)

    def _closed(self):
        for callback in list(self.closed_callbacks):
            core.safe_call(callback)


# ------------------------------------------------------------------------------------------------------------- tool

class Tool(object):
    """Base class for a panel tool: subclass, set ``panel_id`` and ``title``, implement ``build()``.

    * ``MyTool.show()`` opens it (one instance per class), ``MyTool.register()`` lists it in Window > Tools >
      Anim Toolkit and makes the tab come back with the editor layout.
    * ``tick()`` runs every ``tick_interval`` seconds while the panel is open (``tick_interval = None`` turns it off).
    * ``self.set(...)``, ``self.get(...)`` and ``self.rebuild()`` update the UI.
    """

    panel_id = None
    title = None
    tick_interval = None
    floating = False

    _instance = None

    def __init__(self):
        if not self.panel_id:
            raise core.AnimTKError("{} has no panel_id".format(type(self).__name__))
        self.panel = Panel(self.panel_id, self.title or self.panel_id)
        self.panel.closed_callbacks.append(self._on_panel_closed)
        self._tick_id = None
        self._built = False

    # Class level entry points --------------------------------------------------------------------------------------
    @classmethod
    def instance(cls):
        # Look in the class's own __dict__ so a subclass never reuses its parent's instance.
        inst = cls.__dict__.get("_instance")
        if inst is None:
            inst = cls()
            cls._instance = inst
        return inst

    @classmethod
    def factory_command(cls):
        return "import {0} as _animtk_m; _animtk_m.{1}.show()".format(cls.__module__, cls.__name__)

    @classmethod
    def show(cls, floating=None):
        inst = cls.instance()
        inst.open(cls.floating if floating is None else floating)
        return inst

    @classmethod
    def register(cls):
        """Lists the tool under Window > Tools > Anim Toolkit (the tab is restored with the layout)."""
        if unreal is None or not hasattr(unreal, "AnimTKPanel"):
            return
        native = unreal.AnimTKPanel.find_or_create_panel(cls.panel_id, cls.title or cls.panel_id)
        native.register_tab(cls.factory_command())

    # Instance ------------------------------------------------------------------------------------------------------
    def open(self, floating=False):
        if not self._built:
            self.rebuild()
        self.panel.show(floating)
        if self.tick_interval is not None and self._tick_id is None:
            self._tick_id = core.add_tick(self._tick, self.tick_interval)
        core.safe_call(self.on_open)

    def close(self):
        self.panel.close()

    def rebuild(self):
        self.panel.set_root(self.build(), title=self.title)
        self._built = True

    def set(self, widget_id, prop, value):
        return self.panel.set(widget_id, prop, value)

    def get(self, widget_id):
        return self.panel.get(widget_id)

    # Overridables --------------------------------------------------------------------------------------------------
    def build(self):
        raise NotImplementedError

    def on_open(self):
        pass

    def on_close(self):
        pass

    def tick(self):
        pass

    # Internals -----------------------------------------------------------------------------------------------------
    def _tick(self):
        if not self.panel.is_open():
            return
        self.tick()

    def _on_panel_closed(self):
        if self._tick_id is not None:
            core.remove_tick(self._tick_id)
            self._tick_id = None
        self.on_close()


# ------------------------------------------------------------------------------------------------------------- dialogs

def confirm(title, message):
    """Modal Yes/No; returns True on Yes."""
    result = unreal.EditorDialog.show_message(title, message, unreal.AppMsgType.YES_NO)
    return result == unreal.AppReturnType.YES


def message(title, text):
    unreal.EditorDialog.show_message(title, text, unreal.AppMsgType.OK)


class _PromptTool(Tool):
    panel_id = "animtk_prompt"
    title = "Anim Toolkit"
    floating = True

    def __init__(self):
        super(_PromptTool, self).__init__()
        self.callback = None
        self.label = ""
        self.default = ""

    def build(self):
        return VBox(
            Label(self.label, wrap=True),
            TextField(self.default, id="value", on_committed=self._ok),
            HBox(Spacer(stretch=1), Button("OK", on_clicked=self._ok), Button("Cancel", on_clicked=self.close)),
        )

    def _ok(self, event=None):
        value = self.get("value")
        callback, self.callback = self.callback, None
        self.close()
        if callback is not None and value:
            callback(value)


def prompt(title, label, callback, default=""):
    """Asks for a line of text in a small floating window and calls ``callback(text)`` on OK (non-modal)."""
    tool = _PromptTool.instance()
    tool.title = title
    tool.label = label
    tool.default = default
    tool.callback = callback
    tool.rebuild()
    tool.open(floating=True)
