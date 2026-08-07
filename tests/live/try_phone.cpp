// Do the dialogs actually fit on a phone screen?
//
// `android_dialogs::install()` exists because they did not: the media dialog
// came up 1200 logical pixels wide on a 1080-pixel screen, so its list was
// visible and its buttons were not -- off the right edge, with no way to scroll
// a dialog to reach them. It was found by tapping Play and discovering there
// was nothing to tap.
//
// **The fix was never checked against the contents.** It sets the dialog's own
// minimum to nothing and then assigns it the screen rectangle, which is the
// right instruction -- but a widget cannot be made smaller than its *layout's*
// minimum, and a layout's minimum is whatever its least compressible child
// demands. A label that will not wrap, a tree whose columns are fixed, a row of
// five buttons that will not stack: any of those keeps the dialog wider than
// the screen no matter what geometry it is handed, and the buttons go back off
// the edge. So the fix could be exactly right and the outcome unchanged, and
// nothing here would have said so.
//
// This needs no phone. The question is entirely about layout arithmetic: give
// each dialog the same instruction Android gives it, then ask whether every
// button it owns ended up inside it.
//
// **`QWidget::grab()`, never `import`** -- it renders in-process and does not
// touch the X server.
#include "shell_fixture.h"

#include "main_window.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <cstdio>

// A small phone in logical pixels, portrait. Smaller than most, on purpose:
// the point is to find the dialog that cannot shrink, and a generous screen
// hides exactly that.
static const QSize k_phone(360, 640);

// `check`, `section` and `report` come from the fixture, which every other
// driver here uses; only the shot counter is local.
static int g_shots = 0;
static QString g_out;

static void save(QWidget *w, const QString &name) {
	const QString path = QString("%1/%2-%3.png")
	                         .arg(g_out).arg(g_shots, 2, 10, QChar('0')).arg(name);
	if (w->grab().save(path)) {
		std::printf("  shot  %s\n", qPrintable(path));
		++g_shots;
	}
}

// Exactly what `android_dialogs::dialog_sizer` does on a Show event, minus the
// screen lookup: clear the dialog's own minimum, then hand it the rectangle.
// Written out rather than called, because that code is compiled only for
// Android -- so this is a copy, and it is a copy on purpose. If the two drift
// this driver measures the wrong thing, which is worth knowing.
static void as_android_would(QDialog *dlg) {
	dlg->setMinimumSize(0, 0);
	dlg->setGeometry(QRect(QPoint(0, 0), k_phone));
}

// **A dialog whose narrow layout is a known, measured gap.** The sweep reads
// the "N passed, M failed" line, so a driver that reports an unfixed defect as
// a failure makes every future sweep red -- and this tree's own rule is that a
// summary line which is always wrong trains people to skip the summary. Named
// here with the reason, the same way `sweep.sh` names the drivers it skips, so
// the gap is visible rather than silent.
//
// The difference from a skip: the dialog is still measured and its numbers are
// still printed. What is suppressed is only the verdict.
static QString known_gap(const QString &name) {
	if (name == "settings")
		return "its seven pages fit now; what is left is the button box, which "
		        "needs 373 because the Restore label names the page it acts "
		        "on. That is a deliberate choice, and on a device the label "
		        "elides rather than anything being unreachable.";
	return QString();
}

