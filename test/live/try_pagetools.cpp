// The tools that act on the page in front of you: find, and zoom.
//
// Neither existed. `findText` appeared nowhere in the tree, and
// `set_zoom_factor` had been in the seam since kiosk mode needed it with
// nothing a person could reach ever calling it.
//
// Split out of `try_navigate`: these act *on* a page rather than moving between
// pages or describing one, and they are the sections most likely to grow.
#include "shell_fixture.h"

#include "qtwebengine_view.h"
#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QStatusBar>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-pagetools");
	main_window &w   = f.window;
	QAction   *back  = f.back, *fwd = f.fwd, *reload = f.reload;
	QLineEdit *address = f.address;
	QTreeView *tv    = f.tv;
	policy_engine &policy = f.policy;
	const QString out = f.out, one = f.one, two = f.two;
	(void)fwd; (void)back; (void)reload; (void)tv; (void)policy;
	(void)one; (void)two;

	if (!back || !fwd || !reload || !address || !tv) {
		std::printf("the window did not come up as expected\n");
		return 1;
	}
	check(f.open_tab(0, "one.html"), "a page is open to work on");

	section("finding text on the page");
	{
		// Through the menu action, the way somebody reaches it, rather than by
		// calling the slot: the action is the part that can go missing.
		QAction *find_act = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find on &Page"))
				find_act = a;
		check(find_act, "there is a Find on Page action");
		// Page search owns Ctrl+F, the way it does in every browser, and the
		// tree filter moved to Ctrl+Shift+F. Only one action may hold a
		// sequence: two is an ambiguous overload and Qt fires neither
		// reliably, which briefly broke both of these.
		int on_ctrl_f = 0;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->shortcut() == QKeySequence("Ctrl+F"))
				++on_ctrl_f;
		check(on_ctrl_f == 1,
		      QString("exactly one action holds Ctrl+F (%1)").arg(on_ctrl_f));
		check(find_act && find_act->shortcut() == QKeySequence("Ctrl+F"),
		      "and it is the one that searches the page");

		QAction *tree_find = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find in Tree"))
				tree_find = a;
		check(tree_find && tree_find->shortcut() == QKeySequence("Ctrl+Shift+F"),
		      "the tree filter still has a shortcut of its own");

		QWidget *bar = w.findChild<QWidget *>("find_bar");
		check(bar && !bar->isVisible(), "and the bar stays out of the way until asked");

		if (find_act && bar) {
			find_act->trigger();
			spin(300);
			check(bar->isVisible(), "the bar appears");

			auto *input = w.findChild<QLineEdit *>("find_input");
			auto *count = w.findChild<QLabel *>("find_count");
			check(input && count, "with somewhere to type and somewhere to report");
			if (input && count) {
				input->setText("one");
				for (int i = 0; i < 30 && count->text().isEmpty(); ++i)
					spin(200);
				check(count->text().contains("1"),
				      QString("a word on the page is found (%1)").arg(count->text()));

				input->setText("zzzznotonthispage");
				for (int i = 0; i < 30 && !count->text().contains("No"); ++i)
					spin(200);
				check(count->text() == "No matches",
				      QString("and one that is not says so (%1)").arg(count->text()));
			}

			auto *closer = w.findChild<QWidget *>("find_close");
			if (auto *b = qobject_cast<QAbstractButton *>(closer)) {
				b->click();
				spin(300);
				check(!bar->isVisible(), "closing it puts it away again");
			}
		}
	}

	section("zooming the page");
	{
		// Through the actions, and read back through the seam rather than from
		// anything this window remembers -- a level the window believes and the
		// page does not have is exactly the bug worth catching.
		QAction *zin = nullptr, *zout = nullptr, *zoff = nullptr;
		for (QAction *a : w.findChildren<QAction *>()) {
			if (a->text().contains("Zoom &In"))   zin  = a;
			if (a->text().contains("Zoom &Out"))  zout = a;
			if (a->text().contains("Actual Size")) zoff = a;
		}
		check(zin && zout && zoff, "the three zoom actions exist");

		auto *view = w.findChild<qtwebengine_view *>();
		check(view, "and the page can be asked what it is at");
		if (zin && zout && zoff && view) {
			check(qFuzzyCompare(view->zoom_factor(), 1.0), "a page starts at 100%");

			zin->trigger();
			spin(200);
			check(view->zoom_factor() > 1.0,
			      QString("zooming in enlarges it (%1)").arg(view->zoom_factor()));

			// The ladder, not a multiplier: two steps up and two down is
			// exactly where it started, whatever route it took.
			zin->trigger();
			spin(150);
			zout->trigger();
			zout->trigger();
			spin(200);
			check(qFuzzyCompare(view->zoom_factor(), 1.0),
			      QString("and stepping back lands on 100% exactly (%1)")
			          .arg(view->zoom_factor()));

			zin->trigger();
			spin(150);
			zoff->trigger();
			spin(200);
			check(qFuzzyCompare(view->zoom_factor(), 1.0),
			      "Actual Size is an absolute, not an undo");
		}
	}

	return report();
}
