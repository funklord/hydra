// The extractor loop against a real local model. Reports what it actually
// produced and how the gate judged it — pass or fail, the point is to measure.
#include "extractor_dialog.h"
#include "extractor_signals.h"
#include "site_extractor.h"
#include "ollama_provider.h"

#include <QApplication>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QPushButton>
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

	extractor_store store;
	const QUrl page("https://dramafren.example/watch/ep1");
	extractor_dialog dlg(&sig, &store, &prov, "dramafren.example", page);
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
	QTimer::singleShot(240000, &loop, &QEventLoop::quit);

	std::printf("sending...\n");
	button(&dlg, "Send")->click();
	loop.exec();
	if (!answered) { std::printf("no answer within the timeout\n"); return 1; }

	// What came back, and what the gate made of it.
	QPlainTextEdit *script = nullptr;
	for (QPlainTextEdit *e : dlg.findChildren<QPlainTextEdit *>())
		if (e->toPlainText().contains("function") || e->toPlainText().contains("extract"))
			script = e;
	std::printf("\n----- what the model returned -----\n%s\n-----------------------------------\n",
	             script ? qPrintable(script->toPlainText().left(1200)) : "(none)");

	QPushButton *apply = button(&dlg, "Use This");
	std::printf("gate: %s\n", apply && apply->isEnabled() ? "ACCEPTED" : "REJECTED");

	if (script) {
		const extractor_verdict v =
		    site_extractor::check(script->toPlainText(), page,
		                           sig.evidence_for("dramafren.example"));
		std::printf("verdict: usable=%d invented=%d timed_out=%d\n  %s\n",
		             v.usable, v.invented, v.timed_out, qPrintable(v.message.left(300)));
		if (v.usable)
			std::printf("  picked: %s  (kind=%s, headers=%d)\n",
			             qPrintable(v.result.url.toString().left(140)),
			             qPrintable(v.result.kind), int(v.result.headers.size()));
	}
	std::printf("done\n");
	return 0;
}
