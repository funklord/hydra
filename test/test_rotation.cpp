// Turning the phone, and opening the fold (architecture doc sec 8 / sec 19).
//
// Both reach the app as one thing: a window that changed shape. Nothing else
// happens. `android/AndroidManifest.xml` declares
// `orientation|uiMode|screenLayout|screenSize|smallestScreenSize|density` and
// more in `configChanges`, so Android RESIZES the window instead of destroying
// and recreating the activity -- there is no save, no restore, and nothing is
// serialised, because nothing is torn down. That is what keeps a rotation from
// costing the user anything, and it is also why the only signal is a resize
// that something has to be listening for.
//
// `main_window::update_layout_mode` is that listener, and until this file
// nothing in the tree exercised it. The branch it takes reparents `m_sidebar`
// out of the splitter and onto the window, shows `m_drawer_action`, and
// rewrites the empty-page text -- three things one edit away from silently
// regressing, on a code path no suite ran.
//
// WHAT THIS ASSERTS, AND WHY IT IS NOT THE OBVIOUS THING. It asserts the
// widget POINTERS across the shapes, not the values in them. A sidebar that
// was destroyed and rebuilt would look identical from the outside and would
// have thrown away the tree's selection, its scroll position and every open
// branch. Values survive a rebuild that restores them; pointers do not.
//
// It also asserts that the drawer mode actually CHANGES, so a resize that
// never reached the window fails here rather than reporting cheerfully that
// nothing was lost.
//
// The sizes are a foldable's, in logical pixels: 360x800 and 800x360 folded,
// 674x841 and 841x674 open. A desktop window dragged between those sizes is
// the same event, which is why none of this needs a device.
#include "main_window.h"
#include "settings_dialog.h"
#include "site_rules.h"
#include "kiosk_controller.h"
#include "tab_tree_view.h"
#include "policy_engine.h"
#include "consent_blocker.h"
#include "consent_dialog.h"
#include "settings_dialog.h"
#include "annoyance_log.h"
#include "request_filter.h"
#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QSplitter>
#include <QTimer>
#include <cstdio>
#include <unistd.h>   // geteuid, for the check root cannot fail

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// A view that is only a widget. Copied from `test_kiosk.cpp` rather than
// shared: the two files want the double for opposite reasons -- kiosk borrows
// the widget and must give it back, this one never opens a tab at all -- and a
// header holding both would have to grow whichever of them changed first.
class fake_view : public web_view_backend {
public:
	explicit fake_view(QWidget *parent = nullptr) : web_view_backend(nullptr) {
		m_widget = new QLabel("page", parent);
		setParent(m_widget);
	}

	QWidget *widget() override { return m_widget; }
	QUrl url() const override { return m_url; }
	void load(const QUrl &u) override { m_url = u; }
	void back() override {}
	void forward() override {}
	void reload() override {}
	// Recorded rather than dropped, so a test can ask what the shell actually
	// derived from the policy. It was discarded here, which meant nothing in
	// the suite could see `apply_policy`'s output at all.
	view_settings last_settings;
	int settings_applied = 0;
	void apply_settings(const view_settings &s) override {
		last_settings = s;
		++settings_applied;
	}
	void set_permission_decider(permission_decider) override {}
	void set_capture_chooser(capture_chooser) override {}
	void set_zoom_factor(double) override {}
	void inject_script(const QString &, const QString &, bool) override {}
	void inject_main_world_script(const QString &, const QString &) override {}
	void set_script_bridge(QObject *, const QString &) override {}
	QByteArray save_state() const override { return {}; }
	bool restore_state(const QByteArray &) override { return false; }

	QLabel *m_widget = nullptr;
	QUrl    m_url;
};

// The seam sec 19.2 exists for: the shell holds the interface, so a suite can
// build the whole window without an engine behind it.
class fake_factory : public web_view_factory {
public:
	web_view_backend *create_view(QWidget *parent) override {
		++made;
		return new fake_view(parent);
	}
	void set_external_url_handler(external_url_handler) override {}
	void set_download_handler(download_note) override {}
	void clear_browsing_data(const browsing_data &, clear_note done) override {
		if (done) done(clear_report{});
	}

	int made = 0;
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	policy_engine  policy;
	request_filter filter(&policy);
	fake_factory   factory;

	main_window w(&factory, &policy, &filter);

	// **Android hands a window its size; it does not ask.** A desktop Qt
	// window refuses to go below its layout's minimum, so without this the
	// window stays at whatever its contents demand and every assertion below
	// is about a desktop pretending to be a phone. Found the hard way in
	// bbq-predictor, where the first version of the equivalent test turned a
	// window that was still 1662 pixels wide and passed.
	if (w.layout()) w.layout()->setSizeConstraint(QLayout::SetNoConstraint);
	w.setMinimumSize(0, 0);

	w.resize(360, 800);
	w.show();
	spin(120);

