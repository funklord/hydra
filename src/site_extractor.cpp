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
#include <QUrlQuery>
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
// And `const extract = …` is the third form, which the arrangement above
// rejected outright: a `var extract;` in the same scope as a `const extract`
// is "Identifier extract has already been declared", a SyntaxError raised
// before a line of the proposal runs. Measured, and it cost everything — five
// runs in five against real evidence died there, and the message names our
// wrapper's variable rather than anything the model did, so it reads like the
// model produced nonsense when it had in fact produced a correct parser.
//
// The source therefore gets a scope of its own. A declaration of any kind
// (`function`, `var`, `let`, `const`) binds inside it and is handed back; a
// bare `extract = …` with no declaration finds the outer `var` and assigns
// that instead of leaking a global, which is what the outer one is still for.
QString wrap(const QString &source) {
	return QStringLiteral(
	    "(function(){\n"
	    "  var extract;\n"
	    "  var inner = (function(){\n"
	    "    %1\n"
	    "    ;\n"
	    "    return typeof extract !== 'undefined' ? extract : undefined;\n"
	    "  })();\n"
	    "  if (typeof inner === 'function') return inner;\n"
	    "  if (typeof extract === 'function') return extract;\n"
	    "  throw new Error('the script defines no extract() function');\n"
	    "})()").arg(source);
}

// Does the proposal carry a value that only this page load has?
//
// The rotating parts of these addresses are the query values and the long digit
// runs in the path — `?k=4_Lxg1uYRS4SPCO4a_CE8A&kx=1785787643`, or the
// `1742380998` in `cf-master.1742380998.txt`. `shape_of` already treats exactly
// those as the variable parts, so this asks the same question of the *script*:
// if a run of characters appears both in the evidence's variable parts and
// verbatim in the source, the script is answering from this capture rather than
// from the shape of the address.
//
// Measured: a model wrote a lookup table of the five annotated urls, tokens and
// all, and searched it. Every check the gate had passed — the address really
// was requested, really was a manifest, really was fetched once — and the
// extractor would have failed on the next visit, stored, with nothing pointing
// back here.
//
// Short values are ignored deliberately. `k=1` or a two-digit number is not a
// token and appears in ordinary code, so the floor is set where a coincidence
// stops being plausible.
bool embeds_a_token(const QString &source, const QList<evidence_request> &evidence) {
	static const QRegularExpression long_digits("[0-9]{6,}");
	QSet<QString> variable;
	for (const evidence_request &r : evidence) {
		const QUrlQuery q(r.url);
		for (const auto &kv : q.queryItems())
			if (kv.second.size() >= 8)
				variable.insert(kv.second);
		auto it = long_digits.globalMatch(r.url.path());
		while (it.hasNext())
			variable.insert(it.next().captured(0));
	}
	for (const QString &token : variable)
		if (source.contains(token))
			return true;
	return false;
}

// Does the proposal match on the position of a request in this visit's list?
//
// `order` is a real field, so this compiles, runs, and returns the right answer
// on this evidence — and it is this capture's ordering, not a property of the
// site. One extra advert, one request that lost a race, and the numbers shift.
//
// Anticipated rather than measured, and worth saying which: the notes moved out
// of the rows and into a legend keyed by order number precisely so the model
// would stop reaching for a field, and `order` is the field that legend hands
// it. The two arrangements before this one each fixed a symptom and produced
// the next, so this is the next one, guarded before it can be accepted and
// stored.
bool matches_on_order(const QString &source) {
	// Either side of the comparison, and the thing being indexed can be any
	// ordinary expression — `x.order === 3`, but also `3 === r[i].order`, which
	// the first version of this missed because it only allowed a bare
	// identifier before `.order` and real code writes `r[i]`.
	static const QRegularExpression compare(
	    R"((\.\s*order\s*[=!]=+\s*[0-9]+)|([0-9]+\s*[=!]=+\s*[\w$\.\[\]'"]*\.\s*order\b))");
	return compare.match(source).hasMatch();
}