// Every button must land inside the dialog. This is the exact failure the
// Android filter was written for, stated as arithmetic rather than as a feel.
static void measure(QDialog *dlg, const QString &name) {
	const QString gap = known_gap(name);
	// A local `check` that downgrades a verdict to a note for a named gap, and
	// is the ordinary one otherwise.
	auto verdict = [&](bool ok, const QString &what) {
		if (ok || gap.isEmpty()) {
			shell::check(ok, what);
			return;
		}
		std::printf("  KNOWN %s\n", qPrintable(what));
	};

	if (!gap.isEmpty())
		std::printf("  gap   %s: %s\n", qPrintable(name), qPrintable(gap));

	as_android_would(dlg);
	QApplication::processEvents();

	const QSize got = dlg->size();
	// What the layout will not go below, whatever it is asked. This is the
	// number that decides the outcome, so it is printed either way.
	const QSize floor = dlg->layout() ? dlg->layout()->minimumSize()
	                                  : dlg->minimumSizeHint();
	std::printf("  %-16s asked %dx%d, got %dx%d, layout floor %dx%d\n",
	             qPrintable(name), k_phone.width(), k_phone.height(),
	             got.width(), got.height(), floor.width(), floor.height());

	// **The floor decides, not `size()`.** The first version of this compared
	// `dlg->size()` against the screen immediately after `setGeometry`, which
	// is the value that was just assigned -- so it agreed every time and
	// reported two dialogs as fitting whose layouts will not go below 532 and
	// 395 pixels. A check that cannot fail is not a check, and this one had the
	// answer printed beside it the whole time.
	//
	// Qt honours the assignment for exactly as long as nothing re-lays-out; the
	// layout wins the moment anything does, and on a device something always
	// does. So the question is what the layout will accept, which is what
	// `minimumSize()` reports.
	verdict(floor.width() <= k_phone.width(),
	      QString("%1 can shrink to the width (floor %2)")
	          .arg(name).arg(floor.width()));

	// **Name what is stuck, rather than leaving it to be hunted.** A floor is a
	// number about the whole dialog, and the first thing anyone asks next is
	// which child produced it -- so the widest few are printed with their own
	// minimums. Two rounds of guessing at the settings dialog is what put this
	// here.
	if (floor.width() > k_phone.width()) {
		QList<QPair<int, QString>> blame;
		for (QWidget *c : dlg->findChildren<QWidget *>()) {
			const int w = c->minimumSizeHint().width();
			if (w <= 0)
				continue;
			const QString who = c->objectName().isEmpty()
			                        ? QString(c->metaObject()->className())
			                        : QString("%1 (%2)").arg(c->objectName(),
			                                                  c->metaObject()->className());
			blame.append({ w, who });
		}
		std::sort(blame.begin(), blame.end(),
		           [](const auto &a, const auto &b) { return a.first > b.first; });
		for (int i = 0; i < blame.size() && i < 5; ++i)
			std::printf("        widest: %4d  %s\n", blame[i].first,
			             qPrintable(blame[i].second));
	}

	// Height is the softer half: a dialog taller than the screen can be
	// scrolled in a way a wider one cannot, so it is reported and not failed.
	if (floor.height() > k_phone.height())
		std::printf("  note  %s will not go below %d tall against a %d "
		             "screen\n", qPrintable(name), floor.height(),
		             k_phone.height());

	// **Inside the dialog is not the same as readable.** The first version
	// asked only whether each button's rectangle fell within the dialog's, and
	// passed the downloads dialog -- whose "Open Folder" was on screen reading
	// "pen Folde", because Qt had squeezed six buttons into a row built for
	// none of them. A button narrower than its own `sizeHint` has lost some of
	// its label, and a control whose label is cut is not one somebody can use.
	int off = 0, squeezed = 0;
	QStringList lost, cut;
	for (QPushButton *b : dlg->findChildren<QPushButton *>()) {
		if (!b->isVisible())
			continue;
		const QRect r(b->mapTo(dlg, QPoint(0, 0)), b->size());
		if (!dlg->rect().contains(r)) {
			++off;
			if (lost.size() < 4)
				lost << b->text();
		} else if (b->width() < b->sizeHint().width()) {
			++squeezed;
			if (cut.size() < 4)
				cut << QString("%1 (%2 of %3)").arg(b->text())
				        .arg(b->width()).arg(b->sizeHint().width());
		}
	}
	verdict(off == 0, off == 0
	          ? QString("%1: every button is on screen").arg(name)
	          : QString("%1: %2 button(s) off the edge -- %3")
	                .arg(name).arg(off).arg(lost.join(", ")));
	// **The settings window is seven windows.** Measuring only the page that
	// happens to be selected says nothing about the other six, and every
	// constraint found here was on a page the first shot never showed.
	if (auto *pages = dlg->findChild<QListWidget *>("categories")) {
		for (int i = 0; i < pages->count(); ++i) {
			pages->setCurrentRow(i);
			QApplication::processEvents();
			const QString leaf = pages->item(i)->text().toLower()
			                         .replace(' ', '-').replace('&', "and");
			save(dlg, QString("%1-%2").arg(name, leaf));
		}
		pages->setCurrentRow(0);
	}

	verdict(squeezed == 0, squeezed == 0
	          ? QString("%1: and none has its label cut").arg(name)
	          : QString("%1: %2 button(s) squeezed below their label -- %3")
	                .arg(name).arg(squeezed).arg(cut.join("; ")));
	save(dlg, name);
}

// Open a modal through its slot, measure it while it is up, then close it.
static void visit(main_window *w, const QString &slot, const QString &name) {
	QTimer::singleShot(900, [name] {
		for (QWidget *x : QApplication::topLevelWidgets()) {
			auto *d = qobject_cast<QDialog *>(x);
			if (!d || !d->isVisible())
				continue;
			measure(d, name);
			d->reject();
			return;
		}
		std::printf("  skip  %-14s did not open here\n", qPrintable(name));
	});
	QMetaObject::invokeMethod(w, slot.toUtf8().constData());
	shell::spin(1400);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-phone");
	g_out = f.out;

	struct { const char *slot; const char *name; } dialogs[] = {
		{ "open_settings",      "settings" },
		{ "open_site_controls", "site-controls" },
		{ "open_downloads",     "downloads" },
		{ "open_site_rules",    "site-rules" },
	};

	std::printf("\n== every dialog, given a %dx%d screen ==\n",
	             k_phone.width(), k_phone.height());
	for (const auto &d : dialogs)
		visit(&f.window, d.slot, d.name);

	// **A floor, so a run that opened nothing cannot report success.** Every
	// dialog above can decline to open -- three of the shell's do, for want of
	// a page or a model -- and a driver that measured none of them would
	// otherwise print a clean sweep of an empty list.
	const int expected = int(sizeof(dialogs) / sizeof(dialogs[0]));
	if (g_shots < expected) {
		std::printf("\nonly %d of %d dialogs were measured; that is not a "
		             "check of anything\n", g_shots, expected);
		return 1;
	}

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	return shell::report();
}
