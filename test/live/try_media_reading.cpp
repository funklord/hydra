// Reading what a real video is, through the real shell, end to end: the menu
// action, yt-dlp, the local model, the checks, and the record kept in
// readings.ini. Live: it needs the network and a running Ollama.
//
//     QT_QPA_PLATFORM=offscreen ./test/build-make/try_media_reading [url]
//
// HYDRA_TRY_SAVE=1 presses Download on the smallest picture-only file row
// instead, which fetches its sound too, and waits up to ten minutes for the
// two to be combined into one file under the scratch HOME.
//
// HYDRA_TRY_WATCH=1 also presses Watch on the first row, and =picture on the
// first row that plays a picture, then stays up 40 s so the player can fetch
// through this process's proxy. It starts a real player: put a wrapper named
// mpv first on PATH that adds `--ao=null --vo=null`, or it plays aloud.
//
// **Why this exists**: the holder's first installed run of the reading never
// reached it -- the installed build ran an old yt-dlp that YouTube refused --
// while every offline suite passed, because each tested a piece and none the
// chain a person actually triggers. This triggers it.
//
// The profile is a temporary directory; the settings select the local model
// at its default address, so a running Ollama is used and nothing else.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "reading_log.h"
#include "request_filter.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static QAction *menu_action(QWidget *w, const QString &text) {
	for (QAction *a : w->findChildren<QAction *>())
		if (a->text().remove('&') == text)
			return a;
	return nullptr;
}

