#include "scriptlets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// **The catalog, and the whole of it.** Each name is uBlock Origin's, so a
// rule written for the ecosystem resolves here; each implementation is this
// project's own and is meant to be read.
//
// Written as ES5 on purpose: this runs in the page's world at document
// creation, before anything has established what the page supports, and a
// syntax error there would take the whole injection with it.
const char *k_catalog = R"JS(
var C = {};

// json-prune: remove named properties from anything the page parses as JSON.
//
// This is the one that reaches a video ad. A player asks its own backend for a
// configuration document and reads an array of ad placements out of it; remove
// the array before the player sees it and there is nothing to play. The ads
// come from the same hosts as the video, so no network rule can see them.
//
// `paths` is a space-separated list of dotted property paths. `needle`, when
// given, is a list of paths that must ALL be present for anything to be
// removed -- which is how a rule avoids pruning every document the page reads
// and breaking the ones it did not mean.
//
// No `*` in a path. uBlock supports a wildcard there; this does not, and a
// rule using one simply finds nothing to remove rather than matching more than
// it was asked to.
C['json-prune'] = function (paths, needle) {
	var want = String(paths || '').split(/\s+/).filter(Boolean);
	var must = String(needle || '').split(/\s+/).filter(Boolean);
	if (!want.length) return;
	var reach = function (o, p, cut) {
		var parts = String(p).split('.');
		var last = parts.pop();
		var cur = o;
		for (var i = 0; i < parts.length; i++) {
			if (cur === null || typeof cur !== 'object') return false;
			if (!(parts[i] in cur)) return false;
			cur = cur[parts[i]];
		}
		if (cur === null || typeof cur !== 'object') return false;
		if (!(last in cur)) return false;
		if (cut) delete cur[last];
		return true;
	};
	var prune = function (o) {
		if (o === null || typeof o !== 'object') return o;
		for (var i = 0; i < must.length; i++)
			if (!reach(o, must[i], false)) return o;
		for (var j = 0; j < want.length; j++) reach(o, want[j], true);
		return o;
	};
	var real = JSON.parse;
	JSON.parse = function () { return prune(real.apply(this, arguments)); };
	// And the fetch path, which is how a modern player actually asks.
	if (window.Response && window.Response.prototype &&
	     window.Response.prototype.json) {
		var realJson = window.Response.prototype.json;
		window.Response.prototype.json = function () {
			return realJson.apply(this, arguments).then(prune);
		};
	}
};

// set-constant: pin a page global to a value it then cannot change.
//
// **Only the vocabulary below, and that is narrower than uBlock's on
// purpose.** uBlock lets a rule supply an arbitrary string; here a rule may
// ask for a boolean, null, undefined, a number, an empty string or a function
// that does nothing -- the values a flag is read as. An arbitrary string would
// be the one place a filter list could put content of its own choosing into a
// page's globals, and the gain does not pay for it.
C['set-constant'] = function (path, raw) {
	if (!path) return;
	var value;
	var word = String(raw);
	if (word === 'true') value = true;
	else if (word === 'false') value = false;
	else if (word === 'null') value = null;
	else if (word === 'undefined') value = undefined;
	else if (word === 'noopFunc') value = function () {};
	else if (word === 'trueFunc') value = function () { return true; };
	else if (word === 'falseFunc') value = function () { return false; };
	else if (word === '' || word === 'emptyStr') value = '';
	else if (/^-?[0-9]+(\.[0-9]+)?$/.test(word)) value = Number(word);
	else return;

	var parts = String(path).split('.');
	var last = parts.pop();
	var owner = window;
	for (var i = 0; i < parts.length; i++) {
		if (owner[parts[i]] === undefined || owner[parts[i]] === null)
			owner[parts[i]] = {};
		owner = owner[parts[i]];
		if (typeof owner !== 'object' && typeof owner !== 'function') return;
	}
	try {
		Object.defineProperty(owner, last, {
			get: function () { return value; },
			set: function () {},
			configurable: false
		});
	} catch (e) { /* already non-configurable: the page wins, and says so */ }
};
)JS";

const QSet<QString> &catalog_names() {
	static const QSet<QString> names = {
		QStringLiteral("json-prune"),
		QStringLiteral("set-constant"),
	};
	return names;
}

