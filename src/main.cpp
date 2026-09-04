/*! @file
 * @target hydra
 *
 * Without this the program is called `src`: fmake names a program after the
 * file that defines main(), or after the containing directory when that file
 * is main.cpp. Naming it here rather than in a build file keeps the fact where
 * the entry point is.
 */
#include "main_window.h"
#include "settings_dialog.h"   // settings_store
#include "theme.h"
#ifdef Q_OS_ANDROID
#include "android_dialogs.h"
#endif
#include "policy_engine.h"
#include "request_filter.h"
// Desktop only, and excluded from the Android build in `hydra.pro` for the
// reason the guard block below gives: Android runs one process per
// application, so there is no second one to keep out.
#ifndef Q_OS_ANDROID
#include "single_instance.h"
#endif
// The single place that names a concrete backend (architecture doc sec 19.2). The
// whole point of the seam is that this is the only file that has to know, and
// that is now measured rather than asserted: the other fifty-one translation
// units compile for arm64 unchanged.
#ifdef Q_OS_ANDROID
#include "android_view.h"
#else
#include "qtwebengine_factory.h"
#include "qtwebengine_notifications.h"
#endif
#include "torrent_download_source.h"

#include <QApplication>
#include <QIcon>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

#include "accept_language.h"
#include "address_input.h"

#include <QLocale>
#include <QStandardPaths>
#include <QtGlobal>

int main(int argc, char *argv[]) {
	// **Before anything else, because it must not need a display.** A version
	// flag that starts a browser is not one: `hydra --version` used to fall
	// through to the argument classifier, be read as something to open, and
	// raise a window -- which on a machine with no display hangs until
	// somebody kills it. Measured that way while installing this build.
	//
	// **First statement, and that was learned by running it.** The first
	// attempt put this below the single-instance guard and above the
	// classifier, which reads as "early" and is not: `QApplication` is
	// constructed in between, and with no display it aborts before reaching
	// here -- *"no Qt platform plugin could be initialized"*, exit 134, the
	// version never printed. Nothing about the source said so.
	//
	// The first line is what a script would parse, so it is the plain
	// `name version` and nothing else. The Qt line is here because an
	// evening was lost this week to not knowing which of three Qt builds a
	// binary was running against, and the answer is free at runtime.
	//
	// **The copyright line, and no licence line.** `harmonization.md` asks
	// that the holder be named in `--version`, in an About window and in the
	// README, and naming them is factual -- authorship vests on its own and
	// saying who wrote something grants nothing. This project states no terms
	// anywhere and `debian/copyright` says so; a licence line here would be
	// inventing one, which is the copyright holder's alone.
	//
	// The year is the year the work began, taken from the first commit rather
	// than chosen: `git log --reverse --format=%ad --date=short | head -1`
	// answers 2026-07-29.
	if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--version")) {
		std::printf("hydra %s\n", HYDRA_VERSION);
		std::printf("Qt %s (built against %s)\n", qVersion(), QT_VERSION_STR);
		std::printf("Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>\n");
		return 0;
	}

	// Desktop Linux only: force the xcb platform plugin unless the environment
	// has already chosen one, so the X11 behaviour this design relies on
	// (architecture doc sec 2/sec 14) stays predictable, and a Wayland session runs
	// under XWayland. Every other target has one sensible platform plugin --
	// Windows, macOS, and Android each pick correctly on their own -- so forcing
	// anything there would be actively wrong. Q_OS_LINUX is also defined on
	// Android, hence the second half of the guard.
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", QByteArray("xcb"));
#endif

	// **Tell the engine what locale it is running in, before it starts.**
	//
	// Qt WebEngine passes no `--lang`, so Chromium's ICU falls back to a bare
	// language. Measured against the Chromium beside it on a machine whose
	// `LANG` is `en_US.UTF-8`: `Intl.DateTimeFormat().resolvedOptions().locale`
	// answered "en" here and "en-US" there, and `Intl.Collator` and
	// `Intl.NumberFormat` agreed with it -- while `Accept-Language`,
	// `navigator.language` and `navigator.languages` were identical in both. A
	// browser that says en-US and resolves to en is disagreeing with itself,
	// and a site that stores a locale and checks it later reads a change that
	// never happened. Reported as Teams showing "Language changes detected
	// (English)" on every load, in hydra and in no other browser.
	//
	// Appended rather than assigned, so a flag somebody set for their own
	// reasons survives, and skipped entirely if they have already said `--lang`
	// -- an explicit choice outranks this one.
	{
		const QString tag =
		  accept_language::primary_tag(QLocale::system().uiLanguages());
		const QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
		if (!tag.isEmpty() && !existing.contains("--lang=")) {
			QByteArray flags = existing;
			if (!flags.isEmpty())
				flags += ' ';
			flags += "--lang=" + tag.toUtf8();
			qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
		}
	}

	// Recommended for Qt WebEngine.
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

	// Custom URL schemes must be registered before the engine initialises, so
	// this cannot move later. Each download source names the non-web schemes it
	// claims; without the registration Chromium treats them as external
	// protocols and drops such navigations before anything of ours can see them
	// (sec 11.4). The list is empty when the feature is not built, and then
	// nothing changes.
