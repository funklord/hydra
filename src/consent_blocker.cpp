// SPDX-License-Identifier: GPL-3.0-or-later
#include "consent_blocker.h"

#include "policy_engine.h"
#include "consent_rules.h"

namespace {

// Runs in the isolated world (§13.2), like autofill and the picker: it reads
// and clicks the page's DOM, so the page must not be able to rewrite it.
//
// Two passes, and the order is the whole design. **Answer first, hide second.**
// Hiding a banner without answering it leaves the site believing it has not
// been asked -- the overlay comes back next load, the scroll lock often stays,
// and some players refuse to start until their consent object is set. Clicking
// is what actually ends the conversation; hiding is only for what a click
// leaves behind.
//
// Detection is by *button text within a consent-shaped container*, not by a
// list of vendor selectors alone. Vendor ids churn and a list of them is stale
// within weeks -- the same argument §11.5 makes for extractors. The vendor ids
// that are here are the handful that are stable and unambiguous, used to find
// the container faster; the text pass is what makes an unknown CMP work.
const char *k_script = R"JS(
(function () {
  if (window.__hydra_consent) return;
  window.__hydra_consent = 1;

  var RULES = __HYDRA_CONSENT_RULES__;
  var rx = function (p) { try { return p ? new RegExp(p, 'i') : null; }
                          catch (e) { return null; } };
  var REJECT = rx(RULES.reject), ACCEPT = rx(RULES.accept);
  var CONSENTISH = /cookie|consent|gdpr|privacy|tracking/i;

  var bridge = null, done = false, started = false;

  // Rendered at all. A button reading "OK" is legitimately tiny, and an
  // earlier version applied the *container's* size threshold to buttons too,
  // so the one banner whose only exit was a small button went unanswered --
  // exactly the case the option exists for.
  var shown = function (el) {
    if (!el) return false;
    var r = el.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) return false;
    var st = getComputedStyle(el);
    return st.visibility !== 'hidden' && st.display !== 'none' &&
           Number(st.opacity) !== 0;
  };
  // Big enough to be an overlay rather than a stray fixed pixel.
  var visible = function (el) {
    if (!shown(el)) return false;
    var r = el.getBoundingClientRect();
    return r.width >= 40 && r.height >= 20;
  };

  var buttons_in = function (root) {
    return Array.prototype.slice.call(root.querySelectorAll(
      'button, a[role="button"], [role="button"], input[type="button"],' +
      ' input[type="submit"], a')).filter(shown);
  };

  var label_of = function (el) {
    return ((el.textContent || '') + ' ' +
            (el.getAttribute('aria-label') || '') + ' ' +
            (el.value || '')).replace(/\s+/g, ' ').trim();
  };

  // A banner is a thing pinned over the page that talks about cookies and
  // offers a way out. Deliberately shape-based rather than a list of vendor
  // ids: ids churn, and an unknown CMP is the normal case, not the exception.
  // The vendor selectors in RULES.containers are a shortcut to the same
  // answer, not the mechanism.
  var candidates = function () {
    var out = [];
    for (var i = 0; i < RULES.containers.length; i++) {
      var found = document.querySelectorAll(RULES.containers[i]);
      for (var j = 0; j < found.length; j++) out.push(found[j]);
    }
    var all = document.body ? document.body.querySelectorAll('div, section, aside, dialog') : [];
    for (var k = 0; k < all.length; k++) {
      var el = all[k];
      var st = getComputedStyle(el);
      if (st.position !== 'fixed' && st.position !== 'sticky') continue;
      // Its own text, not its descendants' page content: a fixed sidebar
      // containing an article that mentions cookies is not a banner.
      var text = (el.textContent || '').slice(0, 1200);
      if (!CONSENTISH.test(text)) continue;
      out.push(el);
    }
    return out;
  };

  var act = function () {
    if (done) return true;
    var boxes = candidates();
    for (var b = 0; b < boxes.length; b++) {
      var box = boxes[b];
      if (!visible(box)) continue;
      var buttons = buttons_in(box);
      if (!buttons.length || buttons.length > 12) continue;
      // Whole banner per preference tier, so a reject anywhere beats an accept
      // that happens to come first in the DOM.
      var tiers = [ { re: REJECT, as: 'reject' }, { re: ACCEPT, as: 'accept' } ];
      for (var t = 0; t < tiers.length; t++) {
        if (!tiers[t].re) continue;
        for (var i = 0; i < buttons.length; i++) {
          var label = label_of(buttons[i]);
          if (!label || label.length > 60) continue;
          if (!tiers[t].re.test(label)) continue;
          try { buttons[i].click(); } catch (e) { continue; }
          done = true;
          if (bridge) bridge.report_dismissed(label.slice(0, 60), tiers[t].as);
          // Only what the click left behind. A banner that answered properly
          // usually removes itself; what it does not always undo is the scroll
          // lock, which is what makes the page unreadable.
          setTimeout(function () {
            if (visible(box)) box.style.setProperty('display', 'none', 'important');
            [document.documentElement, document.body].forEach(function (n) {
              if (n && getComputedStyle(n).overflow === 'hidden')
                n.style.setProperty('overflow', 'auto', 'important');
            });
          }, 400);
          return true;
        }
      }
    }
    return false;
  };

  // Banners arrive late, often after their own round trip, so one pass at load
  // time misses most of them. Watch for a while and then stop: an observer for
  // the life of the page is a permanent cost for a thing that happens once.
  var start = Date.now();
  var obs = new MutationObserver(function () {
    if (act() || Date.now() - start > 15000) obs.disconnect();
  });
  var begin = function () {
    if (act()) return;
    if (document.documentElement)
      obs.observe(document.documentElement, { childList: true, subtree: true });
    setTimeout(function () { obs.disconnect(); }, 15000);
  };

  var connect = function () {
    if (!window.hydraChannel) return setTimeout(connect, 50);
    window.hydraChannel(function (objs) {
      bridge = objs.hydraConsent;
      if (!bridge) return;
      // Whether to act at all is C++'s decision, asked per page rather than
      // assumed from the script being present: the script is injected once when
      // the view is built, and the per-site setting can change after that.
      //
      // Asked more than once, because the answer depends on the shell knowing
      // which page this is, and that arrives on its own navigation signal. A
      // single ask at channel-connect time raced it and lost -- the answer was
      // "no host yet, so no", and nothing ever asked again.
      var asks = 0;
      var ask = function () {
        if (started) return;
        bridge.active_now(function (on) {
          if (on) { started = true; begin(); }
          else if (++asks < 20) setTimeout(ask, 250);
        });
      };
      ask();
    });
  };
  connect();
})();
)JS";

}  // namespace