// Does the proposal try to read the served-type note at run time?
//
// It is a column of the table the model is shown, never a field of the requests
// the script receives — it cannot be, because a stored extractor runs on later
// visits where nothing has been fetched to ask. A script reading it gets
// `undefined` every time, so it reports "found nothing" and looks like a model
// that could not find the stream, when it is a model that found it and then
// asked the wrong object about it.
//
// Measured: with the note moved into its own column, every run stopped testing
// urls for `->` -- and four in five started reading `request.serves` instead.
// The layout fixed how the note was *read*; it did not fix where the model
// thought the note lived.
bool reads_serves(const QString &source) {
	static const QRegularExpression access(
	    R"((\.\s*serves\b)|(\[\s*['"]serves['"]\s*\]))");
	return access.match(source).hasMatch();
}

QString normalise(const QUrl &u) {
	// Compared as text, but with the pieces that vary between two sightings of
	// the same request removed. A fragment never reaches the server at all.
	return u.adjusted(QUrl::RemoveFragment | QUrl::StripTrailingSlash).toString();
}

// Where an address lives: scheme, host and the path up to its last slash, with
// no query. The query is what rotates per request, so keeping it would mean two
// segments from the same folder never shared a directory.
QString directory_of(const QUrl &u) {
	QString path = u.path();
	const int slash = path.lastIndexOf('/');
	if (slash >= 0)
		path.truncate(slash + 1);
	return u.scheme() + "://" + u.host() + path;
}

}  // namespace

