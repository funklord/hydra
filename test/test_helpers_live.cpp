// The helper tier against a real CDN (architecture doc sec 11.5.1).
//
// Everything else about this tier is tested against an injected fake, which is
// the right way to exercise refusals and spent budgets but proves nothing about
// a live server. What is under test here is the tier itself, not a model: the
// extractor below is hand-written, so a failure is the tier's and not the
// prompt's.
//
//   test_helpers_live <evidence.json>
//
// Evidence comes from test/live/try_extract, and must be fresh -- CDN tokens
// expire in minutes, and stale evidence produces 403s that look like a broken
// allowlist.
#include "extractor_helpers.h"
#include "network_fetcher.h"
#include "site_extractor.h"
#include "extractor_dialog.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	if (argc < 2) {
		std::printf("usage: test_helpers_live <evidence.json>\n");
		return 2;
	}

	QFile f(argv[1]);
	if (!f.open(QIODevice::ReadOnly)) {
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}
	const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
	const QUrl page(root.value("page").toString());
	QList<evidence_request> ev;
	for (const QJsonValue &v : root.value("requests").toArray()) {
		const QJsonObject o = v.toObject();
		ev << evidence_request{ QUrl(o.value("url").toString()),
			                       o.value("kind").toString(),
			                       o.value("order").toInt() };
	}
	std::printf("page: %s\nevidence: %lld requests\n\n",
	             qPrintable(page.toString()), qint64(ev.size()));

	// The page's own context, which is what makes these addresses fetchable at
	// all -- naked they come back 403 (sec 11.3).
	stream_context ctx;
	ctx.referer = page.toString();
	network_fetcher net(ctx);

	helper_allowlist allow;
	allow.observe(ev);
	const int started_with = allow.size();

	helper_host host(&allow, net.as_function(), helper_budget{});
	// Ranked, because a budget is spent by order. Without this the script
	// scans the request log left to right and spends every call on stylesheets
	// and beacons before it reaches the video.
	QStringList ranked;
	for (const evidence_request &r :
	      extractor_dialog::candidates(ev, page, helper_budget{}.max_calls))
		ranked << r.url.toString();
	host.set_candidates(ranked);
	std::printf("ranked candidates: %lld\n", qint64(ranked.size()));
	for (const QString &u : ranked)
		std::printf("    %s\n", qPrintable(u.left(110)));
	std::printf("\n");

	// Deliberately generic: ask what things are, follow the one that says it is
	// a playlist, and return the first variant it names. No site knowledge, and
	// nothing that would work only on the site this was captured from.
	const QString src = R"JS(
		extract = function (page, requests) {
		  var worth = hydra.candidates();
		  for (var i = 0; i < worth.length; i++) {
		    var u = worth[i];
		    var what = hydra.head(u);
		    if (!what || what.kind !== 'hls') continue;
		    hydra.log('a playlist: ' + u);
		    var body = hydra.text(u);
		    var lines = body.split('\n');
		    for (var j = 0; j < lines.length; j++) {
		      var line = lines[j].trim();
		      if (!line || line.charAt(0) === '#') continue;
		      var base = u.replace(/[^\/]*$/, '');
		      var variant = line.indexOf('http') === 0 ? line : base + line;
		      hydra.log('following it to ' + variant);
		      return { url: variant, kind: 'hls',
		               headers: { Referer: page.url } };
		    }
		    return { url: u, kind: 'hls', headers: { Referer: page.url } };
		  }
		  return null;
		};
	)JS";

	// Blocking on this thread is fine here: the server is remote, so nothing
	// this thread owns needs to run for the reply to arrive. That is exactly
	// not true in the dialog, which is why it judges off-thread.
	const extractor_verdict v = site_extractor::check(src, page, ev, &host);

	std::printf("--- what it did ---\n");
	for (const helper_call &c : host.transcript())
		std::printf("%s%-5s %s\n        %s\n", c.allowed ? "  " : "! ",
		             qPrintable(c.verb), qPrintable(c.target.left(110)),
		             qPrintable(c.outcome));

	std::printf("\nallowlist: %d addresses at the start, %d after following\n",
	             started_with, allow.size());
	std::printf("calls: %d, bytes: %lld, breached: %s\n",
	             host.calls_used(), host.bytes_used(),
	             host.breached() ? qPrintable(host.breach()) : "no");
	std::printf("\nverdict: usable=%d invented=%d helper_breach=%d\n  %s\n",
	             v.usable, v.invented, v.helper_breach, qPrintable(v.message));
	if (v.usable)
		std::printf("  picked: %s\n", qPrintable(v.result.url.toString()));

	// The claim the tier exists to make: the answer is an address the page
	// never requested, reached by following a document that named it.
	bool was_observed = false;
	for (const evidence_request &r : ev)
		if (helper_allowlist::normalise(r.url) ==
		    helper_allowlist::normalise(v.result.url))
			was_observed = true;
	if (v.usable)
		std::printf("\n%s\n", was_observed
		  ? "the answer was already in the request log (the pure tier would "
		    "have found this too)"
		  : "the answer was NOT in the request log — it was reached by "
		    "following, which is what this tier is for");
	return v.usable ? 0 : 1;
}
