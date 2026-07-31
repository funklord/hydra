// SPDX-License-Identifier: GPL-3.0-or-later
#include "site_extractor.h"
#include "extractor_helpers.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSValueIterator>
#include <QQmlEngine>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QScopeGuard>

#include <atomic>
#include <chrono>
#include <thread>

namespace site_extractor {
namespace {

// The script is wrapped rather than trusted to define anything at top level:
// this way a proposal that is a bare expression, a function declaration or an
// assignment all work, and none of them can leave anything behind.
//
// `var extract;` rather than `var extract = null;`, and the difference is not
// cosmetic. A function *declaration* hoists to the top of this scope, and an
// initialiser here then runs afterwards and overwrites it with null — so
// `function extract(page, requests) { … }`, which is both valid and the most
// natural way to write it, was rejected as "defines no extract() function".
// A declaration with no initialiser does not disturb the hoisted binding, so
// both forms now arrive intact. A real model wrote the broken-by-us form on
// its first properly-formatted answer against real evidence.
QString wrap(const QString &source) {
	return QStringLiteral(
	    "(function(){\n"
	    "  var extract;\n"
	    "  %1\n"
	    "  if (typeof extract !== 'function')\n"
	    "    throw new Error('the script defines no extract() function');\n"
	    "  return extract;\n"
	    "})()").arg(source);
}

// Two requests differing only in a run of digits are the same request repeated.
QString shape_of(const QUrl &u) {
	static const QRegularExpression digits("[0-9]{2,}");
	QString s = u.toString();
	s.replace(digits, "#");
	return s;
}

QString normalise(const QUrl &u) {
	// Compared as text, but with the pieces that vary between two sightings of
	// the same request removed. A fragment never reaches the server at all.
	return u.adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash).toString();
}

}  // namespace

