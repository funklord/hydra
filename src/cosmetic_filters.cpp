#include "cosmetic_filters.h"

#include "policy_engine.h"
#include "filter_list.h"

#include <QJsonArray>
#include <QLoggingCategory>
#include <QJsonDocument>

void cosmetic_filters::set_page_host(const QString &host) {
	m_host = host;
	if (qEnvironmentVariableIsSet("HYDRA_FILTER_DEBUG"))
		qWarning("cosmetic: page host is now \"%s\"", qPrintable(m_host));
}

void generic_cosmetics::add_exception(const QString &host, const QString &selector) {
	if (host.isEmpty()) {
		selectors.removeAll(selector);
		procedural.removeIf([&](const procedural_cosmetic::scoped_rule &r) {
			return r.selector == selector;
		});
	} else
		excepted[host].insert(selector);
}

bool generic_cosmetics::covers(const QSet<QString> &hosts, const QString &host) {
	if (hosts.isEmpty() || host.isEmpty())
		return false;
	QString h = host.toLower();
	for (;;) {
		if (hosts.contains(h))
			return true;
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			return false;
		h = h.mid(dot + 1);
	}
}

QStringList generic_cosmetics::for_host(const QString &host) const {
	if (host.isEmpty() || covers(generichide, host) || covers(elemhide, host))
		return {};
	QSet<QString> lifted;
	QString h = host.toLower();
	for (;;) {
		const auto it = excepted.constFind(h);
		if (it != excepted.constEnd())
			lifted.unite(*it);
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			break;
		h = h.mid(dot + 1);
	}
	if (lifted.isEmpty())
		return selectors;
	QStringList out;
	out.reserve(selectors.size());
	for (const QString &s : selectors)
		if (!lifted.contains(s))
			out << s;
	return out;
}

QJsonArray generic_cosmetics::procedural_for(const QString &host) const {
	QJsonArray out;
	if (host.isEmpty() || procedural.isEmpty() || hides_nothing_on(host))
		return out;
	QSet<QString> lifted;
	QString h = host.toLower();
	for (;;) {
		const auto it = excepted.constFind(h);
		if (it != excepted.constEnd())
			lifted.unite(*it);
		const int dot = h.indexOf(QLatin1Char('.'));
		if (dot < 0)
			break;
		h = h.mid(dot + 1);
	}
	for (const procedural_cosmetic::scoped_rule &r : procedural)
		if (!lifted.contains(r.selector) && filter_list::scope_matches(r.scope, host))
			out.append(r.rule);
	return out;
}

QStringList cosmetic_filters::selectors() const {
	// `$elemhide` on this host: no cosmetic rule of any list applies.
	if (m_generic && m_generic->hides_nothing_on(m_host))
		return {};
	// Deduplicated through a set: with the generic rules this is fourteen
	// thousand entries, and a list search per entry was quadratic.
	QStringList out;
	QSet<QString> seen;
	auto take = [&](const QStringList &more) {
		for (const QString &s : more)
			if (!seen.contains(s)) {
				seen.insert(s);
				out << s;
			}
	};
	take(selectors_for(m_list, m_host, m_policy));
	take(selectors_for(m_subscribed, m_host, m_policy));
	// Generic rules answer to the shield as every list rule does: a site
	// whose ads are allowed gets none.
	if (m_generic && !m_host.isEmpty() &&
	    !(m_policy && m_policy->is_allowed(policy::feature::ads, m_host)))
		take(m_generic->for_host(m_host));
	// Without the policy: like the network trial, an investigation applies
	// what it is trying even where the shield allows ads.
	take(selectors_for(m_trial, m_host));
	return out;
}

QString cosmetic_filters::debug_state() const {
	return QStringLiteral("host=%1 selectors=%2")
	    .arg(m_host, selectors().join(','));
}

