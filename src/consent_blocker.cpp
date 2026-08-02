// SPDX-License-Identifier: GPL-3.0-or-later
#include "consent_blocker.h"

#include "policy_engine.h"
#include "site_rules.h"

#include <QRegularExpression>

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

  var RULES = { containers: [], reject: '', accept: '' };
  var rx = function (p) { try { return p ? new RegExp(p, 'i') : null; }
                          catch (e) { return null; } };
  var REJECT = null, ACCEPT = null;
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

  var reported = {};
  // A banner we could see and could not answer. Reported once per set of
  // labels, because the observer fires repeatedly and this is one finding.
  var unanswered = function (labels) {
    var key = labels.join('|');
    if (reported[key] || !bridge) return;
    reported[key] = 1;
    bridge.report_unhandled(labels.join('\t'));
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
      // Consent-shaped, on screen, and nothing in it matched. That is the case
      // a new rule has to cover, and what it offered is most of the answer.
      var labels = [];
      for (var q = 0; q < buttons.length; q++) {
        var l = label_of(buttons[q]);
        if (l && l.length <= 60) labels.push(l);
      }
      if (labels.length) unanswered(labels);
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
          if (!on) { if (++asks < 20) setTimeout(ask, 250); return; }
          // Rules fetched now rather than baked in when this script was
          // injected, so a rule accepted a minute ago applies to this load.
          bridge.rules_json(function (json) {
            try { RULES = JSON.parse(json); } catch (e) { return; }
            REJECT = rx(RULES.reject);
            ACCEPT = rx(RULES.accept);
            started = true;
            begin();
          });
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

QString consent_blocker::script_source() { return QString::fromUtf8(k_script); }

void consent_blocker::set_page_host(const QString &host) {
	m_host = host;
}

bool consent_blocker::active_for(const QString &host) const {
	// `block` on this feature means "answer the banner". A site the user has
	// set to `allow` keeps its banner and never gets the script at all.
	return m_policy && !m_policy->is_allowed(policy::feature::cookie_notices, host);
}

void consent_blocker::report_unhandled(const QString &labels) {
	const QString host = m_host;
	if (host.isEmpty() || !active_for(host))
		return;
	// Bounded and deduplicated: the observer fires per mutation on some pages,
	// and the same banner arriving twenty times is one finding, not twenty.
	const QString row = host + "\t" + labels.left(400);
	if (m_unhandled.contains(row))
		return;
	m_unhandled.prepend(row);
	while (m_unhandled.size() > 20)
		m_unhandled.removeLast();
	emit found_unanswerable(host, labels.left(400));
}

site_rule consent_blocker::rule_from_label(const QString &label,
                                               const QString &as) const {
	site_rule r;
	r.kind = (as == "reject") ? QStringLiteral("reject") : QStringLiteral("accept");
	// Anchored and escaped: a label is text a page chose, and dropping it into a
	// regex unescaped would let a banner reading "(.*)" match every button on
	// every site from then on -- a rule that is meant to be shared, no less.
	r.value = "^" + QRegularExpression::escape(label.trimmed().left(60)) + "$";
	// No host, deliberately. See the header: a label describes a shape banners
	// take, not a site, so it is generic and `add()` will flag it for the
	// built-in set.
	r.note = "learned from a banner that could not be answered";
	return r;
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
