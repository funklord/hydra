// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The content script injected into every page (architecture doc §13.2). Kept
// as a string rather than a resource so the injection seam stays the only
// engine-specific piece — an Android backend injects the same source.
//
// Field detection is the fiddly "just works" part every password manager
// faces, so it leans on the standard signals: type=password, the
// current-password / new-password autocomplete values, and input types and
// names that identify a username field.
//
// Two §13.3 rules are visible here and are also enforced on the C++ side,
// because a page can rewrite anything in its own document:
//   * the script never asks about an origin it invents — it sends
//     location.origin, and the controller checks that against the real URL;
//   * nothing is auto-submitted. Fields are filled and the user presses the
//     button themselves.
namespace autofill_script {

inline const char *source() {
	return R"JS(
(function () {
  'use strict';
  if (window.__hydraAutofill) return;
  window.__hydraAutofill = true;

  function isVisible(el) {
    if (!el || el.disabled || el.readOnly) return false;
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  }

  function passwordFields() {
    return Array.from(document.querySelectorAll('input[type=password]'))
                .filter(isVisible);
  }

  // The username is the visible text-ish input closest above the password
  // field, preferring one that advertises itself.
  function usernameFor(pw) {
    const form = pw.form || document;
    const cands = Array.from(form.querySelectorAll('input')).filter(function (el) {
      const t = (el.type || 'text').toLowerCase();
      return isVisible(el) && ['text', 'email', 'tel', ''].indexOf(t) >= 0;
    });
    const strong = cands.filter(function (el) {
      const hay = ((el.autocomplete || '') + ' ' + (el.name || '') + ' ' +
                   (el.id || '') + ' ' + (el.getAttribute('aria-label') || ''))
                  .toLowerCase();
      return /user|login|email|account/.test(hay);
    });
    const pool = strong.length ? strong : cands;
    let best = null;
    for (const el of pool) {
      if (el.compareDocumentPosition(pw) & Node.DOCUMENT_POSITION_FOLLOWING)
        best = el;   // appears before the password field
    }
    return best || pool[0] || null;
  }

  function setValue(el, value) {
    if (!el) return;
    // Assign through the native setter so frameworks that watch the property
    // (React and friends) see the change instead of silently reverting it.
    const proto = Object.getPrototypeOf(el);
    const desc = Object.getOwnPropertyDescriptor(proto, 'value');
    if (desc && desc.set) desc.set.call(el, value); else el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  function fill(entries) {
    if (!entries || !entries.length) return;
    // Multiple matches are the user's choice to make; take the first only
    // when there is exactly one, otherwise leave it to the toolbar UI.
    if (entries.length > 1) return;
    const e = entries[0];
    for (const pw of passwordFields()) {
      // A new-password field is a registration form, not a login — filling it
      // with the existing password is wrong.
      if ((pw.autocomplete || '').toLowerCase() === 'new-password') continue;
      setValue(usernameFor(pw), e.login);
      setValue(pw, e.password);
      break;   // never auto-submit (§13.3)
    }
  }

  function connect() {
    // Wait for the page's shared channel rather than the raw transport: it is
    // what hands out the bridge objects now.
    if (!window.hydraChannel) return setTimeout(connect, 50);
    window.hydraChannel(function (objects) {
      const af = objects.hydraAutofill;
      if (!af) return;
      af.credentials_ready.connect(function (json) {
        try { fill(JSON.parse(json)); } catch (err) { /* malformed; ignore */ }
      });
      window.__hydraRequestFill = function () {
        af.request_credentials(String(location.origin));
      };
      if (passwordFields().length) window.__hydraRequestFill();
    });
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', connect);
  else
    connect();
})();
)JS";
}

}  // namespace autofill_script
