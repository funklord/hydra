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
#include "kiosk_controller.h"
#include "tab_tree_view.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "web_view_backend.h"
#include "web_view_factory.h"

#include <QAction>
#include <QApplication>
#include <QDir>
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
	void apply_settings(const view_settings &) override {}
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

		check(w.m_drawer_action
		              && w.m_drawer_action->isVisible() == next.drawer,
		      QString("%1: the drawer button is shown only where the drawer is")
		              .arg(QString::fromUtf8(next.what)));

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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
