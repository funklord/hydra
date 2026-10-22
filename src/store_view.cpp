#include "store_view.h"

#include "download_manager.h"
#include "downloads_dialog.h"
#include "main_window.h"
#include "media_detector.h"
#include "tab_tree_model.h"
#include "web_view_backend.h"
#ifdef Q_OS_ANDROID
#include "android_view.h"
#endif

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

namespace store_view {

namespace {

// Empty a test-mode directory, and only that: the path is checked to end in
// Qt's test directory before anything is removed, so a Qt that ever stopped
// honouring test mode here would leave the person's data alone rather than
// delete it.
bool clear(QStandardPaths::StandardLocation where, QString *why) {
	const QString dir = QStandardPaths::writableLocation(where);
	if (dir.isEmpty() || !dir.contains(QLatin1String("qttest"))) {
		*why = QString("%1 is not a test-mode directory").arg(dir);
		return false;
	}
	QDir(dir).removeRecursively();
	return QDir().mkpath(dir);
}

bool seed(const QString &from, const QString &to, QString *why) {
	QFile in(from);
	if (!in.open(QIODevice::ReadOnly)) {
		*why = QString("cannot read %1").arg(from);
		return false;
	}
	QFile out(to);
	if (!out.open(QIODevice::WriteOnly) || out.write(in.readAll()) < 0) {
		*why = QString("cannot write %1").arg(to);
		return false;
	}
	return true;
}

}  // namespace

QString host() {
	return QStringLiteral("hydra.example");
}

QStringList names() {
	return { "tabs", "page", "media", "downloads" };
}

bool prepare_profile(const QString &seed_dir, QString *why) {
	QStandardPaths::setTestModeEnabled(true);
	if (!clear(QStandardPaths::AppDataLocation, why) ||
	    !clear(QStandardPaths::CacheLocation, why) ||
	    !clear(QStandardPaths::AppConfigLocation, why))
		return false;
	const QDir data(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
	return seed(seed_dir + "/tree.txt", data.filePath("tree.txt"), why) &&
	       seed(seed_dir + "/download-history.json",
	            data.filePath("download-history.json"), why);
}

void announce_when_drawn(QWidget *w, const QString &name) {
	// Polled rather than caught as an event: the first paint of a top-level
	// window goes to its children, and which child is drawn last is the
	// layout's business. Exposed and visible is the condition; the settle
	// after it covers the frame that follows.
	QPointer<QWidget> watched(w);
	auto *poll = new QTimer(qApp);
	poll->setInterval(100);
	QObject::connect(poll, &QTimer::timeout, poll, [poll, watched, name] {
		if (!watched) {
			qWarning("store-view: the %s view went away before it was drawn",
			         qPrintable(name));
			poll->deleteLater();
			return;
		}
		QWindow *win = watched->window()->windowHandle();
		if (!watched->isVisible() || !win || !win->isExposed())
			return;
		poll->stop();
		poll->deleteLater();
		watched->window()->update();
		QTimer::singleShot(600, qApp, [name] {
			qInfo("store-view-ready %s", qPrintable(name));
		});
	});
	poll->start();
}

}  // namespace store_view

// A main_window member, kept beside the rest of the store views rather than
// in main_window.cpp: it reaches the window's private parts, and nothing but
// a screenshot run calls it.
void main_window::show_store_view(const QString &name) {
	if (!store_view::names().contains(name)) {
		qWarning("store-view: no view called %s; there are %s", qPrintable(name),
		         qPrintable(store_view::names().join(", ")));
		return;
	}
#ifdef Q_OS_ANDROID
	android_view::enable_store_demo();
#endif
	// The page each view stands on: the article for the first two, the
	// video's page for Find Media and the downloads.
	const bool article = name == "tabs" || name == "page";
	node *n = m_model->node_by_id(article ? "r1" : "w1");
	if (!n) {
		qWarning("store-view: the demo tree is not the one loaded");
		return;
	}
	show_node(n);
	web_view_backend *v = current_view();
	if (!v)
		return;

	if (name == "media") {
		// What Find Media lists on the demo video page. Put in directly,
		// since detection depends on what a real player fetched and when.
		struct demo { const char *file; const char *label; int height; };
		const demo rows[] = {
			{ "harbour-at-dusk-1080p.mp4", "Harbour at dusk (1080p)", 1080 },
			{ "harbour-at-dusk-720p.mp4",  "Harbour at dusk (720p)",  720  },
			{ "harbour-at-dusk-480p.mp4",  "Harbour at dusk (480p)",  480  },
		};
		for (const demo &d : rows) {
			media_item m;
			m.kind        = media_kind::direct;
			m.url         = QUrl(QString("https://%1/media/%2").arg(store_view::host(), d.file));
			m.label       = d.label;
			m.file_name   = d.file;
			m.height      = d.height;
			m.known_media = true;
			m_media->add_item(store_view::host(), m);
		}
		QTimer::singleShot(0, this, [this] { open_media(); });
		// `open_media` runs its dialog modally, so the dialog is found by
		// asking which window is modal once it is up.
		auto *find = new QTimer(this);
		find->setInterval(100);
		connect(find, &QTimer::timeout, find, [find] {
			if (QWidget *dlg = QApplication::activeModalWidget()) {
				find->stop();
				find->deleteLater();
				store_view::announce_when_drawn(dlg, "media");
			}
		});
		find->start();
		return;
	}
	if (name == "downloads") {
		open_downloads();
		store_view::announce_when_drawn(m_downloads_ui, name);
		return;
	}

	// The two that show the page wait for it.
	auto *once = new QObject(this);
	connect(v, &web_view_backend::load_finished, once, [this, v, once, name](bool) {
		once->deleteLater();
		if (name == "tabs") {
			set_tree_visible(true);
			store_view::announce_when_drawn(this, name);
			return;
		}
#ifdef Q_OS_ANDROID
		if (auto *a = qobject_cast<android_view *>(v)) {
			a->announce_when_drawn(name);
			return;
		}
#endif
		store_view::announce_when_drawn(v->widget(), name);
	});
}
