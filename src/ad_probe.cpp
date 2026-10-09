#include "ad_probe.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// **What it looks for, and why each is there.**
//
// Players first, because a player that says it is in an ad is the one
// finding that is not a guess. Each names its state in its own classes:
// YouTube's `ad-showing` and `ad-interrupting` while one plays, and
// `ad-created` alone when one has been set up for the video -- measured on the
// second video of a session, where the holder saw a pre-roll and that was the
// only class present -- Video.js's
// `vjs-ad-playing`, JW Player's `jw-flag-ads`. Google's IMA SDK, which most
// other players use for video ads, draws into a container of its own, and a
// visible one means an ad is up.
//
// Elements by name, where an id or class TOKEN is an ad word. Tokens, split on
// `-` and `_`, rather than substrings: `ad` as a substring is in `header`,
// `shadow`, `download` and `read-more`, and a probe that finds an ad in every
// page's header is one nobody reads twice. Visible and at least 20 pixels each
// way, and only the outermost of a nested run, since an ad wrapper and its
// three inner divs are one ad.
//
// Frames from hosts that serve ads, by name. A short list of the ones that
// carry most display advertising, not a filter list: this answers "is there
// one", and the subscriptions answer "block it".
//
// Labels: short visible text that says what it is -- "Ad", "Sponsored",
// "Skip ad", and the same in the languages this browser's holder reads.
// Matched whole, so "Ads" is a label and "Adsorption" is not.
const char *k_script = R"JS(
(function () {
	var out = { players: [], elements: [], frames: [], labels: [] };
	var MAX_NODES = 20000, MAX_EACH = 12;
	var seen = 0;
	var visible = function (el) {
		try {
			var r = el.getBoundingClientRect();
			if (r.width < 2 || r.height < 2) return null;
			var s = window.getComputedStyle(el);
			if (s.display === 'none' || s.visibility === 'hidden' ||
			    parseFloat(s.opacity) === 0) return null;
			return r;
		} catch (e) { return null; }
	};
	var esc = function (t) {
		try { if (window.CSS && CSS.escape) return CSS.escape(t); } catch (e) {}
		return String(t).replace(/[^\w-]/g, '\\$&');
	};
	var classes = function (el) {
		var c = el.getAttribute && el.getAttribute('class');
		return c ? String(c).split(/\s+/).filter(Boolean) : [];
	};
	var selector = function (el) {
		var s = String(el.tagName || '').toLowerCase();
		if (el.id) return s + '#' + esc(el.id);
		var c = classes(el).slice(0, 3);
		for (var i = 0; i < c.length; i++) s += '.' + esc(c[i]);
		return s;
	};
	var size = function (r) { return Math.round(r.width) + 'x' + Math.round(r.height); };

	var players = [
		['youtube',  '#movie_player, .html5-video-player', /^ad-(created|showing|interrupting)$/],
		['video.js', '.video-js',                          /^vjs-ad-(playing|loading)$/],
		['jwplayer', '.jwplayer, .jw-wrapper',             /^jw-flag-ads(-.*)?$/]
	];
	var is_player = [];
	for (var p = 0; p < players.length; p++) {
		var found = [];
		try { found = document.querySelectorAll(players[p][1]); } catch (e) {}
		for (var j = 0; j < found.length && out.players.length < MAX_EACH; j++) {
			is_player.push(found[j]);
			var hits = classes(found[j]).filter(function (c) {
				return players[p][2].test(c);
			});
			if (hits.length)
				out.players.push({ kind: players[p][0], at: selector(found[j]),
				                   state: hits.join(' ') });
		}
	}
	var ima = [];
	try { ima = document.querySelectorAll('.ima-ad-container, [id^="ima-ad"]'); } catch (e) {}
	for (var k = 0; k < ima.length && out.players.length < MAX_EACH; k++) {
		var ir = visible(ima[k]);
		if (ir && ima[k].children.length)
			out.players.push({ kind: 'ima', at: selector(ima[k]),
			                   state: 'ad container visible, ' + size(ir) });
	}

	var words = { ad: 1, ads: 1, advert: 1, adverts: 1, advertisement: 1,
	              advertising: 1, sponsor: 1, sponsored: 1, adslot: 1,
	              adunit: 1, adsbygoogle: 1, adbox: 1, adbanner: 1,
	              banner_ad: 1, dfp: 1, annons: 1, reklam: 1 };
	var ad_named = function (el) {
		var names = classes(el);
		if (el.id) names.push(el.id);
		for (var i = 0; i < names.length; i++) {
			var low = String(names[i]).toLowerCase();
			if (words[low]) return names[i];
			var parts = low.split(/[-_]/);
			for (var j = 0; j < parts.length; j++)
				if (words[parts[j]]) return names[i];
		}
		return null;
	};
	var reported = [];
	var inside_reported = function (el) {
		for (var i = 0; i < reported.length; i++)
			if (reported[i] !== el && reported[i].contains(el)) return true;
		return false;
	};
	var all = [];
	try { all = document.querySelectorAll('[class], [id]'); } catch (e) {}
	for (var n = 0; n < all.length && n < MAX_NODES; n++) {
		if (out.elements.length >= MAX_EACH) break;
		var el = all[n];
		// A player's ad classes are its state, read above; as a name they
		// would report the player itself as an ad element.
		if (is_player.indexOf(el) >= 0) continue;
		var why = ad_named(el);
		if (!why || inside_reported(el)) continue;
		var r = visible(el);
		if (!r || r.width < 20 || r.height < 20) continue;
		reported.push(el);
		out.elements.push({ at: selector(el), name: String(why), size: size(r) });
	}
	seen += Math.min(all.length, MAX_NODES);

	var hosts = /(^|\.)(doubleclick\.net|googlesyndication\.com|googleadservices\.com|adservice\.google\.[a-z.]+|amazon-adsystem\.com|adnxs\.com|criteo\.(com|net)|taboola\.com|outbrain\.com|pubmatic\.com|rubiconproject\.com|imasdk\.googleapis\.com|moatads\.com|adsrvr\.org|teads\.tv)$/i;
	var frames = [];
	try { frames = document.querySelectorAll('iframe'); } catch (e) {}
	for (var f = 0; f < frames.length && out.frames.length < MAX_EACH; f++) {
		var host = '';
		try { host = new URL(frames[f].src, location.href).hostname; } catch (e) {}
		if (!host || !hosts.test(host)) continue;
		var fr = visible(frames[f]);
		if (fr) out.frames.push({ host: host, at: selector(frames[f]), size: size(fr) });
	}

	var label = /^(ad|ads|advertisement|sponsored|sponsored content|promoted|skip ads?|ad \d+ of \d+|annons|reklam|anzeige|werbung)\b[\s:\u00b7\u2022\d]*$/i;
	try {
		var walk = document.createTreeWalker(document.body || document,
		                                     NodeFilter.SHOW_TEXT, null, false);
		var t, steps = 0;
		while ((t = walk.nextNode()) && steps++ < MAX_NODES &&
		       out.labels.length < MAX_EACH) {
			var text = String(t.nodeValue || '').replace(/\s+/g, ' ').trim();
			if (!text || text.length > 40 || !label.test(text)) continue;
			var holder = t.parentElement;
			if (!holder || !visible(holder)) continue;
			out.labels.push({ text: text, at: selector(holder) });
		}
	} catch (e) {}

	out.scanned = seen;
	return JSON.stringify(out);
})()
)JS";