QString shape_of(const QUrl &u) {
	static const QRegularExpression digits("[0-9]+");
	// Query keys without their values: `?k=abc&kx=123` and `?k=def&kx=456` are
	// the same question asked twice, and on a real CDN every segment carries a
	// fresh token, so keeping the values means nothing ever matches anything.
	QUrl bare = u;
	const QUrlQuery q(u);
	QStringList keys;
	for (const auto &pair : q.queryItems())
		keys << pair.first;
	bare.setQuery(QString());
	// Long random-looking path tokens, before anything else touches them. A
	// tracker that puts its payload in the *path* -- `/sbx/b/3NIK470KmUnKT…` --
	// produces a different string every time, so digit-collapsing alone left
	// every beacon its own shape. Three consequences, all measured on the second
	// site: the flood was invisible, so the segment rule could not refuse one;
	// the host looked like a page of one-offs rather than a firehose; and a
	// model picked one and the gate accepted it as a video.
	//
	// Only tokens that are long *and* mix letters with digits, so ordinary path
	// words are left alone -- `SubtitleManager` keeps its name, a 32-character
	// hex hash does not.
	// Note the charset: no `.` and no `-`, so a token breaks at them. With them
	// included `cf-master.1774687168.txt` was one 24-character mixed token and
	// collapsed whole, which erased the very shape a manifest is recognised by
	// -- caught by the suite, which is what it is for.
	static const QRegularExpression token("[A-Za-z0-9_*~%]{16,}");
	QString path = bare.path();
	QString folded;
	int last = 0;
	auto it = token.globalMatch(path);
	while (it.hasNext()) {
		const auto m = it.next();
		const QString t = m.captured();
		const bool has_digit = t.contains(QRegularExpression("[0-9]"));
		const bool has_alpha = t.contains(QRegularExpression("[A-Za-z]"));
		if (!has_digit || !has_alpha)
			continue;
		folded += path.mid(last, m.capturedStart() - last) + "@";
		last = m.capturedEnd();
	}
	folded += path.mid(last);
	bare.setPath(folded);
	QString s = bare.toString(QUrl::RemoveFragment);
	if (!keys.isEmpty())
		s += "?" + keys.join('&');
	// Then digit runs of any length: a flood indexed `seg-1 … seg-9` is the
	// same flood as one indexed `seg-0001 … seg-0009`, and requiring two
	// digits meant the first kind never folded at all.
	s.replace(digits, "#");
	return s;
}

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
                         helper_host *helpers, const QSet<QString> *manifests) {
	extractor_verdict v;

	// --- Read before it is run ------------------------------------------
	//
	// Both of these produce a *usable-looking* answer on this evidence and a
	// broken extractor on the next visit, which is the worst shape a defect can
	// have here: the proposal is accepted, stored for the host, and fails later
	// with nothing to connect it back to this moment. They are checked
	// statically because running the script reports each as something else — a
	// baked-in token looks like a correct answer, and a read of a column that
	// does not exist looks like "the script found nothing".
	//
	// This is §12.4's argument once more: decide what a proposal *would* do
	// before letting it do it.
	if (reads_serves(source)) {
		v.reads_note = true;
		v.message = "Rejected: the script reads a `serves` value at run time. "
		            "That is a column of the evidence you were shown, not a "
		            "field of a request — it is undefined here, and there is "
		            "nothing to probe on a later visit. Decide from it now and "
		            "match the address.";
		return v;
	}

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

	// Asked after "was this even requested", because a script that returns an
	// address the page never fetched has a more basic problem than a stale
	// token, and saying so names the right fault. Everything reaching here
	// returned something real -- which is precisely what makes a baked-in token
	// dangerous, since every other check is satisfied.
	if (embeds_a_token(source, evidence)) {
		v.hardcoded = true;
		v.message = "Rejected: the script has this visit's ids or tokens written "
		            "into it, so it answers for this page load and no other. "
		            "Match a stable part of the address instead.";
		return v;
	}

	if (matches_on_order(source)) {
		v.hardcoded = true;
		v.message = "Rejected: the script matches on `order`, which is where a "
		            "request happened to fall in this visit's list. One advert "
		            "more or one race lost and it is a different number. Match a "
		            "stable part of the address instead.";
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

	// A fifth rule, and the last wrong answer the whole apparatus still accepted.
	// Measured: given evidence whose content types were annotated, three runs in
	// five returned `init-f1-v1-a1.woff` — the initialisation segment. Every
	// existing rule says yes. It is fetched exactly once, so the segment rule
	// does not fire; it is not the page and not furniture; and when the tier
	// fetches it the body genuinely is an ISO-BMFF stream, so the probe confirms
	// it. On its own it decodes to nothing.
	//
	// What settles it is the thing the tier already established: a manifest was
	// found on that host, and the parts it describes sit beside it. So a pick
	// from the manifest's own directory that is not itself a confirmed manifest
	// is a piece of that stream rather than an alternative to it. A media
	// playlist listed alongside a master is in the set and passes, which is
	// right — it is a manifest, and following it is the helper tier's job.
	//
	// Same *directory*, not same host, and the narrowness is deliberate: a
	// progressive mp4 served elsewhere on the same host is a better answer than
	// the manifest, not a piece of it, and refusing that would be a rule doing
	// harm. Missing a piece kept in a subdirectory is the failure this prefers.
	if (manifests && !manifests->isEmpty() &&
	     !manifests->contains(normalise(v.result.url))) {
		const QString dir = directory_of(v.result.url);
		// Named in the order the page fetched them, not in the set's. A player
		// asks for the master and then for the variant it chose, so the earlier
		// sighting is the one to send someone back to — and iterating a QSet gave
		// whichever the hash happened to yield, which is a user-facing sentence
		// that changes between runs for no reason.
		QString named;
		for (const evidence_request &r : evidence) {
			const QString n = normalise(r.url);
			if (!manifests->contains(n) || directory_of(r.url) != dir)
				continue;
			named = r.url.fileName();
			break;
		}
		// A confirmed manifest the page never requested — one the helper tier
		// followed to — is not in the evidence to be ordered. Falling back keeps
		// the rule firing; only the sentence loses its ordering.
		if (named.isEmpty())
			for (const QString &m : *manifests)
				if (directory_of(QUrl(m)) == dir) { named = QUrl(m).fileName(); break; }
		if (!named.isEmpty()) {
			v.is_piece = true;
			v.message = QString("Rejected: that is one of the parts the stream at "
			                     "%1 is made of, and the server confirmed that "
			                     "address is the playlist. Return the playlist.")
			                .arg(named.left(80));
			return v;
		}
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
