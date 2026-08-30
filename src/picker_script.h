#pragma once

// The "zap this" overlay (architecture doc sec 12.1), injected through the same
// isolated-world seam the autofill script uses.
//
// It highlights whatever is under the cursor, captures it on click, and sends
// the shape of the element back -- selector, tag, id, classes and a trimmed
// outerHTML. Escape cancels. It deliberately does not block the ad itself:
// what it produces is evidence for a *proposal* the user then reviews, which is
// the whole point of the diff/accept pipeline.
//
// The text nodes are stripped before the snippet is sent (sec 12.2 asks for
// personal data stripped from the snippet). Keeping structure and dropping
// prose is the cheap version of that, and it loses nothing the model needs to
// write a selector.
namespace picker_script {

inline const char *source() {
	return R"JS(
(function () {
  'use strict';
  if (window.__hydraPicker) return;
  window.__hydraPicker = true;

  let active = false, box = null, current = null;

  function overlay() {
    if (box) return box;
    box = document.createElement('div');
    box.style.cssText = 'position:fixed;z-index:2147483647;pointer-events:none;' +
      'border:2px solid #e74c3c;background:rgba(231,76,60,0.18);display:none;';
    document.documentElement.appendChild(box);
    return box;
  }

  function highlight(el) {
    const b = overlay();
    if (!el) { b.style.display = 'none'; return; }
    const r = el.getBoundingClientRect();
    b.style.display = 'block';
    b.style.left = r.left + 'px';
    b.style.top = r.top + 'px';
    b.style.width = r.width + 'px';
    b.style.height = r.height + 'px';
  }

  // A selector specific enough to be useful, general enough to survive a
  // reload: prefer an id, then tag plus stable-looking classes, then fall back
  // to an nth-child path.
  function selectorFor(el) {
    if (el.id && /^[A-Za-z][\w-]*$/.test(el.id)) return '#' + el.id;
    const tag = el.tagName.toLowerCase();
    const classes = Array.from(el.classList).filter(function (c) {
      // Skip hashed/generated class names — they change on every deploy and a
      // filter rule built on one is dead by tomorrow.
      return /^[A-Za-z][\w-]*$/.test(c) && !/^(css|sc|jsx)-/.test(c) &&
             !/[0-9a-f]{6,}/i.test(c);
    });
    if (classes.length) return tag + '.' + classes.slice(0, 3).join('.');
    const parent = el.parentElement;
    if (!parent) return tag;
    const idx = Array.prototype.indexOf.call(parent.children, el) + 1;
    return selectorFor(parent) + ' > ' + tag + ':nth-child(' + idx + ')';
  }

  // Structure without prose: the model needs the shape, not the copy.
  function shapeOf(el, depth) {
    if (depth > 2) return '';
    let s = '<' + el.tagName.toLowerCase();
    for (const a of Array.from(el.attributes)) {
      if (a.name === 'style') continue;
      // `value` is whatever the user typed. Text nodes are already dropped
      // here, which is what §12.2's "strip personal data" was taken to mean,
      // but an input's value is not a text node and is the most personal thing
      // on a page -- an address, a search, a card number. The snippet exists to
      // show the model an element's *shape*, and a value never contributes to
      // that, so there is nothing to weigh against dropping it.
      if (a.name === 'value') continue;
      s += ' ' + a.name + '="' + String(a.value).slice(0, 80) + '"';
    }
    s += '>';
    for (const c of Array.from(el.children)) s += shapeOf(c, depth + 1);
    return s + '</' + el.tagName.toLowerCase() + '>';
  }

  function capture(el) {
    return JSON.stringify({
      tag: el.tagName.toLowerCase(),
      id: el.id || '',
      classes: Array.from(el.classList),
      selector: selectorFor(el),
      snippet: shapeOf(el, 0).slice(0, 1200)
    });
  }

  function stop() {
    active = false;
    current = null;
    highlight(null);
    document.removeEventListener('mousemove', onMove, true);
    document.removeEventListener('click', onClick, true);
    document.removeEventListener('keydown', onKey, true);
  }

  function onMove(e) {
    if (!active) return;
    current = e.target;
    highlight(current);
  }

  function onClick(e) {
    if (!active) return;
    e.preventDefault();
    e.stopPropagation();
    const el = current || e.target;
    stop();
    if (window.__hydraPickerSend) window.__hydraPickerSend(capture(el));
  }

  function onKey(e) {
    if (!active || e.key !== 'Escape') return;
    e.preventDefault();
    stop();
    if (window.__hydraPickerCancel) window.__hydraPickerCancel();
  }

  function start() {
    if (active) return;
    active = true;
    // Capture phase, so a page that swallows its own clicks cannot stop us.
    document.addEventListener('mousemove', onMove, true);
    document.addEventListener('click', onClick, true);
    document.addEventListener('keydown', onKey, true);
  }

  function connect() {
    // Wait for the page's shared channel rather than the raw transport: it is
    // what hands out the bridge objects now.
    if (!window.hydraChannel) return setTimeout(connect, 50);
    window.hydraChannel(function (objects) {
      const p = objects.hydraPicker;
      if (!p) return;
      window.__hydraPickerSend = function (json) { p.element_picked(json); };
      window.__hydraPickerCancel = function () { p.cancelled(); };
      p.pick_requested.connect(start);
    });
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', connect);
  else
    connect();
})();
)JS";
}

}  // namespace picker_script
