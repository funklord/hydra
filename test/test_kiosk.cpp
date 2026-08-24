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
#include <QLabel>
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
