#include "consent_blocker.h"

#include "policy_engine.h"
#include "site_rules.h"

#include <QRegularExpression>

namespace {

// Runs in the isolated world (sec 13.2), like autofill and the picker: it reads
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
// within weeks -- the same argument sec 11.5 makes for extractors. The vendor ids
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
  // **Each of a button's names on its own**, as well as the three together.
  // YouTube's reads "Reject all" and carries the aria-label "Reject the use
  // of cookies and other data for the purposes described"; joined, they made
  // a label too long to consider and one no anchored pattern matched.
  var names_of = function (el) {
    var out = [];
    [el.textContent, el.getAttribute('aria-label'), el.value, label_of(el)]
      .forEach(function (n) {
        n = (n || '').replace(/\s+/g, ' ').trim();
        if (n && n.length <= 60 && out.indexOf(n) < 0) out.push(n);
      });
    return out;
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
    // Custom elements too, whose tag always has a hyphen. YouTube's mobile
    // consent dialog is pinned by `ytm-consent-bump-v2-renderer`, and a scan
    // of div, section, aside and dialog alone never saw it -- the dialog
    // stayed up on every first visit (measured on the emulator, 2026-10-29).
    var all = document.body ? document.body.querySelectorAll('*') : [];
    for (var k = 0; k < all.length; k++) {
      var el = all[k];
      var tag = el.tagName;
      if (tag !== 'DIV' && tag !== 'SECTION' && tag !== 'ASIDE' && tag !== 'DIALOG' &&
          tag.indexOf('-') < 0)
        continue;
      var st = getComputedStyle(el);
      if (st.position !== 'fixed' && st.position !== 'sticky') continue;
      // Its own text, not its descendants' page content: a fixed sidebar
      // containing an article that mentions cookies is not a banner.
      var text = (el.textContent || '').slice(0, 1200);
      if (!CONSENTISH.test(text)) continue;
      out.push(el);
    }
    // **In a vendor's frame the frame is the banner.** The page pins the
    // frame; nothing inside it need be pinned at all -- Sourcepoint's, on
    // the Guardian, has no fixed element, so the scan found nothing to answer
    // (measured over DevTools on the emulator, 2026-10-29). In a subframe
    // that talks about cookies, its body stands in for the box -- but only
    // when one of its buttons reads like a consent answer. A frame that
    // merely mentions privacy is not a banner: YouTube embeds Google's
    // sign-in page, whose footer does, and it would otherwise have been
    // reported as a banner nothing could answer.
    if (!TOP && !out.length && document.body &&
        CONSENTISH.test((document.body.textContent || '').slice(0, 2000)) &&
        buttons_in(document.body).some(function (b) {
          return names_of(b).some(answers);
        }))
      out.push(document.body);
    return out;
  };

  var reported = {};
  // A banner we could see and could not answer. Reported once per set of
  // labels, because the observer fires repeatedly and this is one finding.
  var unanswered = function (labels) {
    var key = labels.join('|');
    if (reported[key]) return;
    if (!bridge && TOP) return;
    reported[key] = 1;
    if (bridge) bridge.report_unhandled(labels.join('\t'));
    else
      try {
        window.top.postMessage({ __hydra_consent_none: labels.join('\t') }, '*');
      } catch (e) {}
  };

