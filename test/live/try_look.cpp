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
#include "auth_dialog.h"
#include "cert_dialog.h"
#include "permission_dialog.h"
#include "screen_picker.h"
#include "web_view_backend.h"
#include <QStringListModel>
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "settings_dialog.h"   // settings_store
#include "media_fixture.h"
#include "theme.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"

#include <QAbstractButton>
#include <QHash>
#include <QApplication>
#include <QDialog>
#include <QLabel>
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
static int     g_missed = 0;   // surfaces that would not grab

// Two cheap, systematic checks run on every dialog as it is photographed.
//
// **Alt keys, because the menus had two collisions and nobody had looked at the
// dialogs at all.** Qt matches mnemonics case-insensitively and cycles between
// duplicates rather than complaining, so a clash is invisible until somebody
// presses the key and gets the wrong button. And a **window title**, because a
// dialog without one appears in the task switcher as an empty entry.
static int g_problems = 0;
// **What the audit actually looked at.** Zero problems over zero widgets reads
// exactly like zero problems over four hundred, and only one of those is a
// result -- the same trap as a capture run that photographed nothing. The
// counts are printed with the verdict so the verdict means something.
static int g_buttons_seen = 0;
static int g_labels_seen  = 0;
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
		++g_buttons_seen;
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

	// **Text that does not fit the space it was given.** A label narrower than
	// its own `sizeHint()` is drawn cut off or elided, which is the exact
	// failure this driver exists to catch and the one no structural check can:
	// the label is present, correctly worded and in the right place, and the
	// half a person needs is not on screen.
	//
	// Word-wrapped labels are skipped, because for them a narrow width is the
	// point -- they grow taller instead. So are empty ones, and any label whose
	// height already exceeds one line, which is a wrapped paragraph however it
	// was configured.
	//
	// **A margin of four pixels rather than one.** Qt rounds font metrics, and
	// a hint one pixel over the width is not a cut-off label, it is arithmetic.
	// The first pass of this reported nine surfaces and every one of them was
	// rounding, which is how an audit teaches people to ignore it.
	for (QLabel *l : w->findChildren<QLabel *>()) {
		if (!l->isVisible() || l->wordWrap() || l->text().trimmed().isEmpty())
			continue;
		++g_labels_seen;
		const QSize hint = l->sizeHint();
		if (hint.height() > l->height())
			continue;               // already wrapping, whatever it says
		if (hint.width() > l->width() + 4) {
			std::printf("    ! %s: \"%s\" needs %dpx and has %d\n",
			             qPrintable(name), qPrintable(l->text().simplified()),
			             hint.width(), l->width());
			++g_problems;
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
		// **Counted, not merely mentioned.** A line on stdout is not a result:
		// the run below decided its exit code from the audit alone, so every
		// grab could fail and the driver would still say "done" and exit 0.
		++g_missed;
		std::printf("  %-28s could not be grabbed (%s)\n", qPrintable(name),
		             qPrintable(g_out));
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
	          "  - [a1] unopened | Qt documentation | https://docs.example.test/qt | "
	          "created=2026-01-04T09:00:00 | seen=2026-08-01T09:00:00\n"
	          "  - [a2] unopened | A tab with a rather long title that will "
	          "have to be elided somewhere | https://example.test/long | "
	          "created=2026-01-04T09:00:00 | seen=2026-08-01T09:00:00\n"
	          "- [f1] folder | Empty folder\n");
	tf.close();

	// **The colour scheme first, the way `main()` does it.** This driver builds
	// its own window rather than using `shell_fixture` -- it predates it -- so
	// the fixture's copy of this does not reach here, and the two are now the
	// same line in two places. Worth collapsing when something else brings this
	// driver onto the fixture.
	//
	// Without it the captures were a browser nobody runs: nothing applied a
	// scheme, so the first five surfaces came out in Qt's default light palette
	// and the sixth onwards in the desktop's dark one -- the flip being the
	// settings dialog's Cancel, which restores the stored setting and was the
	// first thing all run to ask the desktop what it wanted.
	theme::apply(settings_store::appearance());
	// **And the icon theme, which is the other half.** Applying the palette
	// alone gave a dark window wearing the light theme's icons: on this desktop
	// that made the locked-tab padlock a dark glyph on a dark row, almost
	// invisible, where the same icon reads clearly on light. `main()` calls both
	// and a driver that calls one photographs a mismatch no user has.
	theme::apply_icon_theme(theme::resolve(settings_store::appearance()));

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
	//
	// **So the local fixture, unless a real site is named.** These four surfaces
	// were skipped on every run that did not name a url, which is every run --
	// so the dialogs that need a page were the ones nobody ever looked at, and
	// two of the defects this file exists to catch were found in them long
	// after they were written. The fixture serves a page, a player and a
	// manifest from 127.0.0.1, so it needs no network.
	//
	// A `file://` url will not do, and that is why the fixture rather than a
	// local html file: the annoyance report and the extractor both key on the
	// site host, and `file://` has none, so both correctly refuse and neither
	// gets photographed.
	media_fixture::server fixture;
	const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                                : fixture.start();
	if (target.isEmpty()) {
		std::printf("\n(the fixture did not start, and no url was given)\n");
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

	// **The four the network and a page put in front of somebody**, which no
	// slot reaches and which this driver had therefore never photographed or
	// audited at desktop size. try_phone builds them to measure them at 360
	// pixels; the same four want the mnemonic, window-title and cut-label
	// checks run over them at the size most people will actually meet them.
	//
	// Built directly and shown rather than exec'd, which is what try_phone and
	// try_chrome both do with these: a modal event loop blocks the driver.
	std::printf("\n== and the ones no menu opens ==\n");
	{
		auth_dialog site("bank.example", "Accounts", true, &w);
		site.show();
		QApplication::processEvents();
		save(&site, "auth-site");
	}
	{
		auth_dialog proxy("proxy.corp.example", "Staff", false, &w,
		                   auth_dialog::asker::proxy);
		proxy.show();
		QApplication::processEvents();
		save(&proxy, "auth-proxy");
	}
	{
		permission_dialog cam("meet.example", policy::feature::camera, true, &w);
		cam.show();
		QApplication::processEvents();
		save(&cam, "permission-camera");
	}
	{
		QStringListModel screens({"Screen 1 (built-in, 1920x1080)",
		                           "Screen 2 (external)"});
		QStringListModel windows({"Hydra — a tab that is open", "A terminal",
		                           "Something with a very long window title that "
		                           "a narrow screen has to do something sensible "
		                           "with"});
		screen_picker picker("meet.example", &screens, &windows, &w);
		picker.show();
		QApplication::processEvents();
		save(&picker, "screen-picker");
	}
	{
		QList<web_view_backend::certificate_offer> offered;
		web_view_backend::certificate_offer a;
		a.subject = "Ada Lovelace";
		a.issuer  = "Example Certification Authority";
		a.valid_until = "2027-01-01";
		web_view_backend::certificate_offer b;
		b.subject = "Ada (work)";
		b.issuer  = "Corp CA";
		b.valid_until = "2026-09-01";
		offered << a << b;
		cert_dialog cert("id.example", offered, &w);
		cert.show();
		QApplication::processEvents();
		save(&cert, "certificate");
	}

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	std::printf("%d problem(s) found by the audit of %d surface(s), "
	             "%d button(s) and %d label(s)\n",
	             g_problems, g_shots, g_buttons_seen, g_labels_seen);

	// **A run that photographed nothing is a failed run, not a clean one.**
	//
	// This said "0 image(s)", "0 problem(s) found by the audit" and "done", and
	// exited 0 -- a full green result from a run that captured nothing whatever.
	// The cause was mundane and will recur: HYDRA_SHOTS was unset, so it wrote
	// to /tmp/hydra-look, which belongs to whoever ran it first, and every grab
	// failed silently for the next person. The sweep counts a driver that
	// reaches the end as fine, so this went past both the exit code and the
	// summary line.
	//
	// An audit over an empty set reports success exactly as loudly as a real
	// one. So the count is the evidence now: no pictures, no pass.
	if (g_shots == 0) {
		std::printf("FAIL  nothing was photographed -- is %s writable?\n",
		             qPrintable(g_out));
		std::printf("done\n");
		return 1;
	}
	if (g_missed) {
		std::printf("FAIL  %d surface(s) could not be grabbed\n", g_missed);
		std::printf("done\n");
		return 1;
	}
	// **The word the sweep looks for, and an exit code that means something.**
	// This printed neither, so a driver whose pictures all came out was
	// reported as "did not finish" in every sweep -- and had the audit found a
	// clash, the run would still have exited 0 and said nothing that a script
	// could see. The pictures remain for a person; the audit is a test.
	std::printf("done\n");
	return g_problems ? 1 : 0;
}
