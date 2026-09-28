// Kiosk mode (architecture doc sec 8), which had never been run by anything.
//
// It is the last feature in the shell with neither a unit test nor a live
// driver, and it is the one that borrows a live view's widget out of the window,
// reparents it into a fullscreen stage of its own, and promises to put it back.
// A mistake there does not show up as a wrong pixel; it shows up as a tab that
// is gone when you leave kiosk mode.
//
// A fake backend is enough: kiosk only asks a view for its widget, its url, a
// zoom factor and a settings application, so none of this needs a web engine --
// which is why it is here rather than in `test/live/`.
#include "kiosk_controller.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QSignalSpy>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>

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

// The stage is a frameless top-level the controller makes for itself, and the
// geometric path detaches the page from it -- so `widget()->window()` stops
// answering and the stage has to be found rather than walked to.
static QWidget *frameless_stage(QWidget *home) {
	const QList<QWidget *> tops = QApplication::topLevelWidgets();
	for (QWidget *w : tops) {
		if (w == home || w->parentWidget())
			continue;
		if (w->windowFlags() & Qt::FramelessWindowHint)
			return w;
	}
	return nullptr;
}

static QGraphicsProxyWidget *embedded(QGraphicsView *gv) {
	if (!gv || !gv->scene())
		return nullptr;
	const QList<QGraphicsItem *> items = gv->scene()->items();
	for (QGraphicsItem *it : items)
		if (auto *p = qgraphicsitem_cast<QGraphicsProxyWidget *>(it))
			return p;
	return nullptr;
}

// A view that is only a widget, and a record of what was asked of it.
class fake_view : public web_view_backend {
public:
	// Parented to its own widget, exactly as the real backends are, so the
	// widget owns the backend and deleting the window frees both. That is also
	// why every one of these is heap-allocated below: a stack object adopted by
	// Qt's ownership graph is a double free waiting for the test to end, which
	// is how the first version of this file crashed.
	explicit fake_view(QWidget *parent = nullptr) : web_view_backend(nullptr) {
		m_widget = new QLabel("page", parent);
		setParent(m_widget);
	}

	QWidget *widget() override { return m_widget; }
	QUrl url() const override { return m_url; }
	void load(const QUrl &u) override { m_url = u; loads << u; }
	void back() override {}
	void forward() override {}
	void reload() override {}
	void apply_settings(const view_settings &s) override { settings_applied++; last = s; }
	void set_permission_decider(permission_decider) override {}
	void set_capture_chooser(capture_chooser) override {}
	void set_zoom_factor(double f) override { zooms << f; }
	void inject_script(const QString &, const QString &, bool) override {}
	void inject_main_world_script(const QString &, const QString &) override {}
	void set_script_bridge(QObject *, const QString &) override {}
	QByteArray save_state() const override { return {}; }
	bool restore_state(const QByteArray &) override { return false; }

	QLabel        *m_widget = nullptr;
	QUrl           m_url;
	QList<QUrl>    loads;
	QList<double>  zooms;
	int            settings_applied = 0;
	view_settings  last;
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	section("entering and leaving gives the widget back");
	{
		// The property that matters most: kiosk borrows a live tab's widget. If
		// exit does not return it to where it came from, the tab is still alive
		// and no longer anywhere the shell can show it.
		auto *home = new QWidget;
		auto *layout = new QVBoxLayout(home);
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		layout->addWidget(view.widget());
		home->resize(800, 600);
		home->show();
		spin(120);

		QWidget *const original_parent = view.widget()->parentWidget();
		check(original_parent == home, "the widget starts in the window");

		kiosk_controller k;
		QSignalSpy entered(&k, &kiosk_controller::entered);
		QSignalSpy left(&k, &kiosk_controller::left);

		kiosk_config cfg;
		cfg.home = QUrl("https://kiosk.example/home");
		k.set_config(cfg);

		check(k.enter(&view, home), "entering succeeds");
		check(k.active(), "and it is active");
		check(entered.count() == 1, "and says so once");
		spin(120);
		check(view.widget()->parentWidget() != home,
		      "the widget has been taken out of the window");
		check(view.widget()->window()->isFullScreen() ||
		          view.widget()->window()->windowFlags() & Qt::FramelessWindowHint,
		      "and is inside a frameless fullscreen stage");

		check(!k.enter(&view, home), "entering twice is refused rather than nested");

		k.exit();
		spin(120);
		check(!k.active(), "leaving works");
		check(left.count() == 1, "and says so once");
		check(view.widget()->parentWidget() == home,
		      "and the widget is back where it came from — this is the whole "
		      "contract");
		// Not "and visible again": the controller's contract is to hand the
		// widget back to `restore_to`, and putting it into a layout again is the
		// caller's business -- `main_window` does exactly that in its `left`
		// handler, with m_stack->addWidget(). Asserting visibility here would be
		// asserting somebody else's job and would fail for the right reasons.
		check(view.widget()->parent() == home,
		      "handed back as a child of the window, for the shell to re-add");

		delete home;
	}

