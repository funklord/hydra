// The extractor loop against a real local model. Reports what it actually
// produced and how the gate judged it — pass or fail, the point is to measure.
#include "extractor_dialog.h"
#include "extractor_signals.h"
#include "site_extractor.h"
#include "ollama_provider.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>

static QPushButton *button(QWidget *w, const QString &t) {
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text().contains(t)) return b;
	return nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);
	const QString model = argc > 1 ? argv[1] : "qwen2.5-coder:7b";

	ollama_provider prov;
	prov.set_endpoint(QUrl("http://127.0.0.1:11434"));
	prov.set_model(model);
	std::printf("model: %s   reachable: %s\n", qPrintable(model),
	             prov.probe_now() ? "yes" : "NO");
	if (!prov.available()) return 1;

	// Evidence shaped like the site that motivated §11.5: a disguised manifest
	// among ordinary page traffic and a flood of segments.
	extractor_signals sig;
	auto feed = [&](const QString &u, resource_kind k) {
		request_context c;
		c.url = QUrl(u); c.site_host = "dramafren.example";
		c.request_host = c.url.host(); c.kind = k;
		sig.on_request(c, request_decision{});
	};
	feed("https://dramafren.example/watch/ep1", resource_kind::other);
	feed("https://dramafren.example/wp-content/themes/x/app.js", resource_kind::script);
	feed("https://cdn.example/img/poster.jpg", resource_kind::image);
	feed("https://www.googletagmanager.com/gtag/js?id=G-X", resource_kind::script);
	feed("https://sil5.player.example/v4/db/r3rqgi/cf-master.1774687168.txt?k=UCpS63&kx=17", resource_kind::other);
	for (int i = 0; i < 60; ++i)
		feed(QString("https://sil5.player.example/v4/db/r3rqgi/seg-%1.ts").arg(i, 5, 10, QChar('0')),
		      resource_kind::other);
	feed("https://mc.yandex.com/watch/98086865?wmode=7", resource_kind::script);

	// Or real evidence, captured by tests/live/try_extract from an actual page.
	// The synthetic set above is a guess at what a site does; a captured one is
	// what a site did, and the two disagree in ways that matter (the real
	// segments arrive disguised as .woff2 web fonts, not as .ts).
	QString site = "dramafren.example";
	QUrl page("https://dramafren.example/watch/ep1");
	if (argc > 2) {
		QFile f(argv[2]);
		if (!f.open(QIODevice::ReadOnly)) {
			std::printf("cannot read evidence %s\n", argv[2]);
			return 1;
		}
		const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
		page = QUrl(root.value("page").toString());
		site = root.value("host").toString();
		sig.clear_site(site);
		const QJsonArray reqs = root.value("requests").toArray();
		for (const QJsonValue &v : reqs) {
			const QJsonObject o = v.toObject();
			request_context c;
			c.url = QUrl(o.value("url").toString());
			c.site_host = site;
			c.request_host = c.url.host();
			c.kind = resource_kind::other;
			const QString k = o.value("kind").toString();
			if (k == "script")     c.kind = resource_kind::script;
			else if (k == "image") c.kind = resource_kind::image;
			sig.on_request(c, request_decision{});
		}
		std::printf("evidence: %lld requests from %s\n", qint64(reqs.size()),
		             qPrintable(site));
	}

	extractor_store store;
	extractor_dialog dlg(&sig, &store, &prov, site, page);
	dlg.show();

	QEventLoop loop;
	bool answered = false;
	QObject::connect(&prov, &ai_provider::finished, [&](const QString &) {
		answered = true; QTimer::singleShot(300, &loop, &QEventLoop::quit);
	});
	QObject::connect(&prov, &ai_provider::failed, [&](const QString &e) {
		std::printf("PROVIDER FAILED: %s\n", qPrintable(e.left(200)));
		loop.quit();
	});
	// Real evidence is a much longer payload than the synthetic set, and a 14B
	// on CPU scales with it, so the ceiling has to be movable.
	const int timeout_ms = qEnvironmentVariableIsSet("HYDRA_MODEL_TIMEOUT_MS")
	                           ? qEnvironmentVariableIntValue("HYDRA_MODEL_TIMEOUT_MS")
	                           : 240000;
	QTimer::singleShot(timeout_ms, &loop, &QEventLoop::quit);

	// Wait for Send rather than clicking the instant the dialog exists. The
	// dialog now asks the server what the candidate addresses serve before it
	// will let anything be sent, and a disabled button ignores a click in
	// silence — which presented as the model never answering, with no request
	// reaching Ollama at all.
	QPushButton *send = button(&dlg, "Send");
	QElapsedTimer waited;
	waited.start();
	while (send && !send->isEnabled() && waited.elapsed() < 60000)
		QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
	if (send && !send->isEnabled()) {
		std::printf("Send never became available (%lld ms)\n",
		             qint64(waited.elapsed()));
		return 1;
	}
	// Say how much of the payload came from asking the server. These tokens are
	// short-lived, so evidence captured an hour ago probes as 403 and the run
	// silently measures the un-annotated case instead of the annotated one.
	QPlainTextEdit *payload = dlg.findChild<QPlainTextEdit *>("payload");
	const QString shown = payload ? payload->toPlainText() : QString();
	std::printf("payload: %d chars, %d addresses answered, %d refused\n",
	             int(shown.size()), int(shown.count(QStringLiteral("   -> "))) -
	                 int(shown.count(QStringLiteral("not established"))),
	             int(shown.count(QStringLiteral("not established"))));
	// **What was actually sent**, on request. Counts say how much of the payload
	// was annotated; they cannot say *which* addresses, and that is the
	// difference between a run that measures the model and a run that measures
	// the payload. A miss with the stream absent from the payload is not the
	// model failing to find it -- that is how the second site's 0 of 5 turned
	// out to be a probe budget that never reached the media host, and reading
	// the counts alone would have blamed the model twice.
	const QByteArray dump_to = qgetenv("HYDRA_DUMP_PAYLOAD");
	if (!dump_to.isEmpty()) {
		QFile f(QString::fromLocal8Bit(dump_to));
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(shown.toUtf8());
			f.close();
			std::printf("payload written to %s\n", dump_to.constData());
		} else {
			// Said out loud rather than skipped: a dump that silently did not
			// happen looks exactly like a payload that was empty.
			std::printf("could not write the payload to %s\n", dump_to.constData());
		}
	}
	std::printf("sending after %lld ms...\n", qint64(waited.elapsed()));
	send->click();
	loop.exec();
	if (!answered) { std::printf("no answer within the timeout\n"); return 1; }

	// What came back, and what the gate made of it.
	// By name. Two earlier versions of this line found the pane by content
	// ("whichever one mentions a function", which reported "(none)" the moment a
	// reply was prose) and then by position ("the pane built second", which a
	// third pane would have silently broken). The panes carry object names now.
	QPlainTextEdit *script = dlg.findChild<QPlainTextEdit *>("proposal");
	std::printf("\n----- what the model returned -----\n%s\n-----------------------------------\n",
	             script ? qPrintable(script->toPlainText().left(1200)) : "(none)");

	QPushButton *apply = button(&dlg, "Use This");
	std::printf("gate: %s\n", apply && apply->isEnabled() ? "ACCEPTED" : "REJECTED");

	// What the *shipping path* concluded, in its own words. The line below is a
	// second, weaker judgement — `check()` with no probe results and no fetch of
	// the pick — and reading it as the verdict is a mistake this harness has
	// already caused once: it scored an analytics beacon `usable=1` while the
	// dialog beside it had refused that pick, because the content-type tier had
	// fetched it and found no stream. `gate:` above and this are the answer;
	// `check-only:` is a diagnostic about one rule set.
	if (QLabel *verdict = dlg.findChild<QLabel *>("verdict"))
		std::printf("dialog: %s\n",
		             qPrintable(verdict->text().replace(QRegularExpression("<[^>]*>"),
		                                                 " ").simplified().left(300)));

	if (script) {
		const extractor_verdict v =
		    site_extractor::check(script->toPlainText(), page,
		                           sig.evidence_for(site));
		std::printf("check-only: usable=%d invented=%d timed_out=%d\n  %s\n",
		             v.usable, v.invented, v.timed_out, qPrintable(v.message.left(300)));
		if (v.usable)
			std::printf("  picked: %s  (kind=%s, headers=%d)\n",
			             qPrintable(v.result.url.toString().left(140)),
			             qPrintable(v.result.kind), int(v.result.headers.size()));
	}
	std::printf("done\n");
	return 0;
}
