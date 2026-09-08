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
// once. Offscreen is the default and it is honest about what that costs:
// layout, spacing, wording and empty states are faithful, colours are not.
//
// **The icons are faithful now, and the sentence that used to sit here gave
// the wrong reason for their not being.** It blamed the platform theme --
// "with no platform theme the icon search paths differ, so icons render as
// Qt's built-ins" -- when the cause was that `icon/hydra.qrc` was compiled
// into the application by qmake and into no test binary at all. Every driver
// therefore fell back to the host theme, which is the exact dependency the
// bundled svgs were added to remove, and offscreen that meant Qt's built-ins;
// the shield has no built-in on Linux, so the toolbar drew the word "Shield"
// beside seven pictures for months and this file photographed it.
//
// `test/Makefile` links the resource into the live drivers now. What is left
// of the original caveat is real and much smaller: a *theme* icon, for any
// caller that passes no bundled name, is still the offscreen default rather
// than the desktop's.
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
#include <QTreeWidget>
#include "consent_blocker.h"
#include "consent_dialog.h"
#include "webauth_dialog.h"
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QComboBox>
#include <QLayout>
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
static int g_combos_seen  = 0;
// **Does the current item fit the field the style draws it in?**
//
// Factored out so that the control below runs the same comparison the audit
// runs. A check written twice is a check whose control tests the other copy.
static bool combo_fits(QComboBox *c, int *need, int *room) {
	QStyleOptionComboBox opt;
	opt.initFrom(c);
	opt.editable = c->isEditable();
	*room = c->style()
	            ->subControlRect(QStyle::CC_ComboBox, &opt,
	                              QStyle::SC_ComboBoxEditField, c)
	            .width();
	*need = c->fontMetrics().horizontalAdvance(c->currentText());
	return *need <= *room + 4;
}