	section("what a clear actually clears");
	{
		// **The caches no backend can reach.** `clear_browsing_data` covers
		// cookies, the cache and visited links, and goes straight from the
		// settings page or from kiosk mode to the view factory -- so these
		// two, which live on the window, survived every clear there was.
		//
		// `m_session_permissions` is the one that matters: it holds answers
		// given by somebody who chose *not* to have them remembered, and a
		// hit in it skips the prompt. On a public screen that meant the next
		// person inherited the last person's camera.
		w.m_session_permissions.insert("example.test\ncamera", true);
		// An automatic allowance and the note that it was made, as the
		// antiadblock path leaves them: the note alone is not the state that
		// changed behaviour.
		w.m_antiadblock_fixed.insert("example.test");
		policy.set_setting("example.test", policy::feature::ads,
		                    policy::setting::allow);
		// And one the person set themselves, which forgetting must not touch.
		policy.set_setting("chosen.test", policy::feature::ads,
		                    policy::setting::block);
		check(!w.m_session_permissions.isEmpty() && !w.m_antiadblock_fixed.isEmpty(),
		      "a session answer and a fixed host are cached");
		// **Driven through the wiring, not by calling the slot.** The slot is
		// private and stays private -- widening it for a test would be
		// testing the easy half, and the wiring is where the defect was:
		// both clear paths went straight to the view factory and nothing
		// told the window anything. `toggle_kiosk` builds the controller and
		// makes the connection; emitting its signal is what the idle timer,
		// entering and leaving all do.
		// Through the menu action, which is how a person reaches it and needs
		// no access this test has no business having.
		if (w.m_kiosk_action)
			w.m_kiosk_action->trigger();
		spin(80);
		check(w.m_kiosk != nullptr, "kiosk mode built its controller");
		if (w.m_kiosk)
			emit w.m_kiosk->session_forgotten();
		spin(20);
		check(w.m_session_permissions.isEmpty(),
		      "forgetting drops the session permission answers");
		check(w.m_antiadblock_fixed.isEmpty(),
		      "and the record of which hosts were found running detection");
		check(policy.setting_for("example.test", policy::feature::ads) ==
		       policy::setting::unset,
		      "and the allowance itself, which is what changed behaviour");
		check(policy.setting_for("chosen.test", policy::feature::ads) ==
		       policy::setting::block,
		      "while a rule the person set themselves is left alone");
		if (w.m_kiosk_action)
			w.m_kiosk_action->trigger();
		spin(80);
	}

	section("what a screen reader would be told these controls are");
	{
		// **Names, because Qt has no useful default for these.** A toolbar
		// button takes its accessible name from the action's text, and a bare
		// QLineEdit or QTreeView has none at all -- so before this the drawer
		// button announced as a hamburger glyph, U+2630, and the address bar
		// and the tab
		// tree announced as an unnamed edit box and an unnamed tree.
		//
		// Asserted rather than trusted: the whole gap was invisible for as
		// long as it existed because nothing here ever asked.
		check(w.m_address && w.m_address->accessibleName() == "Address",
		      QString("the address bar is named (\"%1\")")
		          .arg(w.m_address ? w.m_address->accessibleName() : "none"));
		check(w.m_address && !w.m_address->accessibleDescription().isEmpty(),
		      "and says what it is for, which a name alone does not");
		check(w.m_tree && w.m_tree->accessibleName() == "Tab tree",
		      QString("the tab tree is named (\"%1\")")
		          .arg(w.m_tree ? w.m_tree->accessibleName() : "none"));
		// The drawer button carries its name as the action's text, which is
		// where Qt looks and which the icon-only toolbar never draws.
		check(w.m_drawer_action && w.m_drawer_action->text() == "Tab tree",
		      QString("the drawer button is named rather than a glyph (\"%1\")")
		          .arg(w.m_drawer_action ? w.m_drawer_action->text() : "none"));
	}

	section("a narrow window puts the sidebar in a drawer");

	check(w.width() == 360,
	      QString("the window took the size it was given (%1 wide)").arg(w.width()));
	check(w.m_drawer_mode,
	      "360 is below the 620 threshold, so the sidebar is an overlay");
	check(w.m_sidebar != nullptr, "there is a sidebar to move");
	check(w.m_drawer_action && w.m_drawer_action->isVisible(),
	      "the button that reveals it is on the toolbar");

	// Everything whose identity must survive a reshape. The sidebar is the
	// one that moves; the rest are what it carries.
	// `auto`, because main_window.h forward-declares the tree, its model and
	// its proxy: the identities are all this file needs and the complete
	// types would only be there to name them.
	QWidget *const sidebar  = w.m_sidebar;
	auto *const model       = w.m_model;
	auto *const proxy       = w.m_proxy;
	auto *const tree        = w.m_tree;
	QSplitter *const split  = w.m_splitter;
	QLineEdit *const search = w.m_search;

	// A piece of state a person would notice losing.
	const QString typed = QStringLiteral("example.org/some/page");
	if (w.m_address) w.m_address->setText(typed);

	section("turning and unfolding, four shapes");