// **A JS string literal, escaped here rather than hoped for.** The calls are
// emitted as JSON and read back with `JSON.parse`, so every argument is a
// string at every point -- but the JSON itself has to cross into the script as
// source, and that crossing is the only place a filter list could reach the
// parser.
//
// Which characters matter, measured rather than assumed: Qt's JSON writer
// escapes `"`, `\` and the control characters, and leaves a single quote and
// U+2028/U+2029 raw. So the load-bearing cases here are the single quote,
// which would close the literal, and the two Unicode line separators, which
// are line terminators in JS however they arrived. The backslash rule is not
// redundant either -- it keeps JSON's own `\n` intact across the crossing, so
// `JSON.parse` still sees an escape rather than a literal newline.
QString js_string(const QString &text) {
	QString out = QStringLiteral("'");
	for (const QChar c : text) {
		switch (c.unicode()) {
			case '\\': out += QStringLiteral("\\\\");   break;
			case '\'': out += QStringLiteral("\\'");    break;
			case '\n': out += QStringLiteral("\\n");    break;
			case '\r': out += QStringLiteral("\\r");    break;
			case 0x2028: out += QStringLiteral("\\u2028"); break;
			case 0x2029: out += QStringLiteral("\\u2029"); break;
			default:   out += c;
		}
	}
	return out + QStringLiteral("'");
}

}  // namespace

namespace scriptlets {

bool vetted(const QString &name) {
	return catalog_names().contains(name);
}

QStringList names() {
	QStringList out(catalog_names().cbegin(), catalog_names().cend());
	out.sort();
	return out;
}

bool parse_call(const QString &inside, scriptlet_call *out, QString *why) {
	const auto fail = [why](const char *text) {
		if (why)
			*why = QString::fromLatin1(text);
		return false;
	};
	// Commas separate, `\,` is a comma in an argument. Split by hand rather
	// than with `split(',')` for that one reason.
	QStringList parts;
	QString cur;
	for (int i = 0; i < inside.size(); ++i) {
		const QChar c = inside.at(i);
		if (c == QLatin1Char('\\') && i + 1 < inside.size() &&
		    inside.at(i + 1) == QLatin1Char(',')) {
			cur += QLatin1Char(',');
			++i;
		} else if (c == QLatin1Char(',')) {
			parts << cur.trimmed();
			cur.clear();
		} else {
			cur += c;
		}
	}
	parts << cur.trimmed();
	if (parts.isEmpty() || parts.first().isEmpty())
		return fail("names no scriptlet");
	const QString name = parts.takeFirst();
	if (!vetted(name))
		return fail("names a scriptlet this build does not implement");
	if (out) {
		out->name = name;
		out->args = parts;
	}
	return true;
}

QString source_for(const QList<scriptlet_call> &calls) {
	QJsonArray arr;
	for (const scriptlet_call &c : calls) {
		// Checked again here, not because `parse_call` is unreliable but
		// because this is the function that writes the script: a caller that
		// built a call some other way must not be the reason an unvetted name
		// runs. The catalog being closed has to be true at the point of use.
		if (!vetted(c.name))
			continue;
		QJsonObject o;
		o.insert(QStringLiteral("n"), c.name);
		// **The scope travels with the call and is matched in the page, not
		// here.** A scriptlet has to be in place before the page's own
		// scripts run, so it is injected when the view is made rather than
		// when a navigation is noticed -- by which time the document it
		// should have patched already exists. One script covers every site
		// the rules name and each frame decides whether it is one of them,
		// which is also what makes it right inside an iframe: an embedded
		// player's frame has its own hostname, and that is the one the rule
		// was written about.
		o.insert(QStringLiteral("s"), c.scope);
		QJsonArray args;
		for (const QString &a : c.args)
			args.append(a);
		o.insert(QStringLiteral("a"), args);
		arr.append(o);
	}
	if (arr.isEmpty())
		return QString();

	const QString json = QString::fromUtf8(
	  QJsonDocument(arr).toJson(QJsonDocument::Compact));
	return QStringLiteral("(function(){'use strict';")
	     + QString::fromUtf8(k_catalog)
	     + QStringLiteral("var calls;try{calls=JSON.parse(")
	     + js_string(json)
	     + QStringLiteral(");}catch(e){return;}"
	                       "var host='';try{host=String(location.hostname||'');}"
	                       "catch(e){}"
	                       "for(var i=0;i<calls.length;i++){"
	                       "var sc=calls[i].s;"
	                       // An exact host or a subdomain of it, which is the
	                       // same test `cosmetic_filters` applies to a scoped
	                       // selector. An empty scope means every frame, and
	                       // only a caller that has already matched emits one.
	                       "if(sc&&host!==sc&&"
	                       "host.slice(-(sc.length+1))!=='.'+sc)continue;"
	                       "var f=C[calls[i].n];if(!f)continue;"
	                       // One failing scriptlet must not take the others
	                       // with it: they are independent patches and a page
	                       // that defeats one says nothing about the next.
	                       "try{f.apply(null,calls[i].a);}catch(e){}}"
	                       "})();");
}

}  // namespace scriptlets