consent_blocker::consent_blocker(policy_engine *policy, QObject *parent)
	: QObject(parent), m_policy(policy) {}

QString consent_blocker::script_source(const consent_rules &rules) {
	// The rules are substituted in rather than fetched by the script, so what a
	// page can see is what it was going to be judged by anyway, and there is no
	// second copy in JavaScript to drift from the C++ one.
	return QString::fromUtf8(k_script)
	    .replace("__HYDRA_CONSENT_RULES__", rules.to_script_literal());
}

void consent_blocker::set_page_host(const QString &host) {
	m_host = host;
}

bool consent_blocker::active_for(const QString &host) const {
	// `block` on this feature means "answer the banner". A site the user has
	// set to `allow` keeps its banner and never gets the script at all.
	return m_policy && !m_policy->is_allowed(policy::feature::cookie_notices, host);
}

void consent_blocker::report_dismissed(const QString &what, const QString &choice) {
	// The host is the shell's, never the page's. A frame cannot get a
	// relaxation applied to a site it does not speak for.
	const QString host = m_host;
	if (host.isEmpty() || !active_for(host))
		return;

	m_dismissed.prepend(QString("%1 (%2)").arg(what.left(80), choice.left(20)));
	while (m_dismissed.size() > 20)
		m_dismissed.removeLast();
	emit acted(host, choice.left(20));

	// The choice has to survive the next load or the banner simply returns, and
	// a consent choice is recorded in a cookie. First-party only: third-party
	// cookies are what these dialogs are bargaining for, and quietly granting
	// them would turn a convenience into the thing it is meant to protect
	// against. Written as an ordinary per-site rule so the shield shows it and
	// can undo it.
	if (!m_policy)
		return;
	if (m_policy->is_allowed(policy::feature::cookies, host))
		return;
	m_policy->set_setting(host, policy::feature::cookies, policy::setting::allow);
	emit relaxed_cookies(host);
}
