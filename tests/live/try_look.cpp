// A picture of every surface the browser puts in front of somebody.
//
// **For looking at, not for asserting on.** The other drivers check structure:
// that a menu is ordered correctly, that an action exists, that a dialog opens.
// None of them can see that a panel is empty, a label is cut off, a column is
// the wrong width, or that a dialog opens at a size nothing fits in. Those are
// found by looking, and this is what makes looking cheap.
//
// It grabs each widget in-process with `QWidget::grab()` -- never a screen
// capture, never a tool that can grab the X pointer, which froze this desktop
// once. Offscreen is the default and it is honest about what that costs: with
// no platform theme the icon search paths differ, so icons render as Qt's
// built-ins rather than the desktop's. Layout, spacing, wording and empty
// states are faithful; colours and icons are not.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"

#include <QAbstractButton>
#include <QHash>
#include <QApplication>
#include <QDialog>
#include <QTreeView>
#include <QDir>
#include <QEventLoop>
#include <QLineEdit>
#include <QFile>
#include <QTimer>
#include <cstdio>

static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

static QString g_out;
static int     g_shots = 0;

// Two cheap, systematic checks run on every dialog as it is photographed.
//
// **Alt keys, because the menus had two collisions and nobody had looked at the
// dialogs at all.** Qt matches mnemonics case-insensitively and cycles between
// duplicates rather than complaining, so a clash is invisible until somebody
// presses the key and gets the wrong button. And a **window title**, because a
// dialog without one appears in the task switcher as an empty entry.
static int g_problems = 0;
static void audit(QWidget *w, const QString &name) {
	if (!w)
		return;
	if (w->windowTitle().trimmed().isEmpty()) {
		std::printf("    ! %s has no window title\n", qPrintable(name));
		++g_problems;
	}
	// **Only buttons that are on screen together.** The first version of this
	// compared every button in the dialog and reported four clashes in
	// settings, three of which were between *different pages* of a stack --
	// "Remove selected" on Privacy against "Rescan for players" on Media, which
	// cannot both be visible and which Qt would never confuse, since it skips
	// hidden widgets when matching a mnemonic. An audit that cries wolf about
	// pages is an audit somebody turns off.
	QHash<QChar, QString> claimed;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (!b->isVisible())
			continue;
		const QString t = b->text();
		const int amp = t.indexOf('&');
		if (amp < 0 || amp + 1 >= t.size())
			continue;
		const QChar key = t.at(amp + 1).toLower();
		if (claimed.contains(key)) {
			std::printf("    ! %s: Alt+%s is claimed by both \"%s\" and \"%s\"\n",
			             qPrintable(name), qPrintable(QString(key.toUpper())),
			             qPrintable(claimed.value(key)), qPrintable(t));
			++g_problems;
		} else {
			claimed.insert(key, t);
		}
	}
}

static void save(QWidget *w, const QString &name) {
	if (!w)
		return;
	const QString path = QString("%1/%2-%3.png")
	                         .arg(g_out).arg(g_shots, 2, 10, QChar('0')).arg(name);
	audit(w, name);
	if (w->grab().save(path)) {
		std::printf("  %-28s %4dx%-4d %s\n", qPrintable(name),
		             w->width(), w->height(), qPrintable(path));
		++g_shots;
	} else {
		std::printf("  %-28s could not be grabbed\n", qPrintable(name));
	}
}