#ifndef Q_OS_ANDROID
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
#endif

	QApplication app(argc, argv);
#ifdef Q_OS_ANDROID
	// Dialogs here were laid out for a desktop and come up wider than a phone
	// screen, which puts their buttons off the edge with no way to reach them.
	android_dialogs::install();
#endif
	app.setApplicationName("Hydra");

	// **The argument may be a url rather than a tree.** The desktop entry says
	// `Exec=hydra %U` and claims http, https and text/html, so once this is
	// installed as the default browser every clicked link arrives as argv[1].
	// Read as a tree path it names no file, the window comes up empty, and the
	// link is gone -- which is what a browser that cannot open a link looks
	// like from the outside.
	//
	// Which schemes are a page rather than a tree lives in `argument_url`,
	// beside the other question about what a piece of text means, and is
	// tested there. `hydra ./tree.txt` still means the tree: the classifier
	// reads the scheme as *written*, and a path has none.
	//
	// Classified here, above everything, because the single-instance guard
	// below has to know what this process was asked to do before it can decide
	// what to do about it.
	QString open_arg;
	if (argc > 1) {
		const QUrl candidate =
		  argument_url(QString::fromLocal8Bit(argv[1]));
		if (candidate.isValid())
			open_arg = candidate.toString();
	}

#ifndef Q_OS_ANDROID
	// **One Hydra per profile directory**, and this is the first thing after
	// the application name because the application name is what names that
	// directory -- and because everything below it, the web engine profile
	// most of all, is what must not happen twice.
	//
	// It was harmless until it was not. While the profile was off the record
	// each process allocated its own storage and two copies simply did not
	// meet; the named persistent profile put both of them into one directory
	// of leveldb databases and one SQLite `Cookies` file, none of which
	// arbitrates between two writers. It happened twice by accident in the
	// session that added the profile.
	//
	// **Android is deliberately not in this.** There the system runs one
	// process per application and a launcher tap resumes the task it already
	// has, so a second instance is not something that can be started; a lock
	// there would guard against nothing and would be one more thing to leave
	// behind. Links arrive as intents inside the running process rather than
	// as argv, so there is nothing to hand over either.
	single_instance guard(
	  QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
	if (!guard.acquire()) {
		// A tree path is the one argument that cannot be handed over. The
		// running instance already has a tree open and swapping it underneath
		// somebody is not what `hydra other-tree.txt` means -- so say so and
		// stop, rather than raising a window showing a different file.
		if (argc > 1 && open_arg.isEmpty()) {
			qCritical("hydra is already running (%s); quit it before opening "
			           "another tree", qPrintable(guard.owner()));
			return 1;
		}
		// Nothing to open means the launcher was used twice, which is a
		// request to see the window that already exists.
		if (!guard.hand_over(open_arg)) {
			qCritical("hydra is already running (%s) and is not answering; "
			           "refusing to open the same profile twice",
			           qPrintable(guard.owner()));
			return 1;
		}
		return 0;
	}
#endif

	// The colour scheme, before anything is shown: applying it after the first
	// window is up means a visible flash of the wrong theme, which is the sort
	// of thing that looks like a bug in the window manager.
	//
	// The watcher outlives this scope and keeps following the desktop, so a
	// system that switches at sunset takes Hydra with it. It only acts while the
	// choice is "system" -- someone who picked Dark meant Dark.
	// The icon theme first, and before any icon is built. Qt6 finds one through
	// a platform-theme plugin, of which it ships Plasma's and GTK's; this
	// desktop is Trinity and loads neither, so QIcon::themeName() is empty and
	// every QIcon::fromTheme comes back null. The toolbar then draws nothing and
	// says nothing about why.
	// Before any widget: a disabled icon is generated by the *current* style,
	// and Qt's own gives away 40% of an icon's contrast on top of desaturating
	// it -- which is most of what the back and forward arrows look like, since
	// they are unavailable more often than not.
	theme::install_icon_style();

	if (const QString icons = theme::apply_icon_theme(
	        theme::resolve(settings_store::appearance()));
	    !icons.isEmpty())
		qInfo("icon theme: %s", qPrintable(icons));
	else
		qWarning("no icon theme found; toolbar buttons will fall back to text");

	auto *appearance = new theme::watcher(&app);
	appearance->set_choice(settings_store::appearance());
	// And the web engine, before anything creates a profile: this one is a
	// startup flag rather than a live setting, for the reason theme.h explains.
	theme::set_web_engine_scheme(theme::resolve(settings_store::appearance()));

	// Every size is added rather than one image scaled, because the 16px cut is
	// drawn pixel by pixel rather than resampled and would be thrown away by a
	// QIcon that only knew the large one. Qt then picks per use: the tab strip
	// gets the drawn 16, the alt-tab switcher gets 48, the about box gets 256.
	QIcon icon;
	for (int size : { 16, 24, 32, 48, 64, 128, 256 })
		icon.addFile(QString(":/icon/hydra-%1.png").arg(size),
		              QSize(size, size));
	QApplication::setWindowIcon(icon);

	// The only place in the tree that names a concrete web view backend
	// (architecture doc sec 19.2). Swapping in the Android System WebView is
	// meant to be a change to these two lines plus one new backend class.
	// Declaration order matters: each of these outlives the ones below it.
	policy_engine       policy;
	request_filter      filter(&policy);
#ifdef Q_OS_ANDROID
	android_factory factory(&filter);
#else
	qtwebengine_factory factory(&filter);

	// **Notifications may be asked about only if they can be delivered.**
	//
	// Chromium treats a missing presenter as success: the page's notification
	// resolves and goes nowhere. So granting the permission without one puts a
	// prompt in front of somebody for a capability that then does nothing, and
	// the settings page calls it allowed. That is why `policy_engine` defaults
	// this to block while every other real capability moved to `ask`, and this
	// is the line that lifts it once there is somewhere for a notification to
	// go -- a working `org.freedesktop.Notifications` on the session bus,
	// established by asking it rather than by looking up its name.
	//
	// **Before the policy file is read**, which is the whole reason it is here
	// and not later. `main_window`'s constructor loads `policy.ini` immediately
	// below, so anything saved -- including a deliberate block -- overwrites
	// this. It raises a default, it does not overrule a decision.
	if (qtwebengine_notifications::install(factory.profile()))
		policy.set_global_default(policy::feature::notifications,
		                           policy::setting::ask);
#endif

	main_window w(&factory, &policy, &filter);

	// Where the tree lives.
	//
	// **The example is a seed, never the working file.** `sample-tree.txt` in
	// the repository is a committed example: it should change when the example
	// changes and at no other time. The tree an actual person uses is a
	// personal file that changes constantly, by design -- every title a page
	// supplies, every `seen=`, every tab opened or closed.
	//
	// Those are two different files and this used to conflate them. With no
	// argument the search was cwd, then beside the binary, then app data -- so
	// running the browser from a checkout picked up the tracked example *as the
	// working file* and wrote to it. It was reverted from git five times in one
	// day, mostly by people who had not knowingly run anything against it.
	//
	// So: an explicit argument means exactly that file, because somebody asked
	// for it. Otherwise the tree is the personal one in app data, seeded on
	// first run from whichever example can be found. The seed is only ever
	// read.
	//
	// This is also what Android needed, for its own reason -- there is no
	// working directory worth the name there, it is `/` and nothing in it is
	// writable -- and it was solved separately for that platform. One path now,
	// because two answers to one question is how they drift.
	// **A function, because it is now wanted in two places**: when no argument
	// names a tree, and when the argument turned out not to be one.
	const auto default_tree = [] {
		const QString dir =
		  QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		QDir().mkpath(dir);
		const QString path = QDir(dir).filePath("tree.txt");

		// First run gets the example, so the app opens with something in it
		// rather than an empty pane that looks like a failure. Preference
		// order: a checkout being worked in, then the copy the build puts
		// beside the binary, then the one compiled in -- which is
		// the only one an installed or packaged copy has.
		if (!QFileInfo::exists(path)) {
			QStringList seeds;
			seeds << QStringLiteral("sample-tree.txt")
			      << QDir(QCoreApplication::applicationDirPath())
			             .filePath("sample-tree.txt")
			      << QStringLiteral(":/sample-tree.txt");
			for (const QString &from : std::as_const(seeds)) {
				QFile seed(from);
				if (!seed.open(QIODevice::ReadOnly))
					continue;
				QFile out(path);
				if (out.open(QIODevice::WriteOnly))
					out.write(seed.readAll());
				break;
			}
		}
		return path;
	};

	QString tree_path;
	if (argc > 1 && open_arg.isEmpty())
		tree_path = QString::fromLocal8Bit(argv[1]);
	if (tree_path.isEmpty())
		tree_path = default_tree();

	// **A refused argument must not cost the session everything it saves.**
	// `load_tree` declines a path whose directory does not exist -- which is
	// what a url handed over as an argument looks like -- and every writer in
	// the window is guarded on the paths it would have set, so without this the
	// browser would come up working and persist nothing at all: no tree, no
	// view state, no tab histories, and no hint beyond one line on stderr.
	// Silently saving nothing is a worse failure than the litter this replaced.
	//
	// Falling back to the personal tree, loudly. It is the file the same
	// command with no argument would have opened, so the browser is the one
	// the person already has rather than an empty imitation of it.
	if (!w.load_tree(tree_path)) {
		const QString fallback = default_tree();
		if (fallback != tree_path) {
			qCritical("tree: opening %s instead", qPrintable(fallback));
			w.load_tree(fallback);
		}
	}
	// After the tree, so the tab lands in a loaded tree rather than being
	// dropped when the file replaces the model underneath it.
	if (!open_arg.isEmpty())
		w.open_url(QUrl(open_arg));

#ifndef Q_OS_ANDROID
	// What a second instance's argument does when it arrives. Registered after
	// the tree is loaded for the same reason the call above is: a tab handed
	// to an empty model is dropped when the file replaces it.
	//
	// The lambda holds the window by reference and may not outlive it. It
	// cannot: the guard is declared above the window, so the window is
	// destroyed first, and nothing dispatches a connection after `exec()`
	// returns. That is the same declaration-order argument the web engine
	// factory relies on, for the same kind of reason.
	//
	// Nothing here is new window API -- raising and activating are QWidget's
	// own, so a hand-over needs no change to `main_window` at all. Whether the
	// window actually comes forward is the window manager's decision in the
	// end; every desktop has some form of focus-stealing prevention and this
	// asks rather than insists.
	//
	// **`show()` is deliberately not called, and kiosk mode is why.** Entering
	// a kiosk hides this window on purpose -- the stage is its own fullscreen
	// window -- so showing it here would put the browser's chrome back on
	// screen underneath a kiosk that had just been locked down, in answer to
	// somebody clicking a link in another application. A minimized window is
	// still `isVisible()`, so restoring one does not need it either -- checked
	// on this desktop rather than read off the documentation, which describes
	// a minimize as a spontaneous hide event and leaves the question open:
	// shown 1/0, minimized 1/0, hidden 0/1 for isVisible/isHidden. The only
	// thing `show()` would add is the case that must not happen. The url still
	// lands in the tree, because dropping it is what this whole hand-over
	// exists to stop.
	guard.on_message([&w](const QString &message) {
		if (w.isVisible()) {
			w.setWindowState((w.windowState() & ~Qt::WindowMinimized) |
			                  Qt::WindowActive);
			w.raise();
			w.activateWindow();
		}
		if (!message.isEmpty())
			w.open_url(QUrl(message));
	});
#endif

	w.show();
	return app.exec();
}