	section("a view that is not there");
	{
		kiosk_controller k;
		check(!k.enter(nullptr, nullptr), "entering with no view is refused");
		check(!k.active(), "and nothing is entered");
		k.exit();   // must not crash
		check(true, "and exiting when not active does nothing rather than crashing");
	}

	section("the scale modes ask the view for what they need");
	{
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(800, 600);
		home->show();
		spin(80);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::reflow;
		cfg.fit   = fit_mode::contain;
		cfg.design_size = QSize(1024, 768);
		k.set_config(cfg);
		k.enter(&view, home);
		spin(150);
		check(!view.zooms.isEmpty(),
		      "reflow sets a zoom factor, since that is what reflow means");
		check(view.settings_applied > 0,
		      "and kiosk applies its settings preset rather than inheriting the tab's");
		k.exit();
		spin(80);
		check(view.zooms.last() == 1.0,
		      QString("and leaving puts the zoom back to 1.0 (%1)")
		          .arg(view.zooms.last()));
		delete home;
	}

	section("stretch under reflow is not representable, and is not pretended");
	{
		// One zoom factor cannot scale two axes independently. The header says
		// this falls back to cover; a test says it out loud so the fallback
		// cannot quietly become "stretch, badly".
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(800, 600);
		home->show();
		spin(80);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::reflow;
		cfg.fit   = fit_mode::stretch;
		cfg.design_size = QSize(1024, 768);
		k.set_config(cfg);
		check(k.enter(&view, home), "it still enters rather than refusing");
		spin(150);
		check(!view.zooms.isEmpty(), "with a single zoom factor, as cover would use");
		k.exit();
		spin(80);
		delete home;
	}

	section("the four fits under reflow, and how they relate");
	{
		// Checking each zoom factor against a recomputed min/max would be the
		// controller's arithmetic written twice, and would pass against a
		// broken switch as readily as a correct one so long as the test copied
		// the same mistake. What the header promises is a set of RELATIONS --
		// contain never zooms past cover, actual does not zoom at all, and
		// stretch is cover under another name -- and a relation cannot hold
		// while one side changes.
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(800, 600);
		home->show();
		spin(60);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::reflow;
		cfg.design_size = QSize(1400, 500);   // 2.8:1, so no stage matches it
		cfg.fit = fit_mode::contain;
		k.set_config(cfg);
		check(k.enter(&view, home), "entering succeeds");
		spin(100);
		QWidget *const stage = view.widget()->parentWidget();
		std::printf("  --    stage %dx%d against a 1400x500 design\n",
		             stage ? stage->width() : -1, stage ? stage->height() : -1);
		check(!view.zooms.isEmpty(), "contain sets a zoom");
		const double z_contain = view.zooms.isEmpty() ? -1.0 : view.zooms.last();

		cfg.fit = fit_mode::cover;    k.set_config(cfg); spin(30);
		const double z_cover = view.zooms.last();
		cfg.fit = fit_mode::stretch;  k.set_config(cfg); spin(30);
		const double z_stretch = view.zooms.last();
		cfg.fit = fit_mode::actual;   k.set_config(cfg); spin(30);
		const double z_actual = view.zooms.last();
		std::printf("  --    contain %.4f  cover %.4f  stretch %.4f  actual %.4f\n",
		             z_contain, z_cover, z_stretch, z_actual);

		check(z_actual == 1.0, "actual means exactly 1.0, whatever the stage is");
		check(z_stretch == z_cover,
		      "stretch falls back to cover, which is what the header says it does");
		if (z_contain == z_cover) {
			// Only reachable on a stage that happens to be 2.8:1, where the
			// two are the same number and the comparison would be vacuous.
			std::printf("  --    this stage matches the design's aspect, so "
			             "contain and cover coincide and are not compared\n");
		} else {
			check(z_contain < z_cover,
			      "contain fits inside what cover fills, so it is the smaller zoom");
		}
		check(view.widget()->size() == stage->size(),
		      "and under reflow the viewport fills the stage in every fit, since "
		      "the page is what reflows");
		k.exit();
		spin(60);
		delete home;
	}