	struct shape {
		int  width;
		int  height;
		bool drawer;
		const char *what;
	};
	const shape shapes[] = {
		{ 800, 360, false, "folded, turned sideways" },
		{ 674, 841, false, "unfolded, held upright" },
		{ 841, 674, false, "unfolded, turned sideways" },
		{ 360, 800, true,  "folded again, back upright" },
	};

	for (const shape &next : shapes) {
		w.resize(next.width, next.height);
		spin(60);

		check(w.m_drawer_mode == next.drawer,
		      QString("%1: %2x%3 wants drawer=%4, the window says %5")
		              .arg(QString::fromUtf8(next.what))
		              .arg(next.width).arg(next.height)
		              .arg(next.drawer).arg(w.m_drawer_mode));

		check(w.m_sidebar == sidebar,
		      QString("%1: the sidebar was reparented, not rebuilt")
		              .arg(QString::fromUtf8(next.what)));
		check(w.m_model == model && w.m_proxy == proxy && w.m_tree == tree,
		      QString("%1: the tree, its model and its proxy are the same objects")
		              .arg(QString::fromUtf8(next.what)));
		check(w.m_splitter == split,
		      QString("%1: the splitter survived").arg(QString::fromUtf8(next.what)));
		check(w.m_search == search,
		      QString("%1: the search field survived").arg(QString::fromUtf8(next.what)));

		check(!w.m_address || w.m_address->text() == typed,
		      QString("%1: the address bar still holds what was typed")
		              .arg(QString::fromUtf8(next.what)));

		// **This asserted the opposite until the button became a toggle.**
		// It was shown only on a window narrow enough to put the tree in a
		// drawer, so on a desktop there was no way to get the pane out of the
		// way -- the splitter could be dragged shut, which is a different
		// offer and does not come back. It is shown at every width now, and
		// what changes with the mode is what it is checked to.
		check(w.m_drawer_action && w.m_drawer_action->isVisible(),
		      QString("%1: the tab-tree button is there at this size")
		              .arg(QString::fromUtf8(next.what)));
		check(w.m_drawer_action
		              && w.m_drawer_action->isChecked() != next.drawer,
		      QString("%1: and is checked when the tree is showing (drawer=%2, "
		               "checked=%3)")
		              .arg(QString::fromUtf8(next.what)).arg(next.drawer)
		              .arg(w.m_drawer_action && w.m_drawer_action->isChecked()));

		// Whichever mode it is in, the sidebar has to be somewhere the window
		// can show it: in the splitter when wide, on the window when narrow.
		QWidget *const parent = sidebar->parentWidget();
		check(next.drawer ? parent == &w : parent == split,
		      QString("%1: the sidebar is parented where that mode keeps it")
		              .arg(QString::fromUtf8(next.what)));
	}

	section("what happens to an open drawer when the window changes");

	// TWO CASES, and the first version of this file asserted the wrong one.
	//
	// Leaving drawer mode is not a rotation the drawer survives: entering
	// narrow mode calls `set_drawer_open(false, false)`, so a window that goes
	// wide and comes back starts closed by design. The first version asserted
	// the drawer was still on screen after that round trip, found it parked at
	// x=-295..-1, and would have reported a defect. -295 is the closed
	// position for a 360-wide window: `move(m_drawer_open ? 0 : -w, top)`.
	// The test was wrong, not the window.
	//
	// The case `resizeEvent` actually names in its comment -- "a rotation
	// while it is open must not leave it half off" -- is a width change that
	// STAYS narrow, where the drawer is not closed and has to be repositioned
	// by hand. That is the one nothing had run.
	w.resize(360, 800);
	spin(60);
	check(w.m_drawer_mode, "back to a narrow window");

	if (w.m_drawer_action) w.m_drawer_action->trigger();
	spin(400);   // the drawer slides
	check(w.m_drawer_open, "the drawer opened");

	{
		const QRect open_at = w.m_sidebar->geometry();
		check(open_at.left() == 0,
		      QString("an open drawer starts at the left edge: x=%1").arg(open_at.left()));

		// Still narrow, so the drawer stays open and must follow the window.
		// 500 is under the 620 threshold; a foldable's cover screen and a
		// tiled half-window both land in this range.
		w.resize(500, 700);
		spin(200);

		check(w.m_drawer_mode, "500 is still narrow, so this is not a mode change");
		check(w.m_drawer_open, "and the drawer is still open");

		const QRect after = w.m_sidebar->geometry();
		check(after.left() == 0 && after.right() < w.width(),
		      QString("the open drawer is fully on screen after the change: "
		              "x=%1..%2 in a window %3 wide")
		              .arg(after.left()).arg(after.right()).arg(w.width()));
		check(after.width() == qMin(int(w.width() * 0.82), 420),
		      QString("and took the new width's share: %1 of %2")
		              .arg(after.width()).arg(w.width()));
		check(after.bottom() <= w.height(),
		      QString("and does not run off the bottom: y=%1..%2 of %3")
		              .arg(after.top()).arg(after.bottom()).arg(w.height()));
	}

	section("leaving drawer mode closes it, and it can be opened again");

