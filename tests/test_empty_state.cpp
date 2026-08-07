// The message a list shows when it is empty, and when it stops showing it.
//
// Three dialogs had written this separately, two of them the same way and the
// third -- the consent one -- putting its sentence in a status label under the
// table, where it read as a footnote rather than as the answer to why the table
// was blank. Collapsing them into one helper is only worth doing if the one
// helper is right, and the part worth checking is not the label: it is that it
// follows the model rather than waiting to be told.
//
// **A caller that must remember to call `refresh()` will forget on one path**,
// and that path is always the one that empties the list. So the connections are
// the thing under test here.
#include "empty_state.h"

#include <QApplication>
#include <QLabel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QTreeWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// The overlay is found the way a person would find it on screen: a visible
// child of the viewport. Nothing else about it is public, on purpose.
static QLabel *overlay(QAbstractItemView *view) {
	for (QLabel *l : view->viewport()->findChildren<QLabel *>())
		return l;
	return nullptr;
}

// **Each fixture is shown**, because `isVisible()` is false for a child of an
// unshown parent however the child was set -- so an unshown list would report
// every overlay hidden and this file would pass by never seeing one. Six checks
// failed that way on the first run, all of them about the test rather than the
// code.
int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	section("an empty list says so");
	{
		QTreeWidget list;
		list.resize(400, 300);
		list.show();
		empty_state e(&list);
		e.set_text("Nothing here yet.");
		QLabel *l = overlay(&list);
		check(l != nullptr, "an overlay was added to the viewport");
		check(l && l->isVisible(), "and is showing, because the list is empty");
		check(l && l->text() == "Nothing here yet.", "with the words it was given");
		check(l && !l->isEnabled(),
		      "dimmed by the style rather than a hand-picked grey, so it stays "
		      "legible in either colour scheme");
		check(l && l->testAttribute(Qt::WA_TransparentForMouseEvents),
		      "and transparent to the mouse: an empty list must not feel broken");
	}

	section("a row arrives and the message goes, with nobody told to look");
	{
		QTreeWidget list;
		list.resize(400, 300);
		list.show();
		empty_state e(&list);
		e.set_text("Nothing here yet.");
		QLabel *l = overlay(&list);

		new QTreeWidgetItem(&list, QStringList("a download"));
		check(l && !l->isVisible(),
		      "hidden on insert, without a refresh() call from the caller");

		delete list.takeTopLevelItem(0);
		check(l && l->isVisible(), "and back when the last row goes");

		new QTreeWidgetItem(&list, QStringList("one"));
		new QTreeWidgetItem(&list, QStringList("two"));
		check(l && !l->isVisible(), "gone again for two rows");
		list.clear();
		check(l && l->isVisible(),
		      "and back after clear(), which is the path a caller forgets");
	}

	section("children are not rows");
	{
		// The count that matters is top-level: a single site with four buttons
		// under it is one recorded banner, not five, and an overlay that
		// counted descendants would vanish on a list that still looks empty.
		QTreeWidget list;
		list.resize(400, 300);
		list.show();
		empty_state e(&list);
		e.set_text("Nothing recorded.");
		QLabel *l = overlay(&list);
		auto *top = new QTreeWidgetItem(&list, QStringList("site"));
		for (int i = 0; i < 4; ++i)
			new QTreeWidgetItem(top, QStringList("button"));
		check(l && !l->isVisible(), "one top-level row hides it");
		delete list.takeTopLevelItem(0);
		check(l && l->isVisible(), "and removing that one row brings it back");
	}

	section("no words means say nothing");
	{
		// Distinct from an empty list. The media dialog has a case where the
		// page *is* playing something it cannot name, and the explanation goes
		// in the status line instead -- so the overlay must stay out of the way
		// even though there are no rows.
		QTreeWidget list;
		list.resize(400, 300);
		list.show();
		empty_state e(&list);
		QLabel *l = overlay(&list);
		check(l && !l->isVisible(), "empty list, no text, nothing shown");
		e.set_text("something");
		check(l && l->isVisible(), "text makes it appear");
		e.set_text(QString());
		check(l && !l->isVisible(), "and clearing the text takes it away again");
	}

	section("it covers the viewport, not the header");
	{
		QTreeWidget list;
		list.setHeaderLabels({ "Name", "Size" });
		list.resize(400, 300);
		list.show();
		QApplication::processEvents();
		empty_state e(&list);
		e.set_text("Nothing here yet.");
		QLabel *l = overlay(&list);
		check(l && l->geometry() == list.viewport()->rect(),
		      "geometry matches the viewport exactly");
		check(l && l->parentWidget() == list.viewport(),
		      "parented to the viewport, so a header cannot be covered by it");

		list.resize(700, 500);
		QApplication::processEvents();
		check(l && l->geometry() == list.viewport()->rect(),
		      "and follows a resize, which is where a fixed geometry stranded "
		      "the same message in a corner once");
	}

	section("a plain view with a plain model, not only QTreeWidget");
	{
		QTreeView view;
		QStandardItemModel model;
		view.setModel(&model);
		view.resize(400, 300);
		view.show();
		empty_state e(&view);
		e.set_text("Nothing here yet.");
		QLabel *l = overlay(&view);
		check(l && l->isVisible(), "shown for an empty model");
		model.appendRow(new QStandardItem("row"));
		check(l && !l->isVisible(), "and hidden when the model gains a row");
	}

	section("the view is destroyed while the overlay is still alive");
	{
		// **The order the dialogs actually use**, and it segfaulted. The helper
		// is parented to the dialog and the list is a child of the dialog too,
		// so the list goes first -- and `~QTreeWidget` emits `modelReset` from
		// inside `deleteChildren()`, *after* the viewport and the overlay have
		// already been freed. `refresh()` then ran on a dangling label, and the
		// consent dialog crashed on every close.
		//
		// Every other section here declares the view first and the helper
		// second, so the helper dies first and the bug cannot appear. It took a
		// live driver to find; this is it written down.
		auto *list = new QTreeWidget;
		list->resize(400, 300);
		list->show();
		auto *e = new empty_state(list);
		e->set_text("Nothing here yet.");
		new QTreeWidgetItem(list, QStringList("a row"));
		delete list;
		check(true, "destroying the view under a live overlay does not crash");
		e->refresh();
		check(true, "and refresh() afterwards is a no-op rather than a fault");
		delete e;
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