// Open a modal through its slot, photograph it while it is up, then close it.
// Captured by value; this returns before the dialog exists.
static void shoot_modal(main_window *w, const QString &slot, const QString &name) {
	QTimer::singleShot(900, [name] {
		for (QWidget *x : QApplication::topLevelWidgets()) {
			auto *d = qobject_cast<QDialog *>(x);
			if (!d || !d->isVisible())
				continue;
			save(d, name);
			d->reject();
			return;
		}
		std::printf("  %-28s no dialog appeared\n", qPrintable(name));
	});
	QMetaObject::invokeMethod(w, slot.toUtf8().constData());
	spin(1400);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	g_out = qEnvironmentVariableIsSet("HYDRA_SHOTS")
	            ? QString::fromLocal8Bit(qgetenv("HYDRA_SHOTS"))
	            : QStringLiteral("/tmp/hydra-look");
	QDir().mkpath(g_out);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-look-state");
	QDir(out).removeRecursively();
	QDir().mkpath(out);
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Work\n"
	          "  - [a1] unopened | Qt documentation | https://doc.qt.io | "
	          "created=2026-01-04T09:00:00 | seen=2026-08-01T09:00:00\n"
	          "  - [a2] unopened | A tab with a rather long title that will "
	          "have to be elided somewhere | https://example.test/long | "
	          "created=2026-01-04T09:00:00 | seen=2026-08-01T09:00:00\n"
	          "- [f1] folder | Empty folder\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1100, 720);
	w.show();
	spin(1500);

	std::printf("\n== the window itself ==\n");
	save(&w, "window-wide");

	// The narrow case, where the tree becomes a drawer. Worth its own picture
	// because it is a different layout, not the same one squeezed.
	w.resize(520, 720);
	spin(700);
	save(&w, "window-narrow");
	w.resize(1100, 720);
	spin(500);

	// The properties editor is reached from the tree rather than a menu, so it
	// needs its own opening: `edit_properties` is public on the view and blocks
	// like any other modal.
	// The two ways the tree can be empty, which look identical and mean
	// opposite things.
	std::printf("\n== an empty tree, for both reasons ==\n");
	{
		QLineEdit *search = nullptr;
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText().contains("Search"))
				search = e;
		if (search) {
			search->setText("zzzznothingmatchesthis");
			spin(500);
			save(&w, "tree-no-match");
			search->clear();
			spin(400);
		} else {
			std::printf("  no search box found\n");
		}
	}

	std::printf("\n== the tab properties editor ==\n");
	{
		auto *tv    = w.findChild<tab_tree_view *>();
		auto *model = w.findChild<tab_tree_model *>();
		if (tv && model && !model->root()->children.isEmpty()) {
			node *folder = model->root()->children.first();
			node *tab = folder->children.isEmpty() ? folder
			                                        : folder->children.first();
			QTimer::singleShot(900, [] {
				for (QWidget *x : QApplication::topLevelWidgets()) {
					auto *d = qobject_cast<QDialog *>(x);
					if (!d || !d->isVisible())
						continue;
					save(d, "properties");
					d->reject();
					return;
				}
				std::printf("  %-28s no dialog appeared\n", "properties");
			});
			tv->edit_properties(tab);
			spin(1400);
		}
	}

	std::printf("\n== the dialogs ==\n");
	struct { const char *slot; const char *name; } modals[] = {
		{ "open_settings",       "settings" },
		{ "open_site_controls",  "site-controls" },
		{ "open_downloads",      "downloads" },
		{ "open_media",          "media" },
		{ "open_site_rules",     "site-rules" },
		{ "open_reorganizer",    "reorganizer" },
	};
	for (const auto &m : modals)
		shoot_modal(&w, m.slot, m.name);

	// **The surfaces that need a page**, which is why they were blank or absent
	// in the first pass: the media dialog lists what a page is playing, and the
	// extractor works from the requests a page actually made. Both are empty by
	// construction on an empty tab, so photographing them there says nothing
	// about how they look in use.
	const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
	if (target.isEmpty()) {
		std::printf("\n(no url given; the media and extractor dialogs need a "
		             "loaded page -- pass one to include them)\n");
	} else {
		std::printf("\n== with %s loaded ==\n", qPrintable(target));
		node *tab = w.findChild<tab_tree_model *>()->add_tab(nullptr, "Live",
		                                                      target);
		if (tab) {
			auto *tv = w.findChild<QTreeView *>();
			tv->expandAll();
			// The tab was appended at the root, so it is the last top-level row.
			const int last = tv->model()->rowCount() - 1;
			emit tv->activated(tv->model()->index(last, 0));
			spin(qEnvironmentVariableIsSet("HYDRA_SETTLE")
			         ? qEnvironmentVariableIntValue("HYDRA_SETTLE") : 15000);
			save(&w, "window-page-loaded");
			shoot_modal(&w, "open_media", "media-loaded");
			// The extractor probes its candidates when it opens, so it wants
			// longer on screen than a dialog that merely draws itself.
			shoot_modal(&w, "learn_this_site", "extractor-loaded");
			// The report a person files in one click, with the evidence a real
			// page produced -- the only state in which its list means anything.
			shoot_modal(&w, "report_annoyance", "annoyed-loaded");
			// Filter evolution before anything is sent: the review-first half,
			// which is what somebody sees for as long as they are deciding.
			shoot_modal(&w, "open_filter_evolution", "filters-loaded");
		} else {
			std::printf("  could not add a tab for %s\n", qPrintable(target));
		}
	}

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	std::printf("%d problem(s) found by the audit\n", g_problems);
	// **The word the sweep looks for, and an exit code that means something.**
	// This printed neither, so a driver whose pictures all came out was
	// reported as "did not finish" in every sweep -- and had the audit found a
	// clash, the run would still have exited 0 and said nothing that a script
	// could see. The pictures remain for a person; the audit is a test.
	std::printf("done\n");
	return g_problems ? 1 : 0;
}