	{
		// The behaviour the first version mistook for a defect, asserted as
		// what it is -- so a change that started leaving the drawer open
		// across a mode change shows up here as a decision somebody made
		// rather than as a mystery.
		w.resize(800, 360);
		spin(200);
		check(!w.m_drawer_mode, "a wide window has no drawer");
		check(!w.m_drawer_open, "and leaving drawer mode closes it");
		check(w.m_sidebar->parentWidget() == w.m_splitter,
		      "the sidebar went back into the splitter");

		w.resize(360, 800);
		spin(200);
		check(w.m_drawer_mode && !w.m_drawer_open,
		      "coming back to narrow starts with the drawer closed");
		check(w.m_sidebar->geometry().right() < 0,
		      QString("parked fully off the left edge rather than half on: "
		              "x=%1..%2")
		              .arg(w.m_sidebar->geometry().left())
		              .arg(w.m_sidebar->geometry().right()));

		// And the button still works after all of that, which is the thing a
		// person would actually notice.
		if (w.m_drawer_action) w.m_drawer_action->trigger();
		spin(400);
		check(w.m_drawer_open, "the drawer opens again after the round trip");
		check(w.m_sidebar->geometry().left() == 0,
		      QString("and comes back to the left edge: x=%1")
		              .arg(w.m_sidebar->geometry().left()));
	}

	// **The loader refusing is half the fix; the window not writing is the
	// half that saves the file.** `test_tree` proves `tree_outline::load`
	// returns nothing for a tree it could not read. That proves a function.
	// What destroyed data was the caller: `m_tree_path` is assigned near the
	// top of `load_tree`, before the load runs, so a failed load used to leave
	// every writer in the window pointed at a file whose contents it had never
	// seen -- and closing the window wrote an empty tree over it.
	//
	// So this drives the whole path: an unreadable tree, a real window, a real
	// close, and the bytes on disk read back afterwards.
	section("a window will not save over a tree it could not read");
	if (geteuid() == 0) {
		std::printf("  skip  running as root, which can read anything\n");
	} else {
		const QString dir = QDir::temp().filePath("hydra-rotation-unreadable");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
		const QString path = dir + "/tree.txt";
		{
			QFile f(path);
			f.open(QIODevice::WriteOnly | QIODevice::Text);
			f.write("- [tab] something the user cares about | https://example.com/\n");
		}
		const QByteArray before = [&] {
			QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
		}();
		QFile::setPermissions(path, QFile::Permissions());

		{
			main_window unreadable(&factory, &policy, &filter);
			check(!unreadable.load_tree(path),
			      "load_tree refuses a tree it cannot read");
			unreadable.show();
			spin(150);
			// The close is the point: this is the path that used to write an
			// empty tree back over the file.
			unreadable.close();
			spin(150);
		}

		QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
		const QByteArray after = [&] {
			QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll();
		}();
		check(after == before,
		      QString("and the file survived the window closing (%1 bytes, was "
		               "%2)").arg(after.size()).arg(before.size()));
		check(after.contains("something the user cares about"),
		      "with the tab that was in it still named");
	}

	// **The same defect as the tree, in the five stores beside it.** Each one
	// loads a file at startup and saves it back later, and each answers "there
	// is nothing here yet" and "I could not read what is here" with the same
	// `false`. Treating the second as the first leaves the store empty with
	// its path still set, and the next save writes that emptiness over the
	// file.
	//
	// `keep_or_disown` is the caller-side fix and it needed no new state: the
	// writers are all guarded by `isEmpty()` already, so giving up the path is
	// the whole of it. This drives it through a real window and a real close,
	// because the store tests prove functions and what destroys a file is the
	// caller.
	section("a store whose file will not parse is not written over");
	{
		const QString dir = QDir::temp().filePath("hydra-rotation-stores");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
		const QString tree = dir + "/tree.txt";
		{
			QFile f(tree);
			f.open(QIODevice::WriteOnly | QIODevice::Text);
			f.write("- [t1] unopened_tab | A tab | https://example.com/\n");
		}

		// Two of the five, chosen because they fail differently: the policy is
		// refused by its `hydra/kind` marker, the consent rules by the same
		// marker *and* through a caller that used to substitute defaults and
		// then save them back over the file it had not read.
		struct { const char *name; QByteArray body; } files[] = {
			{ "policy.ini",     "this is not an ini file at all\n" },
			{ "site-rules.ini", "{ \"rules\": [] }\n" },
		};
		QList<QByteArray> before;
		for (const auto &spec : files) {
			QFile f(dir + "/" + spec.name);
			f.open(QIODevice::WriteOnly);
			f.write(spec.body);
			f.close();
			before.append(spec.body);
		}

		{
			main_window w2(&factory, &policy, &filter);
			check(w2.load_tree(tree),
			      "the tree itself still loads, so the window is usable");
			w2.show();
			spin(150);
			w2.close();
			spin(150);
		}

		// **Only the first of these two proves the guard, and the sabotage is
		// what said so.** With `keep_or_disown` reverted, `policy.ini` went
		// from 31 bytes to 406 -- a full default policy written over the
		// user's file, which is the destruction this exists to stop.
		// `site-rules.ini` stayed at 16 bytes with the guard sabotaged too,
		// because nothing writes the consent rules on close: that store is
		// saved only from the settings dialog, which this test never opens.
		//
		// So the second row is a true assertion that cannot fail, and it is
		// kept for what it does cover -- that a garbage `site-rules.ini` does
		// not stop the window starting or closing -- and named for that
		// rather than left looking like proof of a guard it never reaches.
		// Its guard is the same one line, and the fixture that reaches its
		// writer is the section below -- through the settings dialog, which
		// is the only thing that saves that store.
		int i = 0;
		for (const auto &spec : files) {
			QFile f(dir + "/" + spec.name);
			f.open(QIODevice::ReadOnly);
			const QByteArray now = f.readAll();
			const bool proves_guard = i == 0;
			check(now == before[i],
			      QString(proves_guard
			                ? "%1 was left exactly as it was (%2 bytes, was %3)"
			                : "%1 survived startup and close, though nothing "
			                  "here writes it (%2 bytes, was %3)")
			          .arg(spec.name).arg(now.size()).arg(before[i].size()));
			++i;
		}
	}

