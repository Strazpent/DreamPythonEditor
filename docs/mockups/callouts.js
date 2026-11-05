// Pins annotation markers and popups to the element they describe, so layout edits never misplace them.
//   <div class="callout" data-for="#runBtn" data-at="tr" data-dx="4" data-dy="0">   marker centred on a corner/edge
//   <div class="popup"   data-anchor="#cursor1" data-at="bl" data-dx="-40" data-dy="2">  top-left placed at that point
//   <div class="modalwin" data-cover="#outArea">   placed over that element with its exact position and size
// data-at: tl t tr l c r bl b br (default tr). Coordinates are relative to the element's offsetParent.
(function () {
  function point(r, at) {
    const xs = { l: r.left, c: r.left + r.width / 2, r: r.right };
    const ys = { t: r.top, c: r.top + r.height / 2, b: r.bottom };
    const v = at.length === 2 ? at[0] : (at === "t" || at === "b" ? at : "c");
    const h = at.length === 2 ? at[1] : (at === "l" || at === "r" ? at : "c");
    return [xs[h], ys[v]];
  }
  function place(el, sel, centre) {
    const target = document.querySelector(sel);
    if (!target) { el.style.outline = "2px solid red"; return; }
    const host = (el.offsetParent || document.body).getBoundingClientRect();
    const [x, y] = point(target.getBoundingClientRect(), el.dataset.at || (centre ? "tr" : "bl"));
    const dx = +(el.dataset.dx || 0), dy = +(el.dataset.dy || 0);
    const half = centre ? 12 : 0;
    el.style.left = (x - host.left + dx - half) + "px";
    el.style.top = (y - host.top + dy - half) + "px";
  }
  function placeAll() {
    document.querySelectorAll("[data-cover]").forEach(el => {
      const t = document.querySelector(el.dataset.cover);
      if (!t) { el.style.outline = "2px solid red"; return; }
      const host = (el.offsetParent || document.body).getBoundingClientRect();
      const r = t.getBoundingClientRect();
      Object.assign(el.style, { left: (r.left - host.left) + "px", top: (r.top - host.top) + "px",
                                width: r.width + "px", height: r.height + "px" });
    });
    document.querySelectorAll("[data-anchor]").forEach(el => place(el, el.dataset.anchor, false));
    document.querySelectorAll(".callout[data-for]").forEach(el => place(el, el.dataset.for, true));
    document.querySelectorAll(".ring[data-for]").forEach(el => {
      const t = document.querySelector(el.dataset.for);
      const host = (el.offsetParent || document.body).getBoundingClientRect();
      const r = t.getBoundingClientRect(), pad = +(el.dataset.pad || 3);
      Object.assign(el.style, { left: (r.left - host.left - pad) + "px", top: (r.top - host.top - pad) + "px",
                                width: (r.width + pad * 2) + "px", height: (r.height + pad * 2) + "px" });
    });
  }
  // Headless Edge first lays the page out slightly narrower than the screenshot, then resizes,
  // so pin again after load, on resize and once things settle.
  placeAll();
  window.addEventListener("load", placeAll);
  window.addEventListener("resize", placeAll);
  [100, 400, 1000].forEach(ms => setTimeout(placeAll, ms));
})();