QStringList cosmetic_filters::selectors_for(const filter_list *list,
                                             const QString &host,
                                             const policy_engine *policy) {
	QStringList out;
	if (!list || host.isEmpty())
		return out;
	// **The shield's escape hatch, honoured here as it is in the interceptor.**
	// `request_filter::decide` skips the whole imported list for a site whose
	// `ads` setting is `allow`, on the stated grounds that a half-working escape
	// leaves the page failing for a reason the person was told they had turned
	// off. Cosmetic rules are the other half of that list and were not skipped,
	// so they went on hiding elements after somebody allowed ads to un-break a
	// page. Same check, same feature, same reason.
	if (policy && policy->is_allowed(policy::feature::ads, host))
		return out;
	for (const filter_rule &r : list->rules()) {
		if (!r.cosmetic)
			continue;
		// For a cosmetic rule `scope` really is the site it applies on -- unlike a
		// network rule, where the same field holds the host being blocked. That
		// dual meaning is why this reads the field only after checking `cosmetic`.
		//
		// A rule with no scope is not applied anywhere. `evaluate()` rejects one
		// at accept time for being unbounded, so any that exists came from an
		// edited file, and silently honouring it everywhere would route around
		// the check that exists to stop exactly that.
		if (r.scope.isEmpty())
			continue;
		// **And it has to be a selector**, checked here as well as at accept
		// time and for the same reason the scope is: a rule that reached the
		// file by any route other than the review must not reach the page.
		if (!filter_list::why_selector_unsafe(
		         r.text.mid(r.text.indexOf("##") + 2).trimmed()).isEmpty())
			continue;
		// A list, not a host: `a.example,b.example##.ad` was compared whole
		// against the hostname and so applied on neither -- the fault the
		// scriptlet runner had, met again here.
		if (!filter_list::scope_matches(r.scope, host))
			continue;
		const int hash = r.text.indexOf("##");
		if (hash < 0)
			continue;
		const QString selector = r.text.mid(hash + 2).trimmed();
		if (!selector.isEmpty() && !out.contains(selector))
			out << selector;
	}
	return out;
}