	// **The fixture the section above said was missing.** That one asserted
	// `site-rules.ini` was unchanged after a window closed, and the sabotage
	// showed the assertion could not fail: nothing writes the consent rules on
	// close, because that store is saved only from the settings dialog. So it
	// proved the guard for `policy.ini` and nothing at all for this one.
	//
	// This reaches the writer. The dialog is built exactly as
	// `main_window::open_settings` builds it -- same arguments, same order,
	// `m_site_rules_path` included -- so what is under test is the real chain:
	// `load_tree` gives up the path, the window hands the empty path to the
	// dialog, and the dialog's own `isEmpty()` check declines to write.
	//
	// **The control is the part that matters**, and it is why the earlier row
	// was worth so little. Asserting "the file did not change" passes equally
	// for a guard that works and for a button that was never wired to a save.
	// So the same click is made with a path that was kept, against a file
	// deleted first: if it comes back, the writer really does fire, and the
	// silence in the other half means something.
	section("the consent rules are not written over when unread");
	if (geteuid() == 0) {
		std::printf("  skip  running as root\n");
	} else {
		const QString dir = QDir::temp().filePath("hydra-rotation-rules");

		// Built exactly as main_window::open_settings does.
		auto forget_imported_via_dialog = [](main_window *w) {
			settings_dialog dlg(w->m_players, w->m_downloads, w->m_torrents,
			                     w->m_local_ai, w->m_external_ai, w->m_policy,
			                     w->m_filters, w->m_filters_path,
			                     w->m_consent, w->m_site_rules_path, w,
			                     w->m_factory);
			auto *forget = dlg.findChild<QPushButton *>("rules_forget");
			if (forget)
				forget->click();
			return forget != nullptr;
		};

		auto make_tree = [&](const QString &d) {
			QDir(d).removeRecursively();
			QDir().mkpath(d);
			QFile f(d + "/tree.txt");
			f.open(QIODevice::WriteOnly | QIODevice::Text);
			f.write("- [t1] unopened_tab | A tab | https://example.com/\n");
			return d + "/tree.txt";
		};

		// The case: a rules file that will not parse.
		{
			const QString d = dir + "-bad";
			const QString tree = make_tree(d);
			const QString rules = d + "/site-rules.ini";
			const QByteArray junk = "this is not an ini file at all\n";
			{ QFile f(rules); f.open(QIODevice::WriteOnly); f.write(junk); }

			main_window w2(&factory, &policy, &filter);
			check(w2.load_tree(tree), "the window opens on a good tree");
			check(w2.m_site_rules_path.isEmpty(),
			      "and has given up the rules file it could not read");
			check(forget_imported_via_dialog(&w2),
			      "the Forget imported button is there and was pressed");
			spin(50);

			QFile back(rules);
			back.open(QIODevice::ReadOnly);
			const QByteArray now = back.readAll();
			check(now == junk,
			      QString("the file it could not read is byte-identical "
			               "(%1 bytes, was %2)").arg(now.size()).arg(junk.size()));
		}

		// **The control.** Same button, same dialog, a path that was kept --
		// and the file deleted first, so its reappearance is proof the click
		// reaches a writer at all.
		{
			const QString d = dir + "-good";
			const QString tree = make_tree(d);
			const QString rules = d + "/site-rules.ini";
			{
				site_rules seed = site_rules::defaults();
				site_rule r;
				r.kind  = "selector";
				r.value = ".cookie-banner";
				r.host  = "example.com";
				seed.add(r);
				check(seed.save(rules), "a real rules file is written");
			}

			main_window w3(&factory, &policy, &filter);
			check(w3.load_tree(tree), "the window opens on a good tree");
			check(!w3.m_site_rules_path.isEmpty(),
			      "and keeps a rules file it could read");

			check(QFile::remove(rules), "the file is removed before the click");
			check(forget_imported_via_dialog(&w3), "the same button is pressed");
			spin(50);
			check(QFileInfo::exists(rules),
			      "and it comes back, so the click really does reach a writer");
		}

		QDir(dir + "-bad").removeRecursively();
		QDir(dir + "-good").removeRecursively();
	}