  // **Pay or consent: neither is taken.** A banner whose only refusal is
  // a paid one -- "Reject all and subscribe", an ad-free or "Ad-Lite" tier,
  // a "PUR Abo" -- is not offering a choice, it is selling the refusal. The
  // holder's call, 2026-10-29: such a wall is set aside, not accepted and
  // not paid. Nothing is clicked, so nothing is agreed to; the wall is
  // hidden and the scroll it locked is given back. The content is served
  // whole beneath these, so this is the same act as answering any banner.
  var PAID = /subscri|prenumer|abonn|\babo\b|\bpay\b|\bbetala|bezahl|\bpur\b|ad-?lite|ad-?free|reklamfri|werbefrei|contentpass|without ads/i;
  // A consent answer of any kind: what makes a frame's body a banner.
  var answers = function (n) {
    return (REJECT && REJECT.test(n)) || (ACCEPT && ACCEPT.test(n)) || PAID.test(n);
  };
  var unlock = function () {
    [document.documentElement, document.body].forEach(function (n) {
      if (!n) return;
      var st = getComputedStyle(n);
      if (st.overflow === 'hidden' || st.overflowY === 'hidden')
        n.style.setProperty('overflow', 'auto', 'important');
      if (st.position === 'fixed' && n === document.body)
        n.style.setProperty('position', 'static', 'important');
    });
  };
  var set_aside = function (box, label) {
    done = true;
    box.style.setProperty('display', 'none', 'important');
    unlock();
    if (bridge) bridge.report_dismissed(label.slice(0, 60), 'aside');
    else if (!TOP)
      try {
        window.top.postMessage({ __hydra_consent_aside: label.slice(0, 60) }, '*');
      } catch (e) {}
  };
  // Paid if any button sells something -- "Reject all and subscribe" on
  // the Guardian, but also a bare "Subscribe now" (Spiegel), "Jetzt BILD PUR
  // abonnieren" (Bild) or "S'abonner" (Le Monde) beside an accept -- and no
  // button refuses for free. Where a free refusal exists it is taken instead.
  var pay_or_consent = function (buttons) {
    var paid = '', free_refusal = false;
    for (var i = 0; i < buttons.length; i++) {
      var names = names_of(buttons[i]);
      for (var n = 0; n < names.length; n++) {
        if (PAID.test(names[n])) { if (!paid) paid = names[n]; }
        else if (REJECT && REJECT.test(names[n])) free_refusal = true;
      }
    }
    return free_refusal ? '' : paid;
  };

