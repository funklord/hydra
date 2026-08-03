// A full turn of the extractor loop on evidence from a live page, rather than
// from the recorded set every other measurement in this project has used.
//
// It does one thing and writes it down: drive the real shell to a real watch
// page, press play, and dump what the interceptor saw as JSON. The proposal
// step is deliberately *not* here — evidence captured once and replayed offline
// is repeatable and costs the site nothing, and it is the second evidence set
// project.md says the prompt work needs before it can mean anything.
//
//   try_extract <url> [seconds-to-watch] [out.json] [css-to-click-first]
//
// The fourth argument exists because a watch page often puts the player behind
// a chooser — this site lists two mirrors from unrelated vendors, and only one
// iframe is in the initial HTML. Loading the other mirror's address directly
// does not work: it wants the embedding page's context and bounces to its own
// homepage without it. So the way to reach it is the way a user does, by
// clicking the thing that swaps it in.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "extractor_signals.h"
#include "extractor_dialog.h"
#include "mse_tap.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QWebEnginePage>
#include <QWebEngineSettings>
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
		std::printf("usage: try_extract <url> [seconds] [out.json] [css]\n");
		return 2;
	}
	const QString target = argv[1];
	const int watch_s = argc > 2 ? QString(argv[2]).toInt() : 40;
	const QString out_path =
		argc > 3 ? QString(argv[3]) : QString("/tmp/hydra-test/evidence.json");
	const QString pre_click = argc > 4 ? QString(argv[4]) : QString();
	const QString host = QUrl(target).host();
	const QString shot_dir = QFileInfo(out_path).absolutePath();

	// A private copy of the tree, beside the output. State blobs and policy.json
	// live next to the tree file, so sharing the repo's one means a capture
	// starts by *restoring the page the last capture left open* — and those
	// requests then land in this capture's evidence, attributed to this host. A
	// contaminated evidence file looks exactly like a real one. It also stops the
	// drivers rewriting `sample-tree.txt`, which until now had to be checked out
	// again by hand after every run.
	//
	// Only what this driver itself writes is cleared — the tree, its state
	// directory and its policy file. Not the output directory, which holds the
	// previous captures and screenshots someone may still be reading.
	//
	// The one node it holds is `about:blank`, and that is the other half of the
	// fix. The driver opens a tab and *then* types the address, so seeding it
	// with a real page meant that page's slower subresources were still arriving
	// after the navigation committed — by which time Chromium reports the new
	// address as their first party, and they land in this capture's evidence
	// under this capture's host. Twelve `doc.qt.io` requests in a dramafren
	// capture is how that presented. A blank first page has nothing to straggle.
	const QString tree = shot_dir + "/tree.txt";
	QFile::remove(shot_dir + "/policy.ini");
	QFile::remove(shot_dir + "/policy.json");
	QDir(shot_dir + "/state").removeRecursively();
	QDir().mkpath(shot_dir);
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		std::printf("could not seed a tree at %s\n", qPrintable(tree));
		return 1;
	}
	tf.write("- [f0] folder | Capture\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	// The second mirror pulls in `fuckadblock.min.js`, so it is watching for a
	// blocker before it will play — which makes "does this player start" and
	// "is Hydra blocking ads" the same question, and an evidence capture cannot
	// answer the first while the second is true. Off by default: a capture that
	// silently stopped blocking ads would be measuring a browser nobody runs.
	if (qEnvironmentVariableIntValue("HYDRA_ALLOW_ADS") == 1) {
		policy.set_global_default(policy::feature::ads, policy::setting::allow);
		std::printf("ads: not blocked for this capture\n");
	}
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
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

	// Swap in the mirror we were asked for, before anything hunts for an iframe.
	// Clicked in the page rather than by loading its address, because that is
	// what supplies the referer the vendor checks — and it is what a user does.
	if (!pre_click.isEmpty())
		QTimer::singleShot(13000, [&] {
			const auto vs = w.findChildren<QWebEngineView *>();
			if (vs.isEmpty()) return;
			const QString js =
				QString("(function(){var e=document.querySelector(%1);"
				         "if(!e) return 'no match'; e.scrollIntoView({block:'center'});"
				         "e.click(); return e.textContent.trim().slice(0,40); })()")
				    .arg(QString(QJsonDocument(QJsonArray{ pre_click })
				                     .toJson(QJsonDocument::Compact))
				             .mid(1).chopped(1));
			vs.last()->page()->runJavaScript(js, [](const QVariant &v) {
				std::printf("pre-click: %s\n", qPrintable(v.toString()));
			});
		});

	// Most of these players fetch nothing until someone presses play, and the
	// first click often lands on a consent banner, an ad vignette or a
	// challenge rather than the video. Shoot the screen as we go, because
	// "nothing happened" and "something is covering the player" look identical
	// in a request log.
	// A popunder that cannot open its window may never let a click through: the
	// usual pattern is to swallow clicks until `window.open` has succeeded once,
	// and `popups` is blocked by default. It turned out not to be the cause here
	// — allowing ads alone is what starts that player, with this still off — but
	// the lever is worth keeping, and keeping separate, so the two cannot be
	// confounded in a future capture.
	//
	// Off unless asked for, because letting a page open windows is exactly what
	// the policy engine is for. This is a capture driver deciding to be a worse
	// citizen for one run, and the evidence it produces should be read knowing
	// that.
	if (qEnvironmentVariableIntValue("HYDRA_ALLOW_POPUPS") == 1)
		QTimer::singleShot(15000, [&] {
			for (QWebEngineView *v : w.findChildren<QWebEngineView *>())
				v->page()->settings()->setAttribute(
					QWebEngineSettings::JavascriptCanOpenWindows, true);
			std::printf("popups: allowed for this capture\n");
		});

	// How many, because three is not always enough. On the second mirror every
	// click was answered by the ad network rather than by the player — a
	// popunder consuming one click at a time — so the clicks were landing and
	// being eaten. Requests appearing on an ad host after a click is the signal
	// to raise this rather than to conclude the player is broken.
	const int clicks = qEnvironmentVariableIsSet("HYDRA_CLICKS")
	                       ? qEnvironmentVariableIntValue("HYDRA_CLICKS") : 3;
	for (int i = 0; i < clicks; ++i)
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
		// What the §11.6 tap saw, beside what the request log saw. The two
		// answer different questions and the difference is the point: a page
		// that fetched no media but is feeding bytes into a MediaSource is
		// delivering over something a request log cannot see, while a page that
		// did neither has not started. Those two are indistinguishable from the
		// requests alone, and telling them apart is what this line is for.
		if (auto *tap = w.findChild<mse_tap *>()) {
			std::printf("\ntap active for %s: %s\n", qPrintable(host),
			             tap->active_for(host) ? "YES" : "no");
			// Every key, not just the one asked about. The hook reports the
			// hostname of the frame it runs in, so a player in a third-party
			// iframe files under *that* name — and asking only about the page's
			// host answers "no" to a page that is plainly playing.
			for (const QString &s : tap->sites()) {
				std::printf("  tap site %s%s\n", qPrintable(s),
				             s == host ? "" : "   (not the page's host)");
				for (const mse_stream &st : tap->streams_for(s))
					std::printf("    %s, %lld bytes, %d appends\n",
					             qPrintable(st.mime), qint64(st.bytes), st.appends);
			}
			if (tap->sites().isEmpty())
				std::printf("  tap saw nothing at all\n");
		} else {
			std::printf("\nmse_tap: MISSING\n");
		}

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
