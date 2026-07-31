// A full turn of the extractor loop on evidence from a live page, rather than
// from the recorded set every other measurement in this project has used.
//
// It does one thing and writes it down: drive the real shell to a real watch
// page, press play, and dump what the interceptor saw as JSON. The proposal
// step is deliberately *not* here — evidence captured once and replayed offline
// is repeatable and costs the site nothing, and it is the second evidence set
// project.md says the prompt work needs before it can mean anything.
//
//   try_extract <url> [seconds-to-watch] [out.json]
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "extractor_signals.h"
#include "extractor_dialog.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QWebEnginePage>
#include <functional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>
#include <QTreeView>
#include <QWebEngineView>
#include <cstdio>

static void click_at(main_window &w, QPoint at) {
	const auto vs = w.findChildren<QWebEngineView *>();
	if (vs.isEmpty()) return;
	QWidget *t = vs.last()->focusProxy() ? vs.last()->focusProxy() : vs.last();
	if (at.isNull()) at = QPoint(t->width() / 2, t->height() / 2);
	QMouseEvent p(QEvent::MouseButtonPress, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent r(QEvent::MouseButtonRelease, at, t->mapToGlobal(at),
	               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(t, &p);
	QApplication::sendEvent(t, &r);
}

// The player is an iframe well below the fold, so a click at the middle of the
// viewport lands on its top edge or on the page behind it. Scroll it to the
// centre and report where it ended up, in viewport coordinates.
static void centre_player(main_window &w, std::function<void(QPoint)> then) {
	const auto vs = w.findChildren<QWebEngineView *>();
	if (vs.isEmpty()) { then(QPoint()); return; }
	static const char *js =
		"(function(){"
		"  var f = document.querySelector('iframe');"
		"  if (!f) return '';"
		"  f.scrollIntoView({block:'center'});"
		"  var r = f.getBoundingClientRect();"
		"  return Math.round(r.left + r.width/2) + ',' +"
		"         Math.round(r.top + r.height/2);"
		"})()";
	vs.last()->page()->runJavaScript(QString::fromLatin1(js),
	                                  [then](const QVariant &v) {
		const QStringList parts = v.toString().split(',');
		if (parts.size() != 2) { then(QPoint()); return; }
		then(QPoint(parts[0].toInt(), parts[1].toInt()));
	});
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	if (argc < 2) {
		std::printf("usage: try_extract <url> [seconds] [out.json]\n");
		return 2;
	}
	const QString target = argv[1];
	const int watch_s = argc > 2 ? QString(argv[2]).toInt() : 40;
	const QString out_path =
		argc > 3 ? QString(argv[3]) : QString("/tmp/hydra-test/evidence.json");
	const QString host = QUrl(target).host();
	const QString shot_dir = QFileInfo(out_path).absolutePath();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree("/home/nabbe/src/hydra/sample-tree.txt");
	w.resize(1280, 860);
	w.show();

	// The same object the Learn This Site dialog reads, reached the same way
	// the app wires it: a rider on the interceptor's observer seam.
	auto *sig = w.findChild<extractor_signals *>();
	std::printf("extractor_signals: %s\n", sig ? "found" : "MISSING");
	if (!sig) return 1;
	std::printf("target: %s\nhost:   %s\n", qPrintable(target), qPrintable(host));

	// Open a tab, then drive the address bar, exactly as try_tap does.
	QTimer::singleShot(2000, [&] {
		auto *tree = w.findChild<QTreeView *>();
		emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	});
	QTimer::singleShot(6000, [&] {
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address") {
				e->setText(target);
				QMetaObject::invokeMethod(e, "returnPressed");
				std::printf("navigated\n");
				return;
			}
		std::printf("NO ADDRESS BAR\n");
	});

	// Most of these players fetch nothing until someone presses play, and the
	// first click often lands on a consent banner, an ad vignette or a
	// challenge rather than the video. Shoot the screen as we go, because
	// "nothing happened" and "something is covering the player" look identical
	// in a request log.
	for (int i = 0; i < 3; ++i)
		QTimer::singleShot(16000 + i * 7000, [&, i] {
			centre_player(w, [&, i](QPoint at) {
				w.grab().save(QString("%1/shot-%2-before.png").arg(shot_dir).arg(i));
				click_at(w, at);
				std::printf("clicked %d at (%d,%d) — %d requests so far\n", i,
				             at.x(), at.y(), sig->count_for(host));
				QTimer::singleShot(3000, [&, i] {
					w.grab().save(QString("%1/shot-%2-after.png").arg(shot_dir).arg(i));
				});
			});
		});

	QTimer::singleShot(16000 + watch_s * 1000, [&] {
		const QList<evidence_request> ev = sig->evidence_for(host);
		std::printf("\n=== evidence: %lld requests for %s ===\n",
		             qint64(ev.size()), qPrintable(host));

		int kept = 0;
		const QString folded = extractor_dialog::summarise(ev, &kept);
		std::printf("folded to %d shapes:\n%s\n", kept,
		             qPrintable(folded.left(4000)));

		QJsonArray arr;
		for (const evidence_request &r : ev) {
			QJsonObject o;
			o.insert("url", r.url.toString());
			o.insert("kind", r.kind);
			o.insert("order", r.order);
			arr.append(o);
		}
		QJsonObject root;
		root.insert("page", target);
		root.insert("host", host);
		root.insert("requests", arr);

		QFile f(out_path);
		if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
			std::printf("\nwrote %lld requests to %s\n", qint64(arr.size()),
			             qPrintable(out_path));
		} else {
			std::printf("\nCOULD NOT WRITE %s\n", qPrintable(out_path));
		}
	});
	QTimer::singleShot(19000 + watch_s * 1000, [] {
		std::printf("done\n");
		qApp->quit();
	});
	return app.exec();
}
