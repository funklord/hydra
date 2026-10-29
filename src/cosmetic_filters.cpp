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
	if (host.isEmpty())
		selectors.removeAll(selector);
	else
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
    });
  };
  start();
})();
)JS");
}
