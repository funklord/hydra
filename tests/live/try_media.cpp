// Point the media path at a real site and report, honestly, what it sees.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "media_detector.h"
#include "web_view_backend.h"
#include "sample_tree.h"
#include "media_fixture.h"

#include <QApplication>
#include <QDir>
#include <QMutex>
#include <QTimer>
#include <QUrl>
#include <QLineEdit>
#include <QTreeView>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QMouseEvent>
#include <cstdio>

// Logs every request the interceptor sees, so a stream that was fetched but
// not classified is distinguishable from one that was never fetched at all.
class request_log : public request_observer {
public:
	void on_request(const request_context &ctx, const request_decision &d) override {
		QMutexLocker lock(&m_lock);
		++m_total;
		if (d.block) {
			++m_blocked;
			if (m_blocked_urls.size() < 12)
				m_blocked_urls << ctx.url.toString().left(90);
		}
		const QString u = ctx.url.toString();
		bool saveable = false;
		const media_kind k = media_detector::classify(ctx.url, &saveable);
		if (k != media_kind::direct || saveable)
			m_interesting << QString("[%1] %2").arg(int(k)).arg(u.left(150));
		// Anything that smells like video, whether or not we classify it.
		static const QStringList hints = { ".m3u8", ".mpd", ".m4s", ".mp4",
			                                  ".webm", ".mkv", "/hls/", "/dash/",
			                                  "playlist.", "master." };
		for (const QString &h : hints) {
			if (u.contains(h, Qt::CaseInsensitive)) {
				m_smells << u.left(170);
				break;
			}
		}
		m_hosts.insert(ctx.request_host);
	}
	QStringList interesting() const { QMutexLocker l(&m_lock); return m_interesting; }
	QStringList smells() const { QMutexLocker l(&m_lock); return m_smells; }
	QSet<QString> hosts() const { QMutexLocker l(&m_lock); return m_hosts; }
	int total() const { QMutexLocker l(&m_lock); return m_total; }
	int blocked() const { QMutexLocker l(&m_lock); return m_blocked; }
	QStringList blocked_urls() const { QMutexLocker l(&m_lock); return m_blocked_urls; }
private:
	mutable QMutex m_lock;
	QStringList m_interesting, m_smells;
	QSet<QString> m_hosts;
	int m_total = 0;
	int m_blocked = 0;
	QStringList m_blocked_urls;
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	// A local fixture unless a real site is named. See media_fixture.h for
	// why the default is not a real one, and what that costs.
	media_fixture::server fixture;
	const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                                : fixture.start();

	policy_engine  policy;
	request_filter filter(&policy);
	request_log    log;
	filter.add_observer(&log);
	qtwebengine_factory factory(&filter);

	main_window w(&factory, &policy, &filter);
	// The tree it opens is the fixture's, not the committed example's: this
	// driver activates the first tab on purpose, and that entry is a real site.
	w.load_tree(shell::single_tab_tree(target));
	w.resize(1280, 860);
	w.show();

	auto *det = w.findChild<media_detector *>();
	std::printf("media_detector: %s\n", det ? "found" : "MISSING");

	// Open a tab first — the address bar acts on the current view — then
	// navigate, exactly as a user would.
	QTimer::singleShot(2000, [&] {
		auto *tree = w.findChild<QTreeView *>();
		const QModelIndex first = tree->model()->index(0, 0);
		emit tree->activated(tree->model()->index(0, 0, first));
		std::printf("tab opened\n");
	});
	QTimer::singleShot(6000, [&] {
		QLineEdit *addr = nullptr;
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address")
				addr = e;
		if (!addr) { std::printf("NO ADDRESS BAR\n"); return; }
		addr->setText(target);
		std::printf("navigating to %s\n", qPrintable(target));
		QMetaObject::invokeMethod(addr, "returnPressed");
	});

