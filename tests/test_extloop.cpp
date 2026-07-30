// The review loop end to end, with a stub provider standing in for a model.
// What a real model returns is unmeasured; what the dialog does with a reply
// is not.
#include "extractor_dialog.h"
#include "extractor_signals.h"
#include "site_extractor.h"
#include "ai_provider.h"

#include <QApplication>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
#include "test_extloop.moc"