// The Watch buttons in row order, or only those of rows that play a picture.
static QList<QPushButton *> watch_buttons(QDialog *dlg, bool picture) {
	QList<QPushButton *> out;
	auto *list = dlg->findChild<QTreeWidget *>();
	if (!list)
		return out;
	for (int i = 0; i < list->topLevelItemCount(); ++i) {
		QTreeWidgetItem *row = list->topLevelItem(i);
		if (picture && row->text(1).contains("sound only"))
			continue;
		if (QWidget *cell = list->itemWidget(row, 3))
			for (QPushButton *b : cell->findChildren<QPushButton *>())
				if (b->text().contains("Watch")) {
					b->setProperty("row", row->text(1));
					out << b;
				}
	}
	return out;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	// Downloads land under HOME; a real one is not this driver's to fill.
	qputenv("HOME", base);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	const QString url = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                             : QStringLiteral("https://www.youtube.com/watch?v=ftQQAvj74KI");
	{
		QSettings s(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
		s.setValue("ai/mode", "local_only");
		s.setValue("ai/ollama_endpoint", "http://localhost:11434");
		s.setValue("ai/ollama_model", qEnvironmentVariableIsSet("HYDRA_MODEL")
		  ? qEnvironmentVariable("HYDRA_MODEL") : QStringLiteral("qwen2.5-coder:14b"));
		s.sync();
	}
	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | A video | %1 | "
		                 "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n")
		             .arg(url).toUtf8());
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(profile + "/tree.txt");
	w.resize(1200, 900);
	w.show();
	spin(800);
	auto *tree = w.findChild<QTreeView *>();
	emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	spin(5000);

	// A failure window would wait for a click; answer it and say what it said.
	// The streams window that follows a reading is closed too: it is modal,
	// and the first version of this driver sat behind it for three minutes
	// with the reading already written, reporting that nothing had been.
	QString failure;
	QTimer watch;
	QObject::connect(&watch, &QTimer::timeout, [&failure] {
		for (QWidget *t : QApplication::topLevelWidgets()) {
			auto *dlg = qobject_cast<QDialog *>(t);
			if (!dlg || !dlg->isVisible())
				continue;
			if (auto *box = qobject_cast<QMessageBox *>(dlg)) {
				failure = box->text();
				box->accept();
			} else {
				// HYDRA_TRY_WATCH presses the first row's Watch first, and
				// says what the window answered.
				if (auto *list = dlg->findChild<QTreeWidget *>())
					for (int i = 0; i < qMin(3, list->topLevelItemCount()); ++i)
						std::printf("  row %d: %s\n", i,
						            qPrintable(list->topLevelItem(i)->text(1)));
				if (qEnvironmentVariableIsSet("HYDRA_TRY_SAVE"))
					if (auto *list = dlg->findChild<QTreeWidget *>()) {
						QPushButton *last = nullptr;
						QString label;
						for (int i = 0; i < list->topLevelItemCount(); ++i) {
							QTreeWidgetItem *row = list->topLevelItem(i);
							if (row->text(0) != "File" ||
							    !row->text(1).contains("the sound is added"))
								continue;
							if (QWidget *cell = list->itemWidget(row, 3))
								for (QPushButton *b : cell->findChildren<QPushButton *>())
									if (b->text().contains("Download")) {
										last  = b;
										label = row->text(1);
									}
						}
						if (last) {
							std::printf("  pressing Download on: %s\n  enabled=%d tip=%s\n",
							            qPrintable(label), int(last->isEnabled()),
							            qPrintable(last->toolTip()));
							last->click();
							for (QLabel *l : dlg->findChildren<QLabel *>())
								if (!l->text().isEmpty())
									std::printf("  label: %s\n", qPrintable(l->text().left(300)));
						}
					}
				if (qEnvironmentVariableIsSet("HYDRA_TRY_WATCH"))
					// =picture presses the first row that plays a picture.
					for (QPushButton *b : watch_buttons(dlg,
					       qEnvironmentVariable("HYDRA_TRY_WATCH") == "picture"))
						if (b->text().contains("Watch")) {
							std::printf("  watch enabled=%d tip=%s\n", int(b->isEnabled()),
							            qPrintable(b->toolTip()));
							std::printf("  pressing Watch on: %s\n",
							            qPrintable(b->property("row").toString()));
							b->click();
							for (QLabel *l : dlg->findChildren<QLabel *>())
								if (!l->text().isEmpty())
									std::printf("  label: %s\n", qPrintable(l->text().left(300)));
							break;
						}
				std::printf("  closing: %s\n", qPrintable(dlg->windowTitle()));
				dlg->reject();
			}
		}
	});
	watch.start(200);

	// What the shell says as it goes, so a run that stops says where.
	auto say = [](const QString &m) {
		if (!m.isEmpty())
			std::printf("  status: %s\n", qPrintable(m));
	};
	if (auto *bar = w.findChild<QStatusBar *>())
		QObject::connect(bar, &QStatusBar::messageChanged, say);

	QAction *find = menu_action(&w, "Find Media on This Page…");
	check(find != nullptr, "the menu action is there");
	if (!find)
		return 1;
	find->trigger();
	const QString path = profile + "/readings.ini";
	for (int i = 0; i < 1800 && !QFile::exists(path) && failure.isEmpty(); ++i)
		spin(100);
	if (!failure.isEmpty())
		std::printf("  yt-dlp said: %s\n", qPrintable(failure.left(400)));
	check(QFile::exists(path), "a reading is recorded in readings.ini");

	reading_log log;
	log.load(path);
	check(log.size() == 1, QString("one reading (%1)").arg(log.size()));
	if (log.size() == 1) {
		const reading_record r = log.all().first();
		std::printf("  read: %s | %s | %s | set=%d | tracklist=%s | by %s\n"
		            "  file: %s\n  rejected: %s\n  flagged: %s\n",
		            qPrintable(r.kind), qPrintable(r.artist), qPrintable(r.title),
		            int(r.is_set), qPrintable(r.tracklist), qPrintable(r.model),
		            qPrintable(r.file_name), qPrintable(r.rejected.join("; ")),
		            qPrintable(r.flagged.join("; ")));
		check(r.model.startsWith("Ollama "), "read by the local model, not the rules");
		check(!r.file_name.isEmpty() && r.file_name.contains(r.id),
		       "and the file name carries the video's id");
	}
	if (qEnvironmentVariableIsSet("HYDRA_TRY_SAVE")) {
		// Done when one file is there that is neither half.
		const QDir dl(QString::fromLocal8Bit(base) + "/Downloads");
		QString combined;
		// Thirty seconds with no Downloads folder is a download that never
		// started, and not worth waiting ten minutes to report.
		for (int i = 0; i < 6000 && combined.isEmpty() && (i < 300 || dl.exists());
		     ++i) {
			spin(100);
			for (const QFileInfo &fi : dl.entryInfoList(QDir::Files))
				if (!fi.fileName().contains(".picture.") &&
				    !fi.fileName().contains(".sound.") && fi.size() > 0)
					combined = fi.absoluteFilePath();
		}
		for (const QFileInfo &fi : dl.entryInfoList(QDir::Files))
			std::printf("  in Downloads: %s (%lld bytes)\n",
			            qPrintable(fi.fileName()), qint64(fi.size()));
		check(!combined.isEmpty(), "the pair is combined into one file");
		if (!combined.isEmpty()) {
			spin(2000);   // the halves are removed after the file appears
			QProcess probe;
			probe.start(QStandardPaths::findExecutable("ffmpeg"),
			            { "-hide_banner", "-i", combined });
			probe.waitForFinished(10000);
			const QString said = QString::fromLocal8Bit(probe.readAllStandardError());
			check(said.contains("Video:") && said.contains("Audio:"),
			      "holding a picture and a sound");
			// Whole, not the first range of each: the first version combined
			// 10 MB of each half and passed both checks above.
			const int at = said.indexOf("Duration: ");
			std::printf("  %s\n", qPrintable(said.mid(at, 22)));
			check(at >= 0 && !said.mid(at + 10, 8).startsWith("00:0"),
			      "and runs longer than a few minutes, so neither half stopped short");
			for (const QFileInfo &fi : dl.entryInfoList(QDir::Files))
				std::printf("  left: %s\n", qPrintable(fi.fileName()));
			check(dl.entryList(QDir::Files).size() == 1, "and nothing else is left");
		}
	}

	// The player fetches through this process's proxy; stay up long enough
	// for it to show whether it plays.
	if (qEnvironmentVariableIsSet("HYDRA_TRY_WATCH"))
		spin(40000);
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
