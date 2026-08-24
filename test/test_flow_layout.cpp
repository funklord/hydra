// A row that becomes several rows, and the arithmetic that decides when.
//
// The defect this exists for: the downloads dialog's six buttons in one
// QHBoxLayout, which at 360 logical pixels gave each of them 51 against an
// 80-pixel label -- "Open Folder" reading "pen Folde". A horizontal row's
// minimum width is the sum of its children, so `android_dialogs` handing the
// dialog the screen rectangle could not help: the layout would not go below
// 532 whatever it was told.
//
// **`minimumSize()` is the check that matters.** Everything else here is
// supporting evidence; that one number is what the Android path reads and what
// kept the dialog too wide.
#include "flow_layout.h"

#include <QApplication>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Fixed-size stand-ins, so every number below is arithmetic rather than a
// guess about what a style will do to a button on this machine.
static QWidget *box(QWidget *parent, int w, int h) {
	auto *x = new QWidget(parent);
	x->setFixedSize(w, h);
	return x;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	section("the minimum is the widest item, not the sum");
	{
		QWidget host;
		auto *flow = new flow_layout(&host, 0, 10, 10);
		box(&host, 80, 20);   // parented, then added below
		flow->addWidget(host.findChildren<QWidget *>().at(0));
		flow->addWidget(box(&host, 120, 20));
		flow->addWidget(box(&host, 60, 20));

		check(flow->count() == 3, "three items");
		// A QHBoxLayout would say 80+120+60 plus two gaps = 280. That sum is
		// exactly what kept the downloads dialog off a phone screen.
		check(flow->minimumSize().width() == 120,
		      QString("minimum width is the widest item (120), not the sum "
		               "(got %1)").arg(flow->minimumSize().width()));
	}

	section("wide enough for one line, so one line");
	{
		QWidget host;
		auto *flow = new flow_layout(&host, 0, 10, 10);
		for (int i = 0; i < 3; ++i)
			flow->addWidget(box(&host, 80, 20));
		// 3*80 + 2*10 = 260.
		check(flow->sizeHint().width() == 260,
		      QString("sizeHint is the unwrapped width, so a desktop dialog "
		               "opens the shape it always did (got %1)")
		          .arg(flow->sizeHint().width()));
		check(flow->heightForWidth(300) == 20,
		      QString("at 300 wide it is one row, 20 tall (got %1)")
		          .arg(flow->heightForWidth(300)));
	}

	section("too narrow, so it wraps");
	{
		QWidget host;
		auto *flow = new flow_layout(&host, 0, 10, 10);
		for (int i = 0; i < 3; ++i)
			flow->addWidget(box(&host, 80, 20));
		// 170 fits two (80+10+80) and not three.
		check(flow->heightForWidth(170) == 50,
		      QString("at 170 wide it is two rows: 20 + 10 + 20 (got %1)")
		          .arg(flow->heightForWidth(170)));
		check(flow->heightForWidth(80) == 80,
		      QString("at 80 wide it is three rows: 20*3 + 10*2 (got %1)")
		          .arg(flow->heightForWidth(80)));
	}

	section("an item wider than the space does not loop");
	{
		// The guard in lay_out(): wrapping is refused when the line is empty,
		// because an item that does not fit an empty line will not fit the next
		// one either, and a layout that keeps trying does not return.
		QWidget host;
		auto *flow = new flow_layout(&host, 0, 10, 10);
		flow->addWidget(box(&host, 500, 20));
		flow->addWidget(box(&host, 500, 20));
		check(flow->heightForWidth(100) == 50,
		      QString("two over-wide items are two rows, and it terminates "
		               "(got %1)").arg(flow->heightForWidth(100)));
	}

	section("the geometry it actually assigns");
	{
		QWidget host;
		auto *flow = new flow_layout(&host, 0, 10, 10);
		QWidget *a = box(&host, 80, 20);
		QWidget *b = box(&host, 80, 20);
		QWidget *c = box(&host, 80, 20);
		for (QWidget *w : { a, b, c })
			flow->addWidget(w);
		host.resize(170, 100);
		host.show();
		QApplication::processEvents();

		check(a->pos() == QPoint(0, 0),
		      QString("first item at 0,0 (got %1,%2)").arg(a->x()).arg(a->y()));
		check(b->pos() == QPoint(90, 0),
		      QString("second beside it at 90,0 (got %1,%2)")
		          .arg(b->x()).arg(b->y()));
		check(c->pos() == QPoint(0, 30),
		      QString("third wrapped to the next row at 0,30 (got %1,%2)")
		          .arg(c->x()).arg(c->y()));
	}

	section("it owns its items");
	{
		// A QLayout does not delete its items; this one must, or every dialog
		// that closes leaks a spacer per button.
		auto *host = new QWidget;
		auto *flow = new flow_layout(host, 0, 10, 10);
		QPointer<QWidget> child = box(host, 80, 20);
		flow->addWidget(child);
		check(flow->takeAt(0) != nullptr, "takeAt returns the item");
		check(flow->count() == 0, "and removes it");
		check(flow->takeAt(0) == nullptr, "taking past the end is nullptr");
		delete host;
		check(true, "destroying the host does not fault");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
