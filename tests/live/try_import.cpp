// The importers, through the real shell (§4).
//
// Both readers are covered offline in `test_session`. What is not coverable
// there is the part a person actually meets: a menu item that builds a folder
// in the live tree, from a live profile, without disturbing the tree file. A
// review UI that is correct and never clicked is this project's most common
// defect, and an importer is exactly that shape.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "session_import.h"
#include "tab_tree_model.h"
#include "node.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMenuBar>
#include <QTimer>
#include <QTreeView>
#include "tree_sort_proxy.h"
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void note(const QString &w) { std::printf("  --    %s\n", qPrintable(w)); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// By the text it carries, since these have no object names. Named lookup would
// be better; by position would break the moment a menu gains an entry.
static QAction *action_named(QWidget *w, const QString &text) {
	for (QAction *a : w->findChildren<QAction *>())
		if (a->text().contains(text))
			return a;
	return nullptr;
}

static node *mirror_folder(tab_tree_model *m, const QString &source) {
	for (node *c : m->root()->children)
		if (c->mirror == source)
			return c;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-import");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | Blank | about:blank | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1100, 760);
	w.show();
	spin(1200);

	auto *model = w.findChild<tab_tree_model *>();
	check(model != nullptr, "the tree model is reachable");
	if (!model) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	const int mine = model->root()->children.size();
	check(mine == 1, QString("the tree starts as its own (%1 top-level)").arg(mine));

	section("Firefox");
	{
		QAction *a = action_named(&w, "Tabs from &Firefox");
		check(a != nullptr, "the menu offers it");
		if (a) {
			a->trigger();
			spin(2500);
			node *m = mirror_folder(model, "firefox");
			if (!m) {
				note("no mirror: this machine may have no Firefox session.");
			} else {
				check(m->children.size() > 0,
				      QString("a mirror appears with tabs in it (%1) — \"%2\"")
				          .arg(m->children.size(), 0, 10).arg(m->title));
				check(model->root()->children.first() == m,
				      "at the top, above the user's own tree");
				check(model->root()->children.size() == mine + 1,
				      "and the user's tree is untouched beside it");
				bool labelled = true;
				for (node *c : m->children)
					if (c->title.isEmpty() || c->url.isEmpty()) labelled = false;
				check(labelled, "every row has a label and an address");
				note(QString("first: %1").arg(m->children.first()->title.left(56)));
			}
		}
	}

	section("Chromium");
	{
		QAction *a = action_named(&w, "Tabs from &Chromium");
		check(a != nullptr, "the menu offers it");
		if (a) {
			a->trigger();
			spin(2500);
			node *m = mirror_folder(model, "chromium");
			if (!m) {
				note("no mirror: this machine may have no Chromium session.");
			} else {
				check(m->children.size() > 0,
				      QString("a mirror appears with tabs in it (%1) — \"%2\"")
				          .arg(m->children.size(), 0, 10).arg(m->title));
				note(QString("first: %1").arg(m->children.first()->title.left(56)));
			}
		}
	}

	section("both at once, and neither in the file");
	{
		check(mirror_folder(model, "firefox") && mirror_folder(model, "chromium"),
		      "two mirrors coexist, one per source");
		check(model->root()->children.size() == mine + 2,
		      "beside the tree the user actually owns");

		// The invariant. Saving here is what the shell does on any structural
		// change, so this is the real path rather than a contrived one.
		check(model->save(tree), "the tree saves");
		QFile f(tree);
		f.open(QIODevice::ReadOnly);
		const QString text = QString::fromUtf8(f.readAll());
		f.close();
		check(!text.contains("Firefox (") && !text.contains("Chromium ("),
		      "and neither mirror is in it");
		check(text.contains("Mine") && text.contains("Blank"),
		      "while the user's own tree is");
		note("tree file after saving with two mirrors on screen:");
		for (const QString &line : text.split('\n'))
			if (!line.trimmed().isEmpty())
				note("  " + line.left(72));
	}

	section("the context menu, opened rather than assumed");
	{
		// QMenu::exec blocks, so the only way to see what it offers is to look
		// while it is up. A menu that is correct and never opened is this
		// project's most common defect, and this one moved between classes --
		// exactly when a signal quietly stops being connected.
		auto *view = w.findChild<QTreeView *>();
		check(view != nullptr, "the tree view is reachable");

		QStringList seen;
		bool popped = false;
		QTimer::singleShot(600, [&] {
			if (QWidget *popup = QApplication::activePopupWidget()) {
				popped = true;
				for (QAction *a : popup->actions())
					if (!a->isSeparator())
						seen << a->text();
				popup->close();
			}
		});
		// On a tab, where the menu is at its fullest.
		node *mine = nullptr;
		for (node *c : model->root()->children)
			if (c->mirror.isEmpty() && c->is_folder() && !c->children.isEmpty())
				mine = c->children.first();
		check(mine != nullptr, "there is a tab of the user's own to right-click");
		if (mine && view) {
			const QModelIndex src = model->index_for_node(mine);
			auto *proxy = w.findChild<tree_sort_proxy *>();
			const QModelIndex at = proxy ? proxy->mapFromSource(src) : src;
			view->scrollTo(at);
			const QRect r = view->visualRect(at);
			emit view->customContextMenuRequested(r.center());
		}
		spin(400);

		check(popped, "a menu actually appears on a right-click");
		check(seen.contains("&Open") && seen.contains("&Suspend"),
		      "offering the two things the shell has to carry out");
		check(seen.contains("Open in &Another App…"),
		      "including the handoff, which on a phone is how audio keeps "
		      "playing with the screen off");
		check(seen.contains("Dup&licate") && seen.contains("New &Folder Here") &&
		          seen.contains("&Delete") && seen.contains("P&roperties…"),
		      QString("and the ones the view does itself (%1)").arg(seen.join(", ")));
	}

	// Left on screen briefly so a screenshot of it means something.
	if (qEnvironmentVariableIsSet("HYDRA_HOLD"))
		spin(qEnvironmentVariableIntValue("HYDRA_HOLD"));

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