QString cosmetic_filters::selectors_json() const {
	QJsonArray arr;
	for (const QString &s : selectors())
		arr.append(s);
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString cosmetic_filters::procedural_json() const {
	// The shield's escape hatch, as for the selectors: a site whose ads are
	// allowed gets no list's hiding.
	QJsonArray arr;
	if (m_generic && !m_host.isEmpty() &&
	    !(m_policy && m_policy->is_allowed(policy::feature::ads, m_host)))
		arr = m_generic->procedural_for(m_host);
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString cosmetic_filters::script_source() {
	// Runs in the isolated world, which shares the DOM but not the page's
	// globals: it can add a stylesheet, and the page cannot reach in and remove
	// the code that added it.
	//
	// `display: none !important` rather than removing nodes, for two reasons. A
	// removed node changes the page's own DOM in ways its scripts notice, and a
	// stylesheet applies to elements that have not been created yet -- which is
	// most of them, since this runs before the page's own content.
	return QStringLiteral(R"JS(
(function () {
  // Named, not `arguments.callee`: that is a syntax error under strict mode, and
  // the injected world is not guaranteed to be sloppy.
  var sheet_el = null;

  // **Procedural rules** (`procedural_cosmetic.h`): each is steps -- a CSS
  // selector, or an operator filtering or moving the set -- and an action.
  // Run on the document as it is, then again whenever it changes, at most
  // every 150 ms: what these select is usually rendered after load. A rule
  // that throws is skipped and the rest still run.
  var pattern = function (arg) {
    var m = /^\/(.*)\/([imsu]*)$/.exec(arg);
    if (m) { try { return new RegExp(m[1], m[2]); } catch (e) { return null; } }
    return null;
  };
  var text_test = function (arg) {
    var re = pattern(arg);
    if (re) return function (s) { return re.test(s); };
    return function (s) { return s.indexOf(arg) >= 0; };
  };
  // A CSS step after an operator is relative to each element of the set:
  // descendants, `> child`, or `+`/`~` a sibling -- the last read from the
  // parent, keeping a match only if its branch follows the element.
  var relative = function (root, css) {
    var c = css.charAt(0);
    if (c !== '+' && c !== '~')
      return Array.prototype.slice.call(root.querySelectorAll(':scope ' + css));
    var parent = root.parentElement;
    if (!parent) return [];
    var found = parent.querySelectorAll(':scope > ' + css.slice(1));
    var out = [];
    for (var i = 0; i < found.length; i++) {
      var top = found[i];
      while (top.parentElement !== parent) top = top.parentElement;
      if (c === '+' ? top === root.nextElementSibling
                    : root.compareDocumentPosition(top) & 4)
        out.push(found[i]);
    }
    return out;
  };
  var unique = function (list) {
    var out = [], seen = new Set();
    for (var i = 0; i < list.length; i++)
      if (list[i] && !seen.has(list[i])) { seen.add(list[i]); out.push(list[i]); }
    return out;
  };
  // `set` null means the document: the first step selects from it.
  var run = function (steps, set) {
    for (var s = 0; s < steps.length; s++) {
      var st = steps[s], out = [], i;
      if (st.css !== undefined) {
        if (set === null)
          out = Array.prototype.slice.call(document.querySelectorAll(st.css));
        else
          for (i = 0; i < set.length; i++) out = out.concat(relative(set[i], st.css));
        set = unique(out);
        continue;
      }
      if (st.op === 'path') {
        // A gate on the page's path: the set, or the document, passes as is.
        if (text_test(st.arg)(location.pathname + location.search) === !!st.negate)
          return [];
        continue;
      }
      if (set === null) {
        if (st.op === 'xpath') set = [document];
        else set = Array.prototype.slice.call(document.querySelectorAll('*'));
      }
      var op = st.op, arg = st.arg;
      if (op === 'has-text') {
        var t = text_test(arg);
        set = set.filter(function (e) { return t(e.textContent || ''); });
      } else if (op === 'min-text-length') {
        var n = parseInt(arg, 10) || 0;
        set = set.filter(function (e) { return (e.textContent || '').length >= n; });
      } else if (op === 'upward') {
        var k = /^\d+$/.test(arg) ? parseInt(arg, 10) : 0;
        set = unique(set.map(function (e) {
          if (!k) return e.parentElement && e.parentElement.closest(arg);
          for (var j = 0; j < k && e; j++) e = e.parentElement;
          return e;
        }));
      } else if (op === 'has' || op === 'not') {
        var want = op === 'has';
        set = set.filter(function (e) { return (run(st.steps, [e]).length > 0) === want; });
      } else if (op === 'matches-css' || op === 'matches-css-before' ||
                 op === 'matches-css-after') {
        var colon = arg.indexOf(':');
        if (colon < 0) return [];
        var prop = arg.slice(0, colon).trim(), val = arg.slice(colon + 1).trim();
        var vre = pattern(val);
        var pseudo = op === 'matches-css' ? null : op === 'matches-css-before' ? '::before' : '::after';
        set = set.filter(function (e) {
          var v = getComputedStyle(e, pseudo).getPropertyValue(prop);
          return vre ? vre.test(v) : v === val;
        });
      } else if (op === 'matches-attr') {
        // A name may be a /pattern/ itself, and a pattern may hold '='.
        var eq = arg.charAt(0) === '/' ? arg.indexOf('/=', 1) : arg.indexOf('=');
        if (eq >= 0 && arg.charAt(0) === '/') eq++;
        var an = (eq < 0 ? arg : arg.slice(0, eq)).replace(/^["']|["']$/g, '');
        var av = eq < 0 ? null : arg.slice(eq + 1).replace(/^["']|["']$/g, '');
        var are = pattern(an), avre = av === null ? null : pattern(av);
        set = set.filter(function (e) {
          for (var a = 0; a < e.attributes.length; a++) {
            var at = e.attributes[a];
            if (are ? !are.test(at.name) : at.name !== an) continue;
            if (av === null || (avre ? avre.test(at.value) : at.value === av)) return true;
          }
          return false;
        });
      } else if (op === 'matches-path') {
        var p = location.pathname + location.search;
        if (!text_test(arg)(p)) return [];
      } else if (op === 'xpath') {
        for (i = 0; i < set.length; i++) {
          var r = document.evaluate(arg, set[i], null, 7, null);   // ordered snapshot
          for (var q = 0; q < r.snapshotLength; q++)
            if (r.snapshotItem(q).nodeType === 1) out.push(r.snapshotItem(q));
        }
        set = unique(out);
      } else {
        return [];   // an operator the reader knows and this does not
      }
    }
    return set || [];
  };
  var act = function (rule, e) {
    if (rule.action === 'remove') { e.remove(); return; }
    if (rule.action === 'remove-attr' || rule.action === 'remove-class') {
      var re = pattern(rule.arg);
      var names = re ? null : rule.arg.split('|');
      var match = function (n) { return re ? re.test(n) : names.indexOf(n) >= 0; };
      if (rule.action === 'remove-attr') {
        var drop = [];
        for (var a = 0; a < e.attributes.length; a++)
          if (match(e.attributes[a].name)) drop.push(e.attributes[a].name);
        drop.forEach(function (n) { e.removeAttribute(n); });
      } else {
        Array.prototype.slice.call(e.classList).forEach(function (n) {
          if (match(n)) e.classList.remove(n);
        });
      }
      return;
    }
    if (e.style.getPropertyValue('display') !== 'none' ||
        e.style.getPropertyPriority('display') !== 'important')
      e.style.setProperty('display', 'none', 'important');
  };
  var procedural = function (rules) {
    var pass = function () {
      for (var i = 0; i < rules.length; i++) {
        try {
          var found = run(rules[i].steps, null);
          for (var j = 0; j < found.length; j++)
            if (found[j] !== document) act(rules[i], found[j]);
        } catch (e) { /* one rule failing does not stop the others */ }
      }
    };
    var due = false, last = 0;
    var schedule = function () {
      if (due) return;
      due = true;
      setTimeout(function () { due = false; last = Date.now(); pass(); },
                 Math.max(0, 150 - (Date.now() - last)));
    };
    var watch = function () {
      if (!document.documentElement) return setTimeout(watch, 10);
      new MutationObserver(schedule).observe(document.documentElement,
                                             { childList: true, subtree: true });
      schedule();
    };
    watch();
  };

  var start = function () {
    if (!window.hydraChannel) return setTimeout(start, 50);
    window.hydraChannel(function (objs) {
    var bridge = objs.hydraCosmetic;
    if (!bridge) return;
    bridge.selectors_json(function (json) {
      var sel;
      try { sel = JSON.parse(json); } catch (e) { return; }
      if (!sel || !sel.length) return;
      var apply = function () {
        if (!document.documentElement) return setTimeout(apply, 10);
        // One stylesheet, not one per selector. **Unmarked**: it carried the
        // id `hydra-cosmetic`, which put this browser's name in every page's
        // DOM for any script to find -- the DOM is shared even where this
        // script's world is not. The element is held here instead, which is
        // all "a second run replaces rather than stacks" needed, since the
        // selectors arrive once per document.
        var el = sheet_el;
        if (!el || !el.isConnected) {
          el = sheet_el = document.createElement('style');
          (document.head || document.documentElement).appendChild(el);
        }
        // **One rule per selector, through the CSS parser.**
        //
        // This was `sel.join(',\n') + '{display:none !important}'`, and the
        // string was the whole stylesheet. Two things follow from that and
        // both are fixed by inserting rules instead. A selector carrying `}`
        // closed the rule and everything after it became CSS of its own
        // choosing -- `filter_list::why_selector_unsafe` refuses those now,
        // and this is the second half, so that neither check has to be the
        // only one. And in CSS a single invalid selector invalidates the
        // *entire* rule it appears in, so one malformed entry silently
        // disabled cosmetic filtering for the whole site; here the engine
        // refuses that one and keeps the rest.
        while (el.sheet && el.sheet.cssRules.length)
          el.sheet.deleteRule(0);
        for (var i = 0; i < sel.length; i++) {
          try {
            el.sheet.insertRule(sel[i] + '{display:none !important}',
                                el.sheet.cssRules.length);
          } catch (e) { /* the engine refused it; the others still apply */ }
        }
      };
      apply();
    });
    if (bridge.procedural_json) bridge.procedural_json(function (json) {
      var rules;
      try { rules = JSON.parse(json); } catch (e) { return; }
      if (rules && rules.length) procedural(rules);
    });
    });
  };
  start();
})();
)JS");
}