extraction run(const QString &source, const QUrl &page,
                const QList<evidence_request> &evidence, int timeout_ms,
                helper_host *helpers) {
	extraction out;
	QJSEngine engine;
	engine.installExtensions(QJSEngine::ConsoleExtension);

	// The §11.5.1 tier, and only when one was supplied. Without it there is no
	// `hydra` in scope at all — a pure-tier script cannot discover the surface
	// exists, let alone use it, which keeps the default the empty sandbox it
	// has always been.
	if (helpers) {
		engine.globalObject().setProperty("hydra", engine.newQObject(helpers));
		QQmlEngine::setObjectOwnership(helpers, QQmlEngine::CppOwnership);
		helpers->begin();
	}

	// A script that never returns must not be able to hang the browser. The
	// interrupt is set from another thread because a tight loop in JS never
	// yields to this one — a timer here would simply never fire.
	std::atomic<bool> done{false};
	std::thread watchdog([&engine, &done, timeout_ms] {
		QElapsedTimer t;
		t.start();
		while (!done.load()) {
			if (t.elapsed() > timeout_ms) {
				engine.setInterrupted(true);
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	});
	const auto finish = qScopeGuard([&done, &watchdog] {
		done.store(true);
		watchdog.join();
	});

	const QJSValue fn = engine.evaluate(wrap(source), "extractor.js");
	if (fn.isError()) {
		out.error = "script error: " + fn.toString();
		return out;
	}
	if (!fn.isCallable()) {
		out.error = "the script does not evaluate to a function";
		return out;
	}

	QJSValue page_obj = engine.newObject();
	page_obj.setProperty("url", page.toString());
	page_obj.setProperty("host", page.host());

	QJSValue list = engine.newArray(uint(evidence.size()));
	for (int i = 0; i < evidence.size(); ++i) {
		QJSValue r = engine.newObject();
		r.setProperty("url", evidence[i].url.toString());
		// `type`, not `kind`. What the browser fetched this as ("script",
		// "image", "other") and what a stream is ("hls", "dash", "direct") are
		// two vocabularies, and they shared the name `kind` until a model read
		// it the obvious way and wrote `request.kind === 'hls'` — never true of
		// anything, so it returned null and looked like a model failure. The
		// return value keeps `kind`, which is the one the proposal decides.
		r.setProperty("type", evidence[i].kind);
		r.setProperty("order", evidence[i].order);
		list.setProperty(uint(i), r);
	}

	const QJSValue res = fn.call({ page_obj, list });
	if (engine.isInterrupted()) {
		out.error = "the script did not finish in time";
		return out;
	}
	if (res.isError()) {
		out.error = "the script threw: " + res.toString();
		return out;
	}
	if (res.isNull() || res.isUndefined()) {
		out.error = "the script found nothing";
		return out;
	}
	if (!res.isObject()) {
		out.error = "the script returned something that is not an object";
		return out;
	}

	out.url  = QUrl(res.property("url").toString());
	out.kind = res.property("kind").toString();
	const QJSValue hs = res.property("headers");
	if (hs.isObject()) {
		QJSValueIterator it(hs);
		while (it.hasNext()) {
			it.next();
			out.headers.insert(it.name(), it.value().toString());
		}
	}
	if (!out.url.isValid() || out.url.isEmpty()) {
		out.error = "the script returned no usable URL";
		return out;
	}
	if (out.kind.isEmpty())
		out.kind = "direct";
	out.ok = true;
	return out;
}

extractor_verdict check(const QString &source, const QUrl &page,
                         const QList<evidence_request> &evidence,
                         helper_host *helpers) {
	extractor_verdict v;
	v.result = run(source, page, evidence, 2000, helpers);

	// A script that reached past what it was allowed, or spent a budget, does
	// not get to return an answer anyway. Checked before the answer is even
	// looked at, because the misbehaviour is the finding.
	if (helpers && helpers->breached()) {
		v.helper_breach = true;
		v.message = "Rejected: the script " + helpers->breach() + ".";
		return v;
	}

	if (!v.result.ok) {
		v.timed_out = v.result.error.contains("in time");
		v.message   = v.result.error;
		return v;
	}

	// The gate. §9.4 rejects a reorganization that invents a tab id because
	// there is no safe repair for one; the same holds here. A proposal is
	// choosing among addresses the page actually fetched, and one that returns
	// something else has authored a destination of its own.
	QSet<QString> seen;
	for (const evidence_request &r : evidence)
		seen.insert(normalise(r.url));

	// Observed, or reachable by following what a fetched document named. With
	// no helper tier these are the same set; with one, the second is the point —
	// a variant listed inside a master playlist was never requested by the page
	// and is still not invented.
	const bool followable =
		helpers && helpers->allowlist() && helpers->allowlist()->allows(v.result.url);
	if (!seen.contains(normalise(v.result.url)) && !followable) {
		v.invented = true;
		v.message  = "Rejected: the script returned a URL this page never "
		             "requested (" + v.result.url.toString().left(120) + ").";
		return v;
	}

	// Observed is not the same as correct in the other direction too, and a real
	// model showed this one as well: asked for the stream in a page it returned
	// the page's own address, with kind 'direct'. The document is the most
	// certainly-observed request there is, so the rule above waves it through,
	// and the media list would then offer the HTML as though it were a video.
	// The page is what the question is *about*; it is never the answer.
	if (normalise(v.result.url) == normalise(page)) {
		v.is_page = true;
		v.message = "Rejected: that is the page's own address, not a stream "
		            "inside it.";
		return v;
	}

	// Observed is not the same as correct, and a real model showed why: asked
	// for a manifest it returned `seg-00000.ts`, which the page had genuinely
	// requested, so the observed-URL rule waved it through. A stream that
	// arrives as hundreds of near-identical requests is a *segment* — the
	// manifest is fetched once. Anything picked out of that flood is refused.
	QHash<QString, int> shape_count;
	for (const evidence_request &r : evidence)
		shape_count[shape_of(r.url)]++;
	if (shape_count.value(shape_of(v.result.url)) > 2) {
		v.is_segment = true;
		v.message = QString("Rejected: that address is one of %1 near-identical "
		                     "requests, which makes it a segment rather than the "
		                     "stream.")
		                .arg(shape_count.value(shape_of(v.result.url)));
		return v;
	}

	// The interceptor already knows what the browser asked for each address as,
	// and the gate was throwing that away. A real run picked a Yandex
	// cookie-sync pixel and was accepted: genuinely requested, fetched once,
	// not the page, so nothing else refused it — and the media list would have
	// offered a tracking pixel as a video. What the browser fetched as an image
	// or a script is page furniture.
	//
	// Judged on *every* sighting, not the first. The same address fetched both
	// as an image and as something else is not decided by which came first, and
	// only an address that was never anything but furniture is refused here.
	int furniture = 0, sightings = 0;
	QString as;
	for (const evidence_request &r : evidence) {
		if (normalise(r.url) != normalise(v.result.url))
			continue;
		++sightings;
		if (r.kind == "image" || r.kind == "script") {
			++furniture;
			as = r.kind;
		}
	}
	if (sightings > 0 && furniture == sightings) {
		v.is_asset = true;
		v.message = QString("Rejected: the browser fetched that as a %1, which "
		                     "makes it part of the page rather than a stream in "
		                     "it.")
		                .arg(as);
		return v;
	}

	static const QSet<QString> kinds = { "hls", "dash", "direct" };
	if (!kinds.contains(v.result.kind)) {
		v.message = QString("Rejected: \"%1\" is not a kind this can act on.")
		                .arg(v.result.kind.left(40));
		return v;
	}

	v.usable = true;
	// Accurate about *which* rule it passed. With the helper tier an accepted
	// address may be one the page never requested, reached by following a
	// document that named it, and saying "the page really requested" would be a
	// plain falsehood in the one place the user is deciding whether to trust it.
	const bool observed_directly = seen.contains(normalise(v.result.url));
	v.message = observed_directly
		? QString("Picks a %1 stream the page really requested.").arg(v.result.kind)
		: QString("Picks a %1 stream reached by following a document the page "
		           "requested.").arg(v.result.kind);
	return v;
}

}  // namespace site_extractor

// ---------------------------------------------------------------- store ----

void extractor_store::set_for(const QString &host, const QString &source,
                               const QString &note) {
	if (host.isEmpty() || source.isEmpty())
		return;
	m_by_host.insert(host, { source, note });
}

QString extractor_store::source_for(const QString &host) const {
	return m_by_host.value(host).source;
}

QString extractor_store::note_for(const QString &host) const {
	return m_by_host.value(host).note;
}

bool extractor_store::has(const QString &host) const {
	return m_by_host.contains(host);
}

void extractor_store::remove(const QString &host) { m_by_host.remove(host); }

QStringList extractor_store::hosts() const {
	QStringList out = m_by_host.keys();
	out.sort();
	return out;
}

bool extractor_store::load(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return false;
	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	m_by_host.clear();
	for (auto it = root.begin(); it != root.end(); ++it) {
		const QJsonObject o = it.value().toObject();
		entry e;
		e.source = o.value("source").toString();
		e.note   = o.value("note").toString();
		if (!e.source.isEmpty())
			m_by_host.insert(it.key(), e);
	}
	return true;
}

bool extractor_store::save(const QString &path) const {
	QJsonObject root;
	for (auto it = m_by_host.cbegin(); it != m_by_host.cend(); ++it) {
		QJsonObject o;
		o.insert("source", it.value().source);
		o.insert("note", it.value().note);
		root.insert(it.key(), o);
	}
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return true;
}