	// **A window hands its observers back.** `request_filter::add_observer` had
	// a counterpart nothing called anywhere in the tree -- found by grepping
	// for callers per method, which is the only way this shape shows up: the
	// interface is complete, which is exactly what is not wrong with it.
	//
	// The observers belong to the window and go when it does; the filter is
	// declared beside the window and outlives it. So a filter that saw a
	// second window kept four dangling pointers from the first, and `notify`
	// dereferences every entry with no check. One window is made in `main`, so
	// the running program never reached it -- this suite did, holding sixteen
	// observers of which twelve were freed by the end.
	section("a window gives its observers back to the filter");
	{
		policy_engine  own_policy;
		request_filter own_filter(&own_policy);
		check(own_filter.observer_count() == 0,
		      QString("a fresh filter has none (%1)")
		          .arg(own_filter.observer_count()));

		int during = -1;
		{
			main_window w4(&factory, &own_policy, &own_filter);
			during = own_filter.observer_count();
		}

		// **The middle number is the control**, and without it the last
		// assertion is worthless: a count of zero after the window is exactly
		// what a filter nothing ever registered with reports, so a test that
		// only checked the end would pass against a window that registers
		// nothing at all.
		//
		// Asserted exactly rather than "more than none", deliberately. If a
		// fifth observer is added later without a matching removal, the end
		// count catches it -- and this one fails too, which is what points at
		// the pair rather than at the symptom.
		check(during == 4,
		      QString("a live window has registered its four (%1)").arg(during));
		check(own_filter.observer_count() == 0,
		      QString("and the filter is empty again once it has gone (%1)")
		          .arg(own_filter.observer_count()));
	}

	// **Asked for from the desktop: the tabs show/hide button on desktop too.**
	// It existed only on a narrow window, where the tree is a drawer over the
	// page. On a wide one the tree is a splitter pane and the only way to get
	// it out of the way was to drag the splitter shut -- which is not the same
	// offer, gives nothing to press to get it back, and leaves no button whose
	// state says which way round things are.
	section("the tab tree can be hidden and brought back on a wide window");
	{
		main_window w5(&factory, &policy, &filter);
		w5.resize(900, 600);
		w5.show();
		spin(120);

		check(!w5.m_drawer_mode, "a 900px window is not in drawer mode");
		check(w5.m_sidebar && w5.m_sidebar->isVisible(),
		      "and the tree is showing to begin with");
		check(w5.m_drawer_action && w5.m_drawer_action->isChecked(),
		      "with the button checked to say so");

		// Widen the pane first, so restoring it can be shown to give back
		// what was there rather than a default that happens to match.
		w5.m_splitter->setSizes({420, 480});
		spin(60);
		const int widened = w5.m_splitter->sizes().value(0);
		check(widened > 300,
		      QString("the pane can be sized by hand (%1)").arg(widened));

		w5.m_drawer_action->trigger();
		spin(60);
		check(w5.m_sidebar && !w5.m_sidebar->isVisible(),
		      "pressing it hides the tree");
		check(!w5.m_drawer_action->isChecked(),
		      "and the button says so");

		w5.m_drawer_action->trigger();
		spin(60);
		check(w5.m_sidebar && w5.m_sidebar->isVisible(),
		      "pressing it again brings the tree back");
		const int restored = w5.m_splitter->sizes().value(0);
		check(restored == widened,
		      QString("at the width it had, not a default (%1, was %2)")
		          .arg(restored).arg(widened));

		// **The button is the only checkable thing in this toolbar**, which is
		// what made the held-contrast fix impossible to look at: everything
		// else here acts and returns, and what toggles lives in menus where a
		// tick says it instead.
		check(w5.m_drawer_action->isCheckable(),
		      "and it is a toggle, so its state is visible on the toolbar");
	}

