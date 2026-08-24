// Renaming a tab, through the dialog a person actually uses (sec 4).
//
// The rule is covered offline in `test_model`: a page title follows the page, a
// chosen name does not get replaced, clearing it hands the tab back. What is
// not coverable there is whether the *dialog* carries those decisions -- whether
// typing a name reaches `update_node` at all, and whether emptying the field
// means what it is supposed to mean.
//
// `QDialog::exec` blocks, so the fields are filled from a timer while it is up.
// That is the only way to drive a modal, and it is worth doing: this dialog is
// the sole route by which a tab is deliberately named.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// Fill the properties dialog while it is on screen, then press OK or Cancel.
//
// **Captured by value, and the reason is a crash this cost.** The first version
// took the title and the accept flag by reference in the timer's lambda -- and
// this function returns *immediately*, before the dialog even exists, so by the
// time the timer fired 500 ms later those parameters were dead stack. It
// segfaulted inside `findChildren` on a `QDialog *` read out of reclaimed
// memory.
//
// That is the same defect this project found in `try_keepass` earlier the same
// day and wrote down: a lambda outliving the locals it captured by reference.
// Knowing the shape did not stop it being written again, which is an argument
// for the rule rather than against it -- the crash was immediate and obvious,
// where the earlier one corrupted a heap and surfaced three checks later.
//
// `saw` is owned by the caller and outlives the dialog, so it may be a pointer.
struct dialog_answer {
	bool    opened = false;
	QString id_label;
};
static void answer_dialog(dialog_answer *saw, QString new_title, bool accept) {
	QTimer::singleShot(500, [saw, new_title, accept] {
		QDialog *dlg = nullptr;
		for (QWidget *w : QApplication::topLevelWidgets())
			if (auto *d = qobject_cast<QDialog *>(w))
				if (d->isVisible()) dlg = d;
		if (!dlg)
			return;
		if (saw)
			saw->opened = true;
		// The first line edit is Title. The id is a *label*, deliberately, and
		// this records which so the check can say so rather than assume it.
		const QList<QLineEdit *> edits = dlg->findChildren<QLineEdit *>();
		if (!edits.isEmpty())
			edits.first()->setText(new_title);
		if (saw) {
			const QList<QLabel *> labels = dlg->findChildren<QLabel *>();
			for (QLabel *l : labels)
				if (l->toolTip().contains("keyed by"))
					saw->id_label = l->text();
		}
		if (auto *bb = dlg->findChild<QDialogButtonBox *>()) {
			QPushButton *b = bb->button(accept ? QDialogButtonBox::Ok
			                                    : QDialogButtonBox::Cancel);
			if (b) { b->click(); return; }
		}
		accept ? dlg->accept() : dlg->reject();
	});
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-rename");
	QDir().mkpath(out);
	QDir(out + "/state").removeRecursively();
	QFile::remove(out + "/policy.ini");
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | Original label | https://example.test/one | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1200);

	auto *model = w.findChild<tab_tree_model *>();
	auto *view  = w.findChild<tab_tree_view *>();
	check(model && view, "the tree and its view are reachable");
	if (!model || !view) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	node *tab = model->root()->children.first()->children.first();
	check(tab && tab->title == "Original label", "the tab starts with its filed name");

	section("the page names it, because nobody has");
	{
		check(model->set_page_title(tab, "Example — page one"),
		      "a page title is accepted");
		check(tab->title == "Example — page one", "and shows in the tree");
		check(!tab->renamed, "without counting as a choice");
	}

	section("renaming it through the dialog");
	{
		dialog_answer saw;
		answer_dialog(&saw, "My own name", /*accept=*/true);
		view->edit_properties(tab);   // blocks until the timer answers
		spin(300);
		check(saw.opened, "the dialog really opened");
		check(tab->title == "My own name",
		      QString("the typed name reaches the tree (%1)").arg(tab->title));
		check(tab->renamed, "and is recorded as chosen");
		check(saw.id_label == tab->id,
		      QString("the id is shown as a label rather than an editable "
		               "field (%1)").arg(saw.id_label.isEmpty() ? "not found"
		                                                         : saw.id_label));
	}

	section("and the page cannot take it back");
	{
		check(!model->set_page_title(tab, "Example — page two"),
		      "a new page title is refused");
		check(tab->title == "My own name", "so the chosen name stays");
	}

	section("cancelling changes nothing");
	{
		dialog_answer cancelled;
		answer_dialog(&cancelled, "Something else entirely", /*accept=*/false);
		view->edit_properties(tab);
		spin(300);
		check(tab->title == "My own name", "Cancel leaves the name alone");
	}

	section("clearing the name hands it back to the page");
	{
		dialog_answer cleared;
		answer_dialog(&cleared, QString(), /*accept=*/true);
		view->edit_properties(tab);
		spin(300);
		check(!tab->renamed, "an emptied name is no longer a choice");
		check(!tab->title.isEmpty(),
		      QString("and the row still has a label (%1)").arg(tab->title));
		check(model->set_page_title(tab, "Example — page three"),
		      "so the page may name it again");
		check(tab->title == "Example — page three", "which it does");
	}

	section("and it is on disk that way");
	{
		check(model->save(tree), "the tree saves");
		QFile f(tree);
		f.open(QIODevice::ReadOnly);
		const QString text = QString::fromUtf8(f.readAll());
		f.close();
		check(!text.contains("named=1"),
		      "a tab following the page carries no marker");
		check(text.contains("Example — page three"),
		      "and the page's name is what was written");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