	auto report = [&](const char *when) {
		const QString host = QUrl(target).host();
		std::printf("\n===== %s =====\n", when);
		std::printf("requests seen: %d across %d hosts\n",
		             log.total(), log.hosts().size());
		QStringList hs = log.hosts().values();
		hs.sort();
		std::printf("hosts: %s\n", qPrintable(hs.join(", ").left(600)));
		std::printf("blocked: %d  (%s)\n", log.blocked(),
		             qPrintable(log.blocked_urls().join(" | ").left(400)));
		// Report per *site key*, not just the page host: an embedded player
		// lives on another host, and if its requests are filed under that host
		// the toolbar badge for the page the user is on would stay empty even
		// though the stream was detected.
		int found = 0;
		QStringList keys = log.hosts().values();
		keys << host;
		keys.removeDuplicates();
		keys.sort();
		for (const QString &k : keys) {
			const QList<media_item> its = det ? det->items_for(k) : QList<media_item>{};
			if (its.isEmpty())
				continue;
			found += its.size();
			std::printf("detector: %d item(s) filed under \"%s\"\n",
			             int(its.size()), qPrintable(k));
			for (const media_item &m : its)
				std::printf("   kind=%d hits=%d  %s\n", int(m.kind), m.hits,
				             qPrintable(m.url.toString().left(140)));
		}
		if (!found)
			std::printf("detector found nothing, under any host\n");
		std::printf("badge for the page the user is on (%s): %d\n",
		             qPrintable(host), det ? det->count_for(host) : -1);
		const QStringList sm = log.smells();
		std::printf("video-shaped URLs seen: %d\n", int(sm.size()));
		for (int i = 0; i < sm.size() && i < 25; ++i)
			std::printf("   %s\n", qPrintable(sm[i]));
	};

	// Reach past the seam deliberately: this is a diagnostic, and the question
	// is what the *engine* ended up with in the DOM.
	QTimer::singleShot(20000, [&] {
		const auto views = w.findChildren<QWebEngineView *>();
		if (views.isEmpty()) { std::printf("PROBE no view\n"); return; }
		views.last()->page()->runJavaScript(
		  "(function(){var f=document.querySelectorAll('iframe');"
		  "var out='URL='+location.href+' BODY='+document.body.innerHTML.length"
		  "+' IFRAMES='+f.length;"
		  "for(var i=0;i<f.length&&i<4;i++)out+=' ['+(f[i].getAttribute('src')||f[i].src||'(none)')"
		  "+' vis='+(f[i].offsetParent!==null)+']';"
		  "var v=document.querySelectorAll('video');out+=' VIDEOS='+v.length;"
		  "return out;})()",
		  [](const QVariant &v) {
			  std::printf("PROBE %s\n", qPrintable(v.toString().left(400)));
		  });
	});

	// Force the frame to load and see whether a request appears at all. If
	// re-assigning src produces one, the element was fine and something
	// suppressed the initial fetch; if it produces nothing, the engine is
	// refusing the frame outright.
	QTimer::singleShot(24000, [&] {
		const auto views = w.findChildren<QWebEngineView *>();
		if (views.isEmpty()) return;
		views.last()->page()->runJavaScript(
		  "(function(){var f=document.querySelector('iframe');"
		  "if(!f) return 'no iframe';"
		  "var s=f.getAttribute('src'); f.removeAttribute('src'); f.src=s;"
		  "return 'forced '+s;})()",
		  [](const QVariant &v) { std::printf("FORCE %s\n", qPrintable(v.toString())); });
	});

	// Press play. Streams are usually not requested until the player is told
	// to start (§11.3 says exactly this), so without a real click the detector
	// has nothing to detect. A posted Qt mouse event goes through the engine's
	// normal input path and counts as a user gesture.
	QTimer::singleShot(21000, [&] {
		const auto views = w.findChildren<QWebEngineView *>();
		if (views.isEmpty()) { std::printf("CLICK no view\n"); return; }
		QWidget *target = views.last()->focusProxy();
		if (!target) target = views.last();
		const QPoint at(target->width() / 2, target->height() / 2);
		for (int i = 0; i < 2; ++i) {          // some players want a second one
			QMouseEvent press(QEvent::MouseButtonPress, at, target->mapToGlobal(at),
			                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QMouseEvent release(QEvent::MouseButtonRelease, at, target->mapToGlobal(at),
			                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(target, &press);
			QApplication::sendEvent(target, &release);
		}
		std::printf("CLICK sent at %d,%d on %s\n", at.x(), at.y(),
		             target->metaObject()->className());
	});

	QTimer::singleShot(22000, [&] { report("after page load"); });
	QTimer::singleShot(45000, [&] { report("after settling"); });
	QTimer::singleShot(47000, [&] { std::printf("\ndone\n"); qApp->quit(); });
	return app.exec();
}