  var act = function () {
    if (done) return true;
    var boxes = candidates();
    for (var b = 0; b < boxes.length; b++) {
      var box = boxes[b];
      if (!visible(box)) continue;
      var buttons = buttons_in(box);
      // Twelve keeps a pinned box on a page honest -- a toolbar of links is
      // not a banner. A vendor frame's body is nothing but the dialog, and
      // Bild's lists each purpose as a button: thirty-five of them, measured.
      var most = (!TOP && box === document.body) ? 40 : 12;
      if (!buttons.length || buttons.length > most) continue;
      var paid = pay_or_consent(buttons);
      if (paid) { set_aside(box, paid); return true; }
      // Whole banner per preference tier, so a reject anywhere beats an accept
      // that happens to come first in the DOM.
      var tiers = [ { re: REJECT, as: 'reject' }, { re: ACCEPT, as: 'accept' } ];
      for (var t = 0; t < tiers.length; t++) {
        if (!tiers[t].re) continue;
        for (var i = 0; i < buttons.length; i++) {
          var names = names_of(buttons[i]), label = '';
          // A name that sells something is never clicked, in either tier.
          for (var n = 0; n < names.length && !label; n++)
            if (tiers[t].re.test(names[n]) && !PAID.test(names[n])) label = names[n];
          if (!label) continue;
          try { buttons[i].click(); } catch (e) { continue; }
          done = true;
          if (bridge) bridge.report_dismissed(label.slice(0, 60), tiers[t].as);
          else if (!TOP)
            try {
              window.top.postMessage({ __hydra_consent_did:
                { label: label.slice(0, 60), as: tiers[t].as } }, '*');
            } catch (e) {}
          // **A click that did not take is clicked again, before anything is
          // hidden.** A banner is often drawn before its buttons are wired:
          // YouTube's took the first click and did nothing, and hiding the
          // box 400 ms later then made the page look answered while the site
          // never recorded a choice, so the dialog came back on every visit.
          // Up to three clicks, then the box and any scroll lock it left are
          // put away as a last resort -- the page has to be usable either way.
          var button = buttons[i], clicks = 1;
          var settle = function () {
            if (visible(box) && clicks < 3) {
              clicks++;
              try { button.click(); } catch (e) {}
              setTimeout(settle, 700);
              return;
            }
            if (visible(box)) box.style.setProperty('display', 'none', 'important');
            [document.documentElement, document.body].forEach(function (n) {
              if (n && getComputedStyle(n).overflow === 'hidden')
                n.style.setProperty('overflow', 'auto', 'important');
            });
          };
          setTimeout(settle, 700);
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
  //
  // **Longer in a vendor's frame.** Its dialog is drawn only once the page
  // has handed it its configuration, and a frame starts as soon as it has
  // its rules -- so on a heavy page the frame's fifteen seconds ran out
  // before Spiegel's Sourcepoint dialog existed. A frame watches for a
  // minute; it is a small document, and the cost is its own.
  // Decided in `begin`, which runs after TOP is known; read here it was
  // still undefined.
  var window_ms = 15000;
  var start = Date.now();
  var obs = new MutationObserver(function () {
    if (act() || Date.now() - start > window_ms) obs.disconnect();
  });
  var begin = function () {
    window_ms = TOP ? 15000 : 60000;
    start = Date.now();
    if (act()) return;
    if (document.documentElement)
      obs.observe(document.documentElement, { childList: true, subtree: true,
                                              attributes: !TOP });
    setTimeout(function () { obs.disconnect(); }, window_ms);
    // **And once a second in a frame**, because a dialog that fades in
    // changes no DOM: Spiegel's Sourcepoint buttons were transparent at
    // every mutation, so each look found nothing, and the fade finishing
    // asked for no look at all (traced on the emulator, 2026-10-29).
    if (!TOP) {
      var tick = setInterval(function () {
        if (act() || Date.now() - start > window_ms) clearInterval(tick);
      }, 1000);
    }
  };

  var TOP = (window.top === window);

  // A frame is not allowed a bridge -- Qt puts the channel transport in the main
  // frame alone -- so the two halves speak to each other instead. The top frame
  // is the only one that can ask C++ anything, and it answers for its children;
  // a child asks it for the rules and hands its dismissals back. Without this a
  // CMP shipped as an iframe, which is how vendors ship them, is simply left
  // standing.
  //
  // The top frame decides *whether* to act, once, for the page. A child cannot
  // talk it into acting on a page where the user turned this off, and cannot
  // report on behalf of a site it is not on, because the host C++ files under is
  // the shell's and never the message's.
  window.addEventListener('message', function (e) {
    if (!e || !e.data) return;
    if (TOP && e.data.__hydra_consent_need && e.source) {
      if (!started || !e.source.postMessage) return;
      try {
        e.source.postMessage({ __hydra_consent_rules: RULES }, '*');
      } catch (err) {}
      return;
    }
    if (TOP && e.data.__hydra_consent_did && bridge) {
      // **A different method from the one this frame's own click uses**, and
      // the difference is that this one changes no policy. A message event
      // names no sender the shell can trust -- any frame, or the page's own
      // script in the world it does control -- and the version that relaxed
      // first-party cookies from here let a page with no banner ask for its
      // block to be lifted. Measured; see `report_dismissed_in_frame`.
      var d = e.data.__hydra_consent_did;
      bridge.report_dismissed_in_frame(String(d.label || '').slice(0, 60),
                                        String(d.as || '').slice(0, 20));
      return;
    }
    if (TOP && e.data.__hydra_consent_aside && e.source) {
      // Only the frame that sent this is hidden, found by its own window, so
      // a frame can put away itself and nothing else. With it go any pinned
      // ancestors holding it, which is where vendors draw the dimmed
      // backdrop, and the page's scroll lock.
      if (!started) return;
      var frames = document.getElementsByTagName('iframe');
      for (var f = 0; f < frames.length; f++) {
        if (frames[f].contentWindow !== e.source) continue;
        var el = frames[f];
        el.style.setProperty('display', 'none', 'important');
        for (var up = el.parentElement; up && up !== document.body; up = up.parentElement) {
          var ps = getComputedStyle(up).position;
          if (ps === 'fixed' || ps === 'sticky')
            up.style.setProperty('display', 'none', 'important');
        }
        unlock();
        if (bridge)
          bridge.report_dismissed_in_frame(
            String(e.data.__hydra_consent_aside).slice(0, 60), 'aside');
        break;
      }
      return;
    }
    if (TOP && e.data.__hydra_consent_none && bridge) {
      bridge.report_unhandled(String(e.data.__hydra_consent_none).slice(0, 400));
      return;
    }
    if (!TOP && e.data.__hydra_consent_rules) {
      if (started) return;
      RULES = e.data.__hydra_consent_rules;
      REJECT = rx(RULES.reject);
      ACCEPT = rx(RULES.accept);
      started = true;
      begin();
    }
  }, true);

  // In a frame there is nothing to connect to; ask upward instead, until the
  // top frame has an answer to give.
  if (!TOP) {
    // Thirty seconds, not ten: on a heavy page the top frame starts late --
    // Spiegel's had not started when its consent frame stopped asking, and
    // the wall stayed. The top frame also sends its rules to every frame as
    // it starts, so a frame that asked too early is answered anyway.
    var tries = 0;
    var beg = function () {
      if (started || ++tries > 120) return;
      try { window.top.postMessage({ __hydra_consent_need: 1 }, '*'); } catch (e) {}
      setTimeout(beg, 250);
    };
    beg();
    return;
  }

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
            var frames = document.getElementsByTagName('iframe');
            for (var f = 0; f < frames.length; f++)
              try {
                frames[f].contentWindow.postMessage({ __hydra_consent_rules: RULES }, '*');
              } catch (e) {}
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

// Bounded and deduplicated: the observer fires per mutation on some pages,
// and the same banner arriving twenty times is one finding, not twenty. Split
// out of `report_unhandled` so the window aggregator can be handed a finding a
// per-view blocker made, under that view's host rather than this object's.
bool consent_blocker::record_unhandled(const QString &host,
                                        const QString &labels) {
	const QString row = host + "\t" + labels.left(400);
	if (m_unhandled.contains(row))
		return false;
	m_unhandled.prepend(row);
	while (m_unhandled.size() > 20)
		m_unhandled.removeLast();
	return true;
}

void consent_blocker::report_unhandled(const QString &labels) {
	const QString host = m_host;
	if (host.isEmpty() || !active_for(host))
		return;
	if (record_unhandled(host, labels))
		emit found_unanswerable(host, labels.left(400));
}

bool consent_blocker::forget_unhandled(const QString &host,
                                         const QString &label) {
	bool dropped = false;
	for (int i = m_unhandled.size() - 1; i >= 0; --i) {
		QStringList parts = m_unhandled.at(i).split(QLatin1Char('\t'));
		if (parts.isEmpty() || parts.first() != host)
			continue;
		// From index 1: the first field is the host, and a host that happens
		// to read like one of its own button labels must not be removed as
		// though it were one.
		for (int j = parts.size() - 1; j >= 1; --j) {
			if (parts.at(j) == label) {
				parts.removeAt(j);
				dropped = true;
			}
		}
		if (!dropped)
			continue;
		if (parts.size() <= 1)
			m_unhandled.removeAt(i);      // nothing left to review
		else
			m_unhandled[i] = parts.join(QLatin1Char('\t'));
		break;
	}
	return dropped;
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

void consent_blocker::report_dismissed_in_frame(const QString &what,
                                                 const QString &choice) {
	// A child frame's dismissal, relayed by the top frame. Recorded, and no
	// policy change -- see the header for why that is the correct half rather
	// than the cautious one.
	Q_UNUSED(what);
	const QString host = m_host;
	if (host.isEmpty() || !active_for(host))
		return;
	emit acted(host, choice.left(20));
}

void consent_blocker::report_dismissed(const QString &what, const QString &choice) {
	// **`what` is deliberately unused**, and the parameter stays because the
	// injected script calls this with both arguments. See the note below: the
	// list that held the banner text is gone, and nothing else ever read it.
	// Marked rather than left, so the build's own warning stays the signal it
	// is meant to be.
	Q_UNUSED(what);

	// The host is the shell's, never the page's. A frame cannot get a
	// relaxation applied to a site it does not speak for.
	const QString host = m_host;
	if (host.isEmpty() || !active_for(host))
		return;

	// **The list this used to keep is gone.** It held the last twenty banner
	// texts with the choice made on each, its accessor said it was "for the
	// status line and the tests", and neither ever called it -- the status
	// line is fed by `acted` below, which is live and says the same thing at
	// the moment it happens. What was left was a per-site record of pages
	// somebody had been on, maintained for nobody and clearable by nothing.
	emit acted(host, choice.left(20));
	// **A wall set aside records no choice**, so there is nothing for a cookie
	// to remember and no reason to relax one: pay-or-consent banners are
	// hidden, not answered (see `set_aside` in the script).
	if (choice == QLatin1String("aside"))
		return;

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