	// **A review loop with no way to finish an item.** `consent_blocker`
	// records a banner nothing could answer and the dialog turns one of its
	// labels into a rule -- and nothing removed the label afterwards. The row
	// stayed, offering the same buttons, with the count under the table
	// unchanged and all of it still there the next time the dialog opened. A
	// person working down the list could not see what they had dealt with.
	//
	// Found from the other end: `found_unanswerable` was emitted and connected
	// to nothing anywhere in the tree, which is what sent anybody to read this
	// code at all.
	section("a banner leaves the review list once it has taught a rule");
	{
		policy_engine  pol;
		consent_blocker blocker(&pol);
		blocker.set_page_host("example.com");
		blocker.report_unhandled("Godta alle\tAvvis alle");
		check(blocker.unhandled().size() == 1,
		      QString("a banner nothing answered is recorded (%1)")
		          .arg(blocker.unhandled().size()));

		// One label of two: the banner stays, because it still has something
		// left to teach.
		check(blocker.forget_unhandled("example.com", "Avvis alle"),
		      "the label that became a rule is dropped");
		check(blocker.unhandled().size() == 1,
		      "the banner stays while it has another label to offer");
		check(!blocker.unhandled().first().contains("Avvis alle"),
		      "but that label is gone from it");
		check(blocker.unhandled().first().contains("Godta alle"),
		      "and the other one is not");

		// The last one takes the row with it: a host with no labels left is
		// nothing to review.
		check(blocker.forget_unhandled("example.com", "Godta alle"),
		      "the last label is dropped too");
		check(blocker.unhandled().isEmpty(),
		      QString("and the banner goes with it (%1 left)")
		          .arg(blocker.unhandled().size()));

		// **Two controls.** A label that was never there must not report a
		// drop, or the return value says nothing; and the host field must not
		// be matched as though it were one of its own button labels.
		blocker.report_unhandled("example.com\tOK");
		check(!blocker.forget_unhandled("example.com", "not a label here"),
		      "a label that is not there is not reported as dropped");
		check(!blocker.forget_unhandled("other.example", "OK"),
		      "and neither is one on a host that was never recorded");
	}

	// **Through the dialog, because the blocker agreeing with itself proves
	// nothing about the caller.** The section above shows `forget_unhandled`
	// works; this shows the button a person presses actually reaches it. That
	// is the gap the tree keeps finding -- a correct function with nothing
	// calling it -- and it is the one the previous section could not see.
	section("learning a rule from the dialog takes the banner off the list");
	{
		policy_engine   pol;
		consent_blocker blocker(&pol);
		blocker.set_page_host("example.com");
		blocker.report_unhandled("Avvis alle");
		check(blocker.unhandled().size() == 1, "one banner is waiting");

		consent_dialog dlg(&blocker, QString(), nullptr);
		auto *list = dlg.findChild<QTreeWidget *>("banners");
		auto *reject = dlg.findChild<QPushButton *>("learn_reject");
		check(list && reject, "the dialog has the list and the refuse button");
		if (list && reject && list->topLevelItemCount() == 1) {
			QTreeWidgetItem *top = list->topLevelItem(0);
			check(top->childCount() == 1,
			      QString("the banner offers its label (%1)")
			          .arg(top->childCount()));
			list->setCurrentItem(top->child(0));
			spin(30);
			check(reject->isEnabled(),
			      "and the button turns on for a label, not a host");
			reject->click();
			spin(30);
			check(blocker.unhandled().isEmpty(),
			      QString("pressing it takes the banner off the list (%1 left)")
			          .arg(blocker.unhandled().size()));
			check(list->topLevelItemCount() == 0,
			      "and the table it was in no longer shows it");
		}
	}

	// The signal that led there now has a listener: the menu entry carries the
	// count, which is what `Media (%1)` and the capture entry already do.
	section("the menu says how many banners are waiting");
	{
		main_window w6(&factory, &policy, &filter);
		check(w6.m_banners_action != nullptr, "there is a review entry");
		const QString quiet = w6.m_banners_action->text();
		check(!quiet.contains('('),
		      QString("with no count while nothing is waiting (%1)").arg(quiet));

		w6.m_consent->set_page_host("example.com");
		w6.m_consent->report_unhandled("Godta alle\tAvvis alle");
		spin(50);
		const QString loud = w6.m_banners_action->text();
		check(loud.contains("(1)"),
		      QString("and a count once one is (%1)").arg(loud));
	}