QStringList strings(const QJsonArray &a,
                     QString (*line)(const QJsonObject &)) {
	QStringList out;
	for (const QJsonValue &v : a)
		if (v.isObject())
			out << line(v.toObject());
	return out;
}

QString s(const QJsonObject &o, const char *k) {
	return o.value(QLatin1String(k)).toString();
}

}  // namespace

namespace ad_probe {

QString source() {
	return QString::fromUtf8(k_script);
}

bool parse(const QString &json, findings *out) {
	const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
	if (!doc.isObject())
		return false;
	const QJsonObject o = doc.object();
	if (!o.contains(QLatin1String("players")))
		return false;
	findings f;
	f.players = strings(o.value("players").toArray(), [](const QJsonObject &p) {
		return QString("%1 player %2 says it is in an ad (%3)")
		    .arg(s(p, "kind"), s(p, "at"), s(p, "state"));
	});
	f.elements = strings(o.value("elements").toArray(), [](const QJsonObject &e) {
		return QString("%1, %2, named \"%3\"")
		    .arg(s(e, "at"), s(e, "size"), s(e, "name"));
	});
	f.frames = strings(o.value("frames").toArray(), [](const QJsonObject &e) {
		return QString("frame from %1 at %2, %3")
		    .arg(s(e, "host"), s(e, "at"), s(e, "size"));
	});
	f.labels = strings(o.value("labels").toArray(), [](const QJsonObject &e) {
		return QString("\"%1\" shown at %2").arg(s(e, "text"), s(e, "at"));
	});
	if (out)
		*out = f;
	return true;
}

QStringList describe(const findings &f) {
	QStringList out;
	for (const QString &p : f.players)
		out << "player: " + p;
	for (const QString &l : f.labels)
		out << "label: " + l;
	for (const QString &fr : f.frames)
		out << "frame: " + fr;
	for (const QString &e : f.elements)
		out << "element: " + e;
	return out;
}

}  // namespace ad_probe