	section("crop-via-clip keeps the viewport at its native size");
	{
		// scale_mode::none is the robust path and was never run. Its whole
		// behaviour is a geometry: native size, positioned by alignment, and
		// whatever hangs over the edge is the stage's problem.
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(800, 600);
		home->show();
		spin(60);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::none;
		cfg.design_size = QSize(640, 480);
		cfg.alignment = Qt::AlignLeft | Qt::AlignTop;
		k.set_config(cfg);
		check(k.enter(&view, home), "entering succeeds");
		spin(100);

		QWidget *const stage = view.widget()->parentWidget();
		check(stage && stage != home, "the page is on the stage");
		std::printf("  --    stage %dx%d against a 640x480 design\n",
		             stage ? stage->width() : -1, stage ? stage->height() : -1);
		check(!view.zooms.isEmpty() && view.zooms.last() == 1.0,
		      "nothing is scaled, which is the whole of what 'none' means");
		check(view.widget()->size() == QSize(640, 480),
		      QString("the viewport stays the design size (%1x%2), which is what "
		               "separates this from reflow")
		          .arg(view.widget()->width()).arg(view.widget()->height()));
		check(view.widget()->pos() == QPoint(0, 0),
		      "top-left alignment puts it in the corner");

		cfg.alignment = Qt::AlignRight | Qt::AlignBottom;
		k.set_config(cfg);
		spin(40);
		QRect g = view.widget()->geometry();
		check(g.x() + g.width() == stage->width() &&
		          g.y() + g.height() == stage->height(),
		      QString("bottom-right alignment puts its far corner on the stage's "
		               "(%1,%2 %3x%4)")
		          .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()));