	// **A record of where somebody has been, with no way to remove it.**
	// `annoyance_log`'s own header says `clear_host` and `clear_all` "exist
	// for that" -- for letting a person read and clear what the Annoyed button
	// filed -- and nothing in the program called either. Only the tests did.
	// The file holds the host, the address that was open and a timestamp.
	//
	// It gets its own control rather than joining cookies, because
	// `forget_shell_caches` states the rule: forgetting is about what browsing
	// left behind, not about undoing decisions, and a report is something a
	// person went to the trouble of filing.
	section("the reports somebody filed can be cleared from inside Hydra");
	{
		main_window w7(&factory, &policy, &filter);
		check(w7.m_annoyances != nullptr, "the window keeps an annoyance log");

		annoyance_report r;
		r.host = "example.com";
		r.page = "https://example.com/article";
		w7.m_annoyances->add(r);
		check(w7.m_annoyances->all().size() == 1, "with a report in it");

		// Built exactly as `open_settings` builds it, so what is under test is
		// the dialog the person actually gets.
		settings_dialog dlg(w7.m_players, w7.m_downloads, w7.m_torrents,
		                     w7.m_local_ai, w7.m_external_ai, w7.m_policy,
		                     w7.m_filters, w7.m_filters_path, w7.m_consent,
		                     w7.m_site_rules_path, &w7, w7.m_factory,
		                     w7.m_annoyances);
		// **This connection stands in for the window's, and that is the
		// limit of what the section proves.** `main_window::open_settings`
		// makes its own dialog and `exec()`s it, so the real handler -- which
		// also writes the file -- is not reachable from here without a modal
		// nothing can answer. What is checked is that pressing Clear with the
		// box ticked emits, which is the half that was missing entirely; the
		// window's six lines are read rather than run.
		QObject::connect(&dlg, &settings_dialog::annoyance_reports_cleared,
		                  &w7, [&w7] {
			                  w7.m_annoyances->clear_all();
		                  });

		auto *box = dlg.findChild<QCheckBox *>("clear_reports");
		auto *go  = dlg.findChild<QPushButton *>("clear_now");
		check(box && go, "the privacy page offers the control and a button");
		if (box && go) {
			check(!box->isChecked(),
			      "off by default, because these are not a site's leavings");

			// Only the reports, so nothing is asked of the engine -- and the
			// cookie and cache boxes start ticked, so they have to be cleared
			// or this would delete more than it is testing.
			if (auto *c = dlg.findChild<QCheckBox *>("clear_cookies"))
				c->setChecked(false);
			if (auto *c = dlg.findChild<QCheckBox *>("clear_cache"))
				c->setChecked(false);
			box->setChecked(true);

			// **The confirmation is modal and would hang a headless run**, so
			// it is answered from a timer. Yes rather than Cancel, because
			// Cancel is the default button and a run that failed to find the
			// dialog would otherwise pass by doing nothing.
			QTimer::singleShot(120, [] {
				if (auto *m =
				        qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
					m->button(QMessageBox::Yes)->click();
			});
			go->click();
			spin(500);

			check(w7.m_annoyances->all().isEmpty(),
			      QString("clearing takes the reports with it (%1 left)")
			          .arg(w7.m_annoyances->all().size()));
		}

		// The control for the control: with no log supplied there is nothing
		// to clear, so the checkbox must not be offered at all rather than
		// sitting there doing nothing.
		settings_dialog bare(w7.m_players, w7.m_downloads, w7.m_torrents,
		                      w7.m_local_ai, w7.m_external_ai, w7.m_policy,
		                      w7.m_filters, w7.m_filters_path, w7.m_consent,
		                      w7.m_site_rules_path, &w7, w7.m_factory,
		                      nullptr);
		check(bare.findChild<QCheckBox *>("clear_reports") == nullptr,
		      "and it is absent when there is no log to clear");
	}

	// **Every field of `view_settings` has to be driven by something.** The
	// struct carries in-class defaults and `apply_policy` builds a fresh one
	// on every navigation, so a field it forgets silently takes the default
	// instead of the policy -- and that is not hypothetical here. The comment
	// beside `s.scrollbars` records it happening: the bars defaulted to true,
	// every re-apply handed them back, and an unattended kiosk screen grew
	// scrollbars the moment it followed a link.
	section("what the policy says reaches the page");
	{
		policy_engine  pol;
		request_filter filt(&pol);
		main_window w8(&factory, &pol, &filt);
		w8.resize(900, 600);

		using F = policy::feature;
		const F driven[] = { F::javascript, F::images, F::autoplay, F::popups };
		for (auto f : driven)
			pol.set_setting("blocked.example", f, policy::setting::block);
		for (auto f : driven)
			pol.set_setting("allowed.example", f, policy::setting::allow);

		auto settings_for = [&](const char *host) {
			w8.open_url(QUrl(QString("https://%1/page").arg(host)));
			spin(120);
			view_settings out;
			for (auto *v : w8.m_views_by_id)
				if (auto *f = dynamic_cast<fake_view *>(v))
					if (f->settings_applied > 0 && f->url().host() == host)
						out = f->last_settings;
			return out;
		};

		const view_settings blocked = settings_for("blocked.example");
		const view_settings allowed = settings_for("allowed.example");

		check(!blocked.javascript && !blocked.images && !blocked.autoplay &&
		          !blocked.popups,
		      QString("a blocked site gets none of them (js=%1 img=%2 auto=%3 "
		               "pop=%4)").arg(blocked.javascript).arg(blocked.images)
		          .arg(blocked.autoplay).arg(blocked.popups));

		// **The control, and the reason the line above is not enough.** Every
		// field defaults somewhere, so "all false" would also be produced by a
		// struct nobody filled in if the defaults happened to be false. The
		// pair is what says the policy reached the page.
		check(allowed.javascript && allowed.images && allowed.autoplay &&
		          allowed.popups,
		      QString("and an allowed one gets all of them (js=%1 img=%2 "
		               "auto=%3 pop=%4)").arg(allowed.javascript)
		          .arg(allowed.images).arg(allowed.autoplay)
		          .arg(allowed.popups));

		// **A field added later must not be able to slip past the pair
		// above.** They name four fields by hand, so a fifth would be
		// unmentioned and untested -- which is exactly how `scrollbars` got
		// its own comment. This fails to compile when the struct grows, and
		// the message says what to do about it.
		static_assert(sizeof(view_settings) == 5,
		               "view_settings gained or lost a field: drive it from "
		               "apply_policy and name it in the two checks above, or "
		               "say here why it is not policy's to set");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
