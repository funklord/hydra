#include "cosmetic_filters.h"
#include "filter_list.h"

#include <QJsonArray>
#include <QLoggingCategory>
#include <QJsonDocument>

void cosmetic_filters::set_page_host(const QString &host) {
	m_host = host;
	if (qEnvironmentVariableIsSet("HYDRA_FILTER_DEBUG"))
		qWarning("cosmetic: page host is now \"%s\"", qPrintable(m_host));
}

QString cosmetic_filters::debug_state() const {
	return QStringLiteral("host=%1 selectors=%2")
	    .arg(m_host, selectors_for(m_list, m_host).join(','));
}

QStringList cosmetic_filters::selectors_for(const filter_list *list,
                                             const QString &host) {
	QStringList out;
	if (!list || host.isEmpty())
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
		if (host != r.scope && !host.endsWith("." + r.scope))
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
	for (const QString &s : selectors_for(m_list, m_host))
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
        // One stylesheet, not one per selector, and marked so a second run on
        // the same document replaces rather than stacks.
        var id = 'hydra-cosmetic';
        var el = document.getElementById(id);
        if (!el) {
          el = document.createElement('style');
          el.id = id;
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
