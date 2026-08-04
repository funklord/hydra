// The menu bar and the tree's context menu, checked against the conventions
// desktop software settled on between about 1995 and 2010.
//
// **Why a test and not a careful afternoon.** Menus rot by appending. Every one
// of the twenty items that had accumulated in Tools was added by someone with a
// reason, and the result was Settings fourth from the top between a video
// capture and an AI parser, the two importers separated by an unrelated action,
// and Undo at the bottom under a name that only made sense if you already knew
// what it undid. Nothing was wrong with any single addition. The order was
// nobody's decision, and it will stop being one again the moment this is not
// checked.
//
// So the rules are written down as assertions rather than as a comment: Quit
// last in File, About last in Help, Settings last in Tools, Properties last and
// Delete above it in the context menu, and no menu longer than fits in the
// glance a menu is for.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QContextMenuEvent>
#include <QRegularExpression>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// A menu's items by visible text, separators as "-", submenus marked.
static QStringList items_of(QMenu *m) {
	QStringList out;
	for (QAction *a : m->actions()) {
		if (a->isSeparator())        out << "-";
		else if (a->menu())          out << a->text() + " >";
		else                         out << a->text();
	}
	return out;
}

// Last item that is not a separator.
static QString last_item(QMenu *m) {
	const QStringList it = items_of(m);
	for (int i = it.size() - 1; i >= 0; --i)
		if (it[i] != "-")
			return it[i];
	return QString();
}

static QMenu *menu_named(QMenuBar *bar, const QString &title) {
	for (QAction *a : bar->actions())
		if (a->menu() && a->text().remove('&').compare(title, Qt::CaseInsensitive) == 0)
			return a->menu();
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-menus");
	QDir().mkpath(out);
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | A tab | https://example.test/one | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1000);

	QMenuBar *bar = w.findChild<QMenuBar *>();
	check(bar != nullptr, "the menu bar is reachable");
	if (!bar) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

	section("the bar itself");
	{
		QStringList titles;
		for (QAction *a : bar->actions())
			if (a->menu())
				titles << a->text().remove('&');
		std::printf("     %s\n", qPrintable(titles.join("  |  ")));
		// The order everyone already knows. Go sits where History/Bookmarks sat
		// in the browsers of the period; what matters is that File and Edit lead
		// and Help is last.
		check(titles == QStringList({ "File", "Edit", "View", "Go", "Tools", "Help" }),
		      "File, Edit, View, Go, Tools, Help — in that order");
	}

	struct { const char *name; const char *last; } tails[] = {
		{ "File",  "&Quit" },
		{ "Help",  "&About" },
		{ "Tools", "&Settings…" },
	};
	section("where the conventions are strongest");
	for (const auto &t : tails) {
		QMenu *m = menu_named(bar, t.name);
		check(m && last_item(m) == QString::fromUtf8(t.last),
		      QString("%1 ends with %2 (%3)")
		          .arg(t.name, t.last, m ? last_item(m) : QStringLiteral("no menu")));
	}

	section("Edit exists at all, and Undo leads it");
	{
		QMenu *e = menu_named(bar, "Edit");
		check(e != nullptr, "there is an Edit menu");
		if (e) {
			const QStringList it = items_of(e);
			std::printf("     %s\n", qPrintable(it.join(" / ")));
			check(!it.isEmpty() && it.first().contains("Undo"),
			      "Undo is the first item, where it has always been");
			// Rename and Delete were reachable only by right-clicking a row.
			check(!it.filter(QRegularExpression("Rename")).isEmpty(),
			      "and Rename is on a menu, not only on a right-click");
			check(!it.filter(QRegularExpression("Delete")).isEmpty(),
			      "and so is Delete");
		}
	}

	section("no menu is a list to read");
	for (QAction *a : bar->actions()) {
		QMenu *m = a->menu();
		if (!m)
			continue;
		int n = 0;
		for (const QString &i : items_of(m))
			if (i != "-")
				++n;
		std::printf("     %-8s %2d items\n", qPrintable(a->text().remove('&')), n);
		// Tools held twenty. A menu past about a dozen is being scanned rather
		// than glanced at, which is what submenus are for.
		check(n <= 12, QString("%1 has %2 items, twelve or fewer")
		                   .arg(a->text().remove('&')).arg(n));
	}

	section("the two importers are together");
	{
		QMenu *f = menu_named(bar, "File");
		QMenu *imp = nullptr;
		if (f)
			for (QAction *a : f->actions())
				if (a->menu() && a->text().contains("Import"))
					imp = a->menu();
		check(imp != nullptr, "File has an Import submenu");
		if (imp) {
			const QStringList it = items_of(imp);
			check(it.size() == 2 &&
			      !it.filter(QRegularExpression("Firefox")).isEmpty() &&
			      !it.filter(QRegularExpression("Chromium")).isEmpty(),
			      "holding Firefox and Chromium and nothing between them");
		}
	}

	section("Tools groups rather than lists");
	{
		QMenu *t = menu_named(bar, "Tools");
		QStringList subs;
		if (t)
			for (QAction *a : t->actions())
				if (a->menu())
					subs << a->text().remove('&');
		std::printf("     submenus: %s\n", qPrintable(subs.join(", ")));
		check(subs.size() >= 3,
		      QString("the twenty flat items became %1 submenus plus the rest")
		          .arg(subs.size()));
	}

	section("the context menu, where Delete sits above Properties");
	{
		// `QMenu::exec` blocks, so the menu is inspected from a timer while it is
		// up and then dismissed -- the same shape `try_rename` needed for the
		// properties dialog, and captured by value for the same reason: this
		// scope is still alive here, but the lambda must not depend on that.
		auto *view  = w.findChild<tab_tree_view *>();
		auto *model = w.findChild<tab_tree_model *>();
		check(view && model, "the tree view is reachable");
		if (view && model) {
			QStringList seen;
			bool opened = false;
			QTimer::singleShot(400, [&seen, &opened] {
				for (QWidget *popup : QApplication::topLevelWidgets()) {
					auto *m = qobject_cast<QMenu *>(popup);
					if (!m || !m->isVisible())
						continue;
					opened = true;
					seen = items_of(m);
					m->close();
					return;
				}
			});
			// Right-click the tab, which is the row with the most items on it.
			// Addressed through the view's own (proxy) model rather than the
			// model's index_for_node, whose indexes belong to the source and do
			// not name the same row here.
			view->expandAll();
			spin(200);
			const QModelIndex idx =
			    view->model()->index(0, 0, view->model()->index(0, 0));
			const QPoint at = idx.isValid()
			                      ? view->visualRect(idx).center()
			                      : QPoint(20, 20);
			QApplication::postEvent(
			    view->viewport(),
			    new QContextMenuEvent(QContextMenuEvent::Mouse, at,
			                           view->viewport()->mapToGlobal(at)));
			spin(1200);

			check(opened, "right-clicking a tab opens a menu");
			if (opened) {
				std::printf("     %s\n", qPrintable(seen.join(" / ")));
				const int del   = seen.indexOf("&Delete");
				const int props = seen.indexOf("P&roperties…");
				check(props >= 0 && props == seen.size() - 1,
				      "Properties is last, on its own");
				// The one that matters. Delete used to be *below* Properties,
				// which puts the irreversible item where the harmless one lives
				// in every file manager of the period.
				check(del >= 0 && props > del,
				      QString("and Delete is above it, not below (delete %1, "
				               "properties %2)").arg(del).arg(props));
			}
		}
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