		cfg.alignment = Qt::AlignCenter;
		k.set_config(cfg);
		spin(40);
		g = view.widget()->geometry();
		const int left = g.x(), right = stage->width() - (g.x() + g.width());
		const int top = g.y(), bottom = stage->height() - (g.y() + g.height());
		check(qAbs(left - right) <= 1 && qAbs(top - bottom) <= 1,
		      QString("centring leaves equal margins (%1/%2 across, %3/%4 down)")
		          .arg(left).arg(right).arg(top).arg(bottom));
		k.exit();
		spin(60);
		delete home;
	}

	section("the anchor reaches the modes that place a rectangle, and no others");
	{
		// **A setting whose own description sent people to the wrong mode.**
		// The settings page said the anchor governed "which edges are cropped
		// when Cover makes it larger", and the comment above the control said
		// it "only means anything under Cover" -- citing `aligned_rect` as the
		// proof the controller honoured it. `aligned_rect` is called from the
		// `scale_mode::none` branch, which has nothing to do with Cover.
		//
		// So this asserts the relationship the code actually has, in both
		// directions: under a mode that places a design rectangle the anchor
		// moves it, and under reflow there is no rectangle to move. The second
		// half is the one that catches the description coming back.
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(400, 300);
		home->show();
		spin(60);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::none;
		cfg.design_size = QSize(320, 240);
		cfg.alignment = Qt::AlignLeft | Qt::AlignTop;
		k.set_config(cfg);
		check(k.enter(&view, home), "entering succeeds");
		spin(100);
		const QRect top_left = view.widget()->geometry();
		cfg.alignment = Qt::AlignRight | Qt::AlignBottom;
		k.set_config(cfg);
		spin(60);
		const QRect bottom_right = view.widget()->geometry();
		check(top_left != bottom_right,
		      QString("under No scaling the anchor moves the design (%1,%2 to "
		               "%3,%4)")
		          .arg(top_left.x()).arg(top_left.y())
		          .arg(bottom_right.x()).arg(bottom_right.y()));
		check(top_left.size() == bottom_right.size(),
		      "without resizing it, since the anchor is a position");
		k.exit();
		spin(60);

		// Reflow, with Cover so that something genuinely is larger than the
		// viewport -- which is the case the old description named.
		cfg.scale = scale_mode::reflow;
		cfg.fit   = fit_mode::cover;
		cfg.alignment = Qt::AlignLeft | Qt::AlignTop;
		k.set_config(cfg);
		check(k.enter(&view, home), "entering again under reflow");
		spin(100);
		const QRect r_top_left = view.widget()->geometry();
		const double z_top_left = view.zooms.isEmpty() ? -1.0 : view.zooms.last();
		cfg.alignment = Qt::AlignRight | Qt::AlignBottom;
		k.set_config(cfg);
		spin(60);
		check(view.widget()->geometry() == r_top_left,
		      QString("under Reflow the anchor moves nothing -- the viewport is "
		               "the whole stage either way (%1x%2 at %3,%4)")
		          .arg(r_top_left.width()).arg(r_top_left.height())
		          .arg(r_top_left.x()).arg(r_top_left.y()));
		check(view.zooms.last() == z_top_left,
		      "and does not change the zoom either, so nothing about Cover's "
		      "crop is anchored");
		k.exit();
		spin(60);
		delete home;
	}

	section("geometric scale borrows the page through a scene and gives it back");
	{
		// The historically fragile path (sec 8.3), and the failure that matters
		// is not a wrong pixel. QGraphicsProxyWidget takes ownership of what it
		// embeds, so a teardown that deletes the scene without releasing the
		// widget first destroys the tab -- which is the same loss the first
		// section in this file exists to catch, by a route that section cannot
		// reach.
		auto *home = new QWidget;
		auto *layout = new QVBoxLayout(home);
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		layout->addWidget(view.widget());
		home->resize(800, 600);
		home->show();
		spin(80);

		QPointer<QWidget> page(view.widget());

		kiosk_controller k;
		kiosk_config cfg;
		cfg.scale = scale_mode::geometric;
		cfg.fit = fit_mode::contain;
		// Deliberately not a size whose contain factor comes out at 1.0: an
		// identity transform is what a path that never calls setTransform
		// leaves behind, so a design that scales by one would pass this section
		// with the transform untouched.
		cfg.design_size = QSize(500, 400);
		k.set_config(cfg);
		check(k.enter(&view, home), "entering succeeds");
		spin(150);

		QPointer<QWidget> stage(frameless_stage(home));
		check(stage != nullptr, "there is a stage");
		QGraphicsView *gv = stage ? stage->findChild<QGraphicsView *>() : nullptr;
		check(gv != nullptr, "with a graphics view in it");
		check(page && page->parentWidget() == nullptr,
		      "and the page is embedded rather than parented, since addWidget "
		      "only takes a top-level");
		check(page && page->size() == QSize(500, 400),
		      QString("rendered at the design size (%1x%2) with the transform "
		               "doing the scaling")
		          .arg(page ? page->width() : -1).arg(page ? page->height() : -1));

		QGraphicsProxyWidget *proxy = embedded(gv);
		check(proxy != nullptr, "the scene holds the page");
		const QTransform t_contain = proxy ? proxy->transform() : QTransform();
		check(proxy && t_contain.m11() == t_contain.m22(),
		      QString("contain scales both axes alike (%1, %2)")
		          .arg(t_contain.m11()).arg(t_contain.m22()));
		check(proxy && t_contain.m11() != 1.0,
		      "and it is a scale rather than an identity, which is what an "
		      "untouched transform would look like");

		cfg.fit = fit_mode::stretch;
		k.set_config(cfg);
		spin(60);
		const QTransform t_stretch = proxy ? proxy->transform() : QTransform();
		std::printf("  --    stretch transform %.4f x %.4f\n",
		             t_stretch.m11(), t_stretch.m22());
		if (stage && stage->width() * 400 == stage->height() * 500) {
			std::printf("  --    this stage matches the design's aspect, so "
			             "stretch and contain coincide and are not compared\n");
		} else {
			check(proxy && t_stretch.m11() != t_stretch.m22(),
			      "stretch scales the axes independently -- the thing reflow "
			      "cannot do, and the reason this mode exists");
		}

		k.exit();
		spin(150);
		check(page != nullptr,
		      "the page still exists after leaving: the proxy owned it and had "
		      "to be made to let go");
		check(page && page->parentWidget() == home,
		      "and is back in the window, which is the contract");
		check(stage == nullptr, "the stage is gone");

		check(k.enter(&view, home),
		      "and it can be entered again, so the teardown left nothing stale");
		spin(150);
		check(page && page->parentWidget() == nullptr, "on a second scene");
		k.exit();
		spin(150);
		check(page && page->parentWidget() == home, "and handed back again");
		delete home;
	}

	section("the way out is the way the settings page says it is");
	{
		// **The lockdown was verified to be DESCRIBED and never to work.**
		// `test_rotation` asserts that the Kiosk Mode menu entry stops
		// promising "Esc returns" when `allow_escape` is off, and
		// `test_settings` asserts the flag survives a round trip -- so the
		// sentence and the storage are covered and the key press is not. The
		// settings row says turning it off means "Esc and F11 will not leave",
		// and that an unattended display is why the switch exists; a lockdown
		// that does not lock is the failure that matters, and it is the one
		// nothing looked at.
		//
		// F11 is in this because of the reason the controller gives for
		// handling it at all: the shell is hidden while kiosk is up, so the
		// shortcut that got somebody in reaches nothing, and the stage has to
		// answer for it itself.
		auto send_key = [](QWidget *to, int key) {
			QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
			QCoreApplication::sendEvent(to, &press);
		};

		struct { int key; const char *name; } keys[] = {
			{ Qt::Key_Escape, "Esc" },
			{ Qt::Key_F11,    "F11" },
		};

		for (const auto &kk : keys) {
			// Allowed: the key leaves, and leaving means the widget is handed
			// back -- the contract the first section in this file is about,
			// reached by the route a person actually uses.
			{
				auto *home = new QWidget;
				auto *layout = new QVBoxLayout(home);
				auto *view_p = new fake_view(home);
				fake_view &view = *view_p;
				layout->addWidget(view.widget());
				home->resize(400, 300);
				home->show();
				spin(60);

				kiosk_controller k;
				kiosk_config cfg;
				cfg.allow_escape = true;
				k.set_config(cfg);
				check(k.enter(&view, home), QString("%1: entered").arg(kk.name));
				spin(100);
				QWidget *stage = frameless_stage(home);
				check(stage != nullptr, QString("%1: there is a stage").arg(kk.name));
				if (stage)
					send_key(stage, kk.key);
				spin(100);
				check(!k.active(),
				       QString("%1 leaves when the setting allows it").arg(kk.name));
				check(view.widget()->parentWidget() == home,
				       QString("%1: and the page is handed back").arg(kk.name));
				delete home;
			}

			// Locked down: the same key does nothing at all, which is the
			// direction that matters. A public screen that can be escaped is
			// the failure this switch exists to prevent.
			{
				auto *home = new QWidget;
				auto *view_p = new fake_view(home);
				fake_view &view = *view_p;
				home->resize(400, 300);
				home->show();
				spin(60);

				kiosk_controller k;
				kiosk_config cfg;
				cfg.allow_escape = false;
				k.set_config(cfg);
				check(k.enter(&view, home),
				       QString("%1: entered locked down").arg(kk.name));
				spin(100);
				QWidget *stage = frameless_stage(home);
				if (stage)
					send_key(stage, kk.key);
				spin(100);
				check(k.active(),
				       QString("%1 does not leave when it is locked down")
				         .arg(kk.name));
				k.exit();
				spin(80);
				delete home;
			}
		}

		// The control: a key that is neither must not leave in either mode, or
		// the checks above would pass for a stage that exits on any key at all.
		{
			auto *home = new QWidget;
			auto *view_p = new fake_view(home);
			fake_view &view = *view_p;
			home->resize(400, 300);
			home->show();
			spin(60);

			kiosk_controller k;
			kiosk_config cfg;
			cfg.allow_escape = true;
			k.set_config(cfg);
			k.enter(&view, home);
			spin(100);
			QWidget *stage = frameless_stage(home);
			if (stage)
				send_key(stage, Qt::Key_A);
			spin(100);
			check(k.active(),
			       "an ordinary key does not leave even with Esc allowed");
			k.exit();
			spin(80);
			delete home;
		}
	}

	section("idle reset walks back home");
	{
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(400, 300);
		home->show();
		spin(80);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.home = QUrl("https://kiosk.example/attract");
		cfg.idle_reset_seconds = 1;
		k.set_config(cfg);
		k.enter(&view, home);
		view.loads.clear();

		spin(1400);
		check(!view.loads.isEmpty(),
		      "an abandoned session goes back to the home url on its own");
		check(view.loads.last() == QUrl("https://kiosk.example/attract"),
		      "and to the configured one");
		k.exit();
		spin(80);
		delete home;
	}

	section("idle reset off means off");
	{
		auto *home = new QWidget;
		auto *view_p = new fake_view(home);
		fake_view &view = *view_p;
		home->resize(400, 300);
		home->show();
		spin(80);

		kiosk_controller k;
		kiosk_config cfg;
		cfg.home = QUrl("https://kiosk.example/attract");
		cfg.idle_reset_seconds = 0;
		k.set_config(cfg);
		k.enter(&view, home);
		view.loads.clear();
		spin(1400);
		check(view.loads.isEmpty(),
		      "zero seconds is off, not one second — a kiosk that resets under "
		      "someone's hands is worse than one that never does");
		k.exit();
		spin(80);
		delete home;
	}

	section("forgetting says so, so the shell can drop what no backend holds");
	{
		// **The window keeps caches a view factory cannot reach**, the
		// per-session permission answers above all: an answer somebody chose
		// *not* to have remembered, which skips the prompt next time it is
		// asked for. Nothing cleared them, so on a public screen the next
		// person inherited the last person's camera. This signal is how the
		// window is told, and these check it fires at every moment a session
		// is actually forgotten.
		kiosk_controller k;
		QSignalSpy forgotten(&k, &kiosk_controller::session_forgotten);

		kiosk_config cfg;
		cfg.home = QUrl("https://kiosk.example/home");
		cfg.clear_between_sessions = false;
		k.set_config(cfg);

		fake_view view;
		QWidget *home = new QWidget;
		home->resize(800, 600);
		view.widget()->setParent(home);
		home->show();
		spin(80);

		k.enter(&view, home);
		k.exit();
		spin(80);
		check(forgotten.count() == 0,
		      QString("with clear_between_sessions off nothing is announced "
		               "(%1)").arg(forgotten.count()));

		// **No factory, deliberately.** Dropping the shell's own caches needs
		// nothing but this object, and a kiosk told to forget with no factory
		// warns and returns -- so an announcement below that guard would keep
		// the last person's answers as well as their cookies. Written first
		// with the emit below the guard, where this case reports zero.
		cfg.clear_between_sessions = true;
		k.set_config(cfg);
		forgotten.clear();
		k.enter(&view, home);
		check(forgotten.count() >= 1,
		      QString("entering a session announces the forgetting (%1)")
		          .arg(forgotten.count()));
		forgotten.clear();
		k.exit();
		spin(80);
		check(forgotten.count() >= 1,
		      QString("and so does leaving one (%1)").arg(forgotten.count()));
		// **Unparented before the window goes.** `fake_view` owns its widget,
		// and leaving it a child of `home` means `home`'s destructor frees it
		// and the view's frees it again -- `free(): invalid pointer`, after
		// every assertion above had already passed. A suite that aborts on
		// the way out still fails, and reads as a fault in what it was
		// testing rather than in how it tidied up.
		view.widget()->setParent(nullptr);
		delete home;
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
