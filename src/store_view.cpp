#include "store_view.h"

#include "download_manager.h"
#include "downloads_dialog.h"
#include "main_window.h"
#include "media_detector.h"
#include "tab_tree_model.h"
#include "web_view_backend.h"
#ifdef Q_OS_ANDROID
#include "android_intents.h"
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
	// Again now the window is up: Qt sets the system bars as it shows a
	// window, which can bring the bar back that `main` hid.
	android_intents::hide_status_bar();
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
		// What Find Media lists on the demo video page: three sizes in two
		// formats each, folded as a looked-up video is, and its sound.
		//
		// **Through the lookup cache, not into the list**, which is where a
		// real answer comes from. Put straight into the list, the rows were
		// taken out again by `show_cached_lookup` as the page came to the
		// front -- it replaces the page's vouched rows with the cache's,
		// which had none -- and the first run on a phone photographed an
		// empty dialog. Found 2026-10-22.
		struct demo { int height; const char *codec; const char *size; bool lead; };
		const demo rows[] = {
			{ 1080, "H.264", "212 MB", true },  { 1080, "AV1", "98 MB", false },
			{ 720,  "H.264", "118 MB", true },  { 720,  "AV1", "56 MB", false },
			{ 480,  "H.264", "61 MB",  true },  { 480,  "AV1", "29 MB", false },
		};
		QList<media_item> found;
		const QUrl sound(QString("https://%1/media/harbour-at-dusk.m4a").arg(store_view::host()));
		for (const demo &d : rows) {
			media_item m;
			const QString file = QString("harbour-at-dusk-%1p-%2.mp4")
			                       .arg(d.height).arg(QString(d.codec).toLower().remove('.'));
			m.kind        = media_kind::direct;
			m.url         = QUrl(QString("https://%1/media/%2").arg(store_view::host(), file));
			m.site_host   = store_view::host();
			m.file_name   = file;
			m.height      = d.height;
			m.has_audio   = false;
			m.audio       = sound;
			m.known_media = true;
			m.group       = QString("%1p").arg(d.height);
			m.group_lead  = d.lead;
			m.variant     = QString("%1p · %2 · %3").arg(d.height).arg(d.codec, d.size);
			m.label       = QString("[video] Harbour at dusk · %1 · picture only, the "
			                        "sound is added").arg(m.variant);
			m.rank        = 200000 + d.height * 4 + (d.lead ? 2 : 0);
			found << m;
		}
		media_item s;
		s.kind        = media_kind::direct;
		s.url         = sound;
		s.site_host   = store_view::host();
		s.file_name   = "harbour-at-dusk.m4a";
		s.has_video   = false;
		s.known_media = true;
		s.variant     = "m4a 129 kbit/s · 4.1 MB";
		s.label       = "[video] Harbour at dusk · " + s.variant + " · sound only";
		s.rank        = 100000;
		found << s;
		m_lookups.finish(QUrl(QString("https://%1/harbour.html").arg(store_view::host())),
		                 found, "Found 3 sizes of the video, and its sound.",
		                 QDateTime::currentDateTimeUtc());
		m_shown_lookup.clear();
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
	// **A load of its own to wait for.** The window warms background tabs
	// as it starts, the demo page among them, so by now it may already have
	// finished -- and a wait for a `load_finished` that came before the
	// connection never ends. A reload always ends in one, and costs nothing
	// for a page served from the package.
	v->reload();
}