// **The combo check has a way of being permanently silent, so it is made to
// speak before anything is audited.**
//
// `SC_ComboBoxEditField` is the style's answer, not this driver's. A style
// that returned the whole widget rect for it -- or a Qt that changed what the
// sub-control means -- would leave `need > room` unable to fire, and the audit
// would report zero combo problems for ever in exactly the words it uses when
// there are none. The count in the summary proves the loop ran; it cannot
// prove the comparison inside it can come out false.
//
// Two fixtures, because one of them only shows the check is capable of
// refusing and the other that it does not refuse everything: a box far too
// narrow for its item must be reported, and the same box given room must not.
// A control that can only fail one way is a constant.
//
// It refuses the run rather than counting a problem. A control failure means
// no result below means anything, which is not something to report as a
// finding among the findings.
static bool combo_control() {
	QComboBox narrow;
	narrow.addItem("a considerably longer item than this box can show");
	narrow.resize(40, narrow.sizeHint().height());
	int need = 0, room = 0;
	if (combo_fits(&narrow, &need, &room)) {
		std::printf("control: a %dpx combo showing a %dpx item was called a "
		             "fit (field %dpx) -- the combo check cannot refuse "
		             "anything, so no result below means anything\n",
		             narrow.width(), need, room);
		return false;
	}
	QComboBox roomy;
	roomy.addItem("short");
	roomy.resize(400, roomy.sizeHint().height());
	if (!combo_fits(&roomy, &need, &room)) {
		std::printf("control: a 400px combo showing a %dpx item was called a "
		             "cut-off (field %dpx) -- the combo check refuses "
		             "everything, so no result below means anything\n",
		             need, room);
		return false;
	}
	return true;
}

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
	// **Which buttons are greyed, said in words.**
	//
	// A picture cannot answer it. Offscreen, a disabled button differs from an
	// enabled one by a shade that does not survive being looked at -- the
	// downloads dialog's five actions were read as "enabled with nothing
	// selected", which would have been this project's own rule broken, and
	// they are correctly disabled. Reading the code settled it; the log should
	// have. Not a problem count: this is the state of the surface, printed so
	// that whoever reads the picture next does not have to guess.
	QStringList greyed;
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>())
		if (b->isVisible() && !b->isEnabled())
			greyed << b->text().remove('&');
	if (!greyed.isEmpty())
		std::printf("      greyed: %s\n", qPrintable(greyed.join(", ")));

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

	// **The sentence that says where the payload is going, still saying it.**
	//
	// The three review dialogs put it in a label of its own precisely because
	// it used to share `m_status` with the working line -- and the extractor
	// probes its candidates the moment it opens, so the provider sentence was
	// replaced by a count of addresses before anybody could read it. That is
	// invisible to every structural check: the label was present, correctly
	// worded when it was set, and showing something else by the time the
	// screen settled.
	//
	// Checked here because this driver photographs each surface *after* it has
	// settled, which is the only moment the question can be asked.
	//
	// **Visible, not merely present.** The first version asked only about the
	// text, and the sabotage that was supposed to prove it -- pointing the
	// note at the status label -- left the original widget in place with its
	// name and its correct text, hidden. `findChild` returned that one and the
	// audit passed. A privacy sentence nobody can see is the same as an absent
	// one, so the check now asks the question a reader would.
	if (QLabel *note = w->findChild<QLabel *>("provider_note")) {
		if (!note->isVisible() || !note->text().contains("provider")) {
			std::printf("    ! %s: the provider note says \"%s\"\n",
			             qPrintable(name),
			             qPrintable(note->text().left(60)));
			++g_problems;
		}
	}

	// **A button whose own text does not fit it**, which the label rule above
	// cannot see and which is the same failure: Qt elides the text and the
	// button still looks like a button. It matters most at a phone's width,
	// where "Restore Privacy & security defaults" is the widest string this
	// dialog can put on a control -- the name of the page is in it, so the
	// worst case belongs to whichever page has the longest name rather than to
	// anything anybody sized.
	//
	// Same four-pixel margin and the same reason as below.
	for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
		if (!b->isVisible() || b->text().trimmed().isEmpty())
			continue;
		if (b->sizeHint().width() > b->width() + 4) {
			std::printf("    ! %s: \"%s\" needs %dpx and has %d\n",
			             qPrintable(name), qPrintable(b->text().remove('&')),
			             b->sizeHint().width(), b->width());
			++g_problems;
		}
	}

	// **The class the audit was not looking at.** It counted buttons and
	// labels, so a `QComboBox` could elide its current item and be reported as
	// nothing at all -- the same shape as the address bar taking whatever the
	// toolbar's buttons left over, met in a different widget.
	//
	// The comparison is not `sizeHint()`, which for a combo is the widest item
	// it holds: a deliberately narrow box with one long entry would be
	// reported for ever, and a check nobody can satisfy is a check that gets
	// ignored. What a person cannot read is the CURRENT item, so this measures
	// that string against the field the style actually draws it in -- which is
	// `SC_ComboBoxEditField`, arrow and frame already taken out.
	//
	// Same four-pixel margin as above, and the same reason: Qt rounds font
	// metrics, and one pixel over is arithmetic rather than a cut-off word.
	for (QComboBox *c : w->findChildren<QComboBox *>()) {
		if (!c->isVisible() || c->currentText().trimmed().isEmpty())
			continue;
		++g_combos_seen;
		int need = 0, room = 0;
		if (!combo_fits(c, &need, &room)) {
			std::printf("    ! %s: combo \"%s\" needs %dpx and has %d\n",
			             qPrintable(name), qPrintable(c->currentText()),
			             need, room);
			++g_problems;
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
// `narrow` resizes the dialog before capturing it, for the surfaces that
// change shape rather than merely getting smaller. The window is photographed
// at two widths already; a dialog with a layout switch in it deserves the
// same, and the settings page has one -- below `k_narrow_threshold` its
// category list becomes a dropdown, which is a code path nothing had ever
// looked at.
static void shoot_modal(main_window *w, const QString &slot, const QString &name,
                         QSize narrow = QSize()) {
	QTimer::singleShot(900, [name, narrow] {
		for (QWidget *x : QApplication::topLevelWidgets()) {
			auto *d = qobject_cast<QDialog *>(x);
			if (!d || !d->isVisible())
				continue;
			if (narrow.isValid()) {
				d->resize(narrow);
				// The layout switch happens on a resize event, so the dialog
				// has to be let run before it is worth photographing.
				QApplication::processEvents();
				spin(200);
				// **Twice.** The first resize is clamped by the minimum the
				// dialog has in its *wide* layout; switching to the narrow one
				// lowers that minimum, and nothing re-applies the request. One
				// resize therefore photographs a dialog wider than it was
				// asked for -- and reads as a floor it does not have.
				d->resize(narrow);
				QApplication::processEvents();
			}
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

	if (!combo_control())
		return 2;

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

	// The settings dialog again, at a phone's width, where its category list
	// stops being a sidebar.
	shoot_modal(&w, "open_settings", "settings-narrow", QSize(380, 700));

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
		// **The same dialog with the warning in it**, which the secure shot
		// above cannot show. It is not a rare state: Android's WebView reports
		// a challenge with a host and a realm and nothing else, so anything
		// not provably on the page's own https origin is drawn like this --
		// and a printer or a router asking for a password over plain HTTP is
		// exactly what that path meets.
		auth_dialog plain("printer.lan", "Configuration", false, &w);
		plain.show();
		QApplication::processEvents();
		save(&plain, "auth-site-insecure");
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

	// **Two dialogs this audit had never seen.** Comparing what it
	// photographs against the dialog classes in `src/`, `consent_dialog` and
	// `webauth_dialog` were absent rather than excluded -- neither name
	// appeared anywhere in this file. `try_phone` measures the passkey dialog
	// and now the cookie one, but that driver asks whether a thing fits a
	// phone; this one asks whether its words are readable and its mnemonics
	// unique, which is a different question and was never put.
	{
		webauth_dialog wa("login.beispiel.invalid", &w);
		// The account list, because it is the state with the most text in it
		// and the one whose stretch was got wrong once already.
		wa.ask_for_account({ "ada@beispiel.invalid",
		                      "ada.lovelace.work@ein-sehr-langer-name.invalid",
		                      "Sicherheitsschlüssel (NFC)" });
		wa.show();
		QApplication::processEvents();
		save(&wa, "webauth-account");
	}
	{
		// Filled, for the reason the annoyance and consent fixtures give: an
		// empty list makes any layout look fine. Recorded through the blocker
		// rather than poked into the widget, so what is drawn is what the
		// dialog builds from a real one -- and asserted below, because
		// `report_unhandled` returns silently when the blocker is not active
		// for the host and would leave this an empty window.
		if (consent_blocker *b = w.m_consent) {
			b->set_page_host("nachrichten.beispiel.invalid");
			b->report_unhandled("Alle akzeptieren | Nur notwendige Cookies | "
			                     "Einstellungen verwalten");
			b->set_page_host("aviser.eksempel.invalid");
			b->report_unhandled("Godta alle | Avvis alle | Administrer valg");
			consent_dialog cd(b, g_out + "/look-consent-rules.json", &w);
			cd.show();
			QApplication::processEvents();
			QTreeWidget *rows = cd.findChild<QTreeWidget *>("banners");
			if (!rows || rows->topLevelItemCount() < 2) {
				std::printf("    ! consent: %d banner(s), so the audit below "
				             "would be of an empty dialog\n",
				             rows ? rows->topLevelItemCount() : -1);
				++g_problems;
			}
			save(&cd, "consent");
		} else {
			std::printf("    ! consent: no blocker on the window\n");
			++g_problems;
		}
	}

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	std::printf("%d problem(s) found by the audit of %d surface(s), "
	             "%d button(s), %d label(s) and %d combo(s)\n",
	             g_problems, g_shots, g_buttons_seen, g_labels_seen,
	             g_combos_seen);

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
