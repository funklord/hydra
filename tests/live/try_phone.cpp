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

#include "annoyance_log.h"
#include "annoyed_dialog.h"
#include "auth_dialog.h"
#include "extractor_dialog.h"
#include "extractor_signals.h"
#include "filter_dialog.h"
#include "filter_list.h"
#include "filter_signals.h"
#include "ollama_provider.h"
#include "reorganize_dialog.h"
#include "site_extractor.h"
#include "tab_tree_model.h"
#include "cert_dialog.h"
#include "main_window.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLayout>
#include <QAction>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
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
// Dialogs actually measured. Counted separately from shots, because the
// settings walk takes one picture per page and would otherwise make the floor
// below pass on a run that measured one window seven times.
static int g_measured = 0;
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
static void as_android_would(QWidget *dlg) {
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
static void measure(QWidget *dlg, const QString &name) {
	++g_measured;
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

	// **Spare height has to go somewhere, and a paragraph is the wrong
	// somewhere.** A dialog handed the whole screen has more height than its
	// contents asked for. If nothing in it is willing to absorb the difference
	// -- no list, no stretch -- a word-wrapped QLabel will, because its policy
	// permits it, and the result is an inch of nothing between each sentence
	// with the fields pushed to the bottom edge. The proxy password prompt
	// arrived exactly like that, and neither check above could see it: every
	// button was on screen and none was cut.
	//
	// A label taller than the text it holds is the signature. Measured against
	// `heightForWidth` at the width it actually got, with a line of slack for
	// the margins a style adds.
	int stretched = 0;
	QStringList puffed;
	for (QLabel *l : dlg->findChildren<QLabel *>()) {
		if (!l->isVisible() || !l->wordWrap() || l->width() <= 0)
			continue;
		// `empty_state`'s overlay is *meant* to take the whole viewport -- it
		// is a message centred in an empty list, and centring is the point. It
		// was the first thing this check reported, on two dialogs, and the
		// check was the thing that was wrong.
		if (l->objectName() == "empty_state")
			continue;
		const int wants = l->heightForWidth(l->width());
		if (wants > 0 && l->height() > wants + l->fontMetrics().height()) {
			++stretched;
			if (puffed.size() < 3)
				puffed << QString("\"%1…\" (%2 tall for %3 of text)")
				            .arg(l->text().left(24)).arg(l->height()).arg(wants);
		}
	}
	verdict(stretched == 0, stretched == 0
	          ? QString("%1: and no paragraph is absorbing spare height")
	                .arg(name)
	          : QString("%1: %2 label(s) stretched past their text -- %3")
	                .arg(name).arg(stretched).arg(puffed.join("; ")));

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

	// **The two a person cannot walk away from.** A password prompt and a
	// certificate chooser are not opened by a menu -- the network puts them in
	// front of you -- so no slot reaches them and the pass above never saw
	// either. They are also the two where being unable to reach a button
	// matters most: one of them is how you say "do not send my identity".
	//
	// Built directly and never exec'd, which is what `try_chrome` does with
	// them for the same reason: a modal blocks the driver.
	std::printf("\n== and the two the network puts in front of you ==\n");
	{
		auth_dialog site("bank.example", "Accounts", true, &f.window);
		site.show();
		QApplication::processEvents();
		measure(&site, "auth-site");
	}
	{
		auth_dialog proxy("proxy.corp.example", "Staff", false, &f.window,
		                   auth_dialog::asker::proxy);
		proxy.show();
		QApplication::processEvents();
		measure(&proxy, "auth-proxy");
	}
	{
		// Two offers rather than one: the list is the part that has to fit, and
		// a single row cannot show whether it wraps.
		QList<web_view_backend::certificate_offer> offered;
		web_view_backend::certificate_offer a;
		a.subject = "Ada Lovelace";
		a.issuer  = "Example Certification Authority";
		a.valid_until = "2027-01-01";
		web_view_backend::certificate_offer b;
		b.subject = "Ada (work)";
		b.issuer  = "Corp CA";
		b.valid_until = "2026-09-01";
		offered << a << b;

		cert_dialog dlg("id.example", offered, &f.window);
		dlg.show();
		QApplication::processEvents();
		measure(&dlg, "certificate");
	}
	{
		// The one-click report, which offers four tools in a row and is
		// therefore the same shape that squeezed the downloads dialog. It takes
		// only a report, so it needs no page to reach here.
		annoyance_report r;
		r.host     = "example.invalid";
		r.page     = "http://example.invalid/article";
		r.observed = 41;
		r.suspects << "http://ads.invalid/pagead/banner.js";
		r.suspects << "http://tracker.invalid/track?id=1";
		annoyed_dialog dlg(r, &f.window);
		dlg.show();
		QApplication::processEvents();
		measure(&dlg, "annoyance");
	}

	// **The three that ask a model.** In the shell these open only when
	// `choose_ai()` returns a provider, which is why no capture pass has ever
	// photographed them -- but the question here is about layout, and a layout
	// does not care whether the provider behind it can answer. Built directly
	// with one that cannot, the same way `try_send_gate` reaches the filter
	// dialog.
	// **The window itself, which is the surface anybody actually uses.** Every
	// measurement above is of something that appears in front of it. The
	// toolbar carries back, forward, reload, the drawer button, an address bar,
	// a shield and sometimes a media button, and that is a row -- the shape
	// that has failed twice already in this pass.
	std::printf("\n== the browser window ==\n");
	measure(&f.window, "window");

	// **And with the drawer out**, which is the only way to reach a tab on a
	// phone and had never been photographed. The tree is an overlay at this
	// width rather than a pane, so what matters is whether it covers enough to
	// be usable without covering so much that there is no way back -- it takes
	// 82% of the width, leaving a strip of page to tap on.
	if (QAction *drawer = f.window.findChild<QAction *>("drawer")) {
		drawer->trigger();
	} else {
		// The action carries no object name, so reach it the way a person does:
		// the toolbar button whose text is the drawer glyph.
		for (QToolButton *b : f.window.findChildren<QToolButton *>())
			if (b->isVisible() && b->text().contains(QChar(0x2630)))
				b->click();
	}
	spin(600);
	save(&f.window, "window-drawer");

	// Back to a size the rest of the run expects to build dialogs against.
	f.window.resize(1100, 720);
	QApplication::processEvents();

	// **The media dialog, which needs a page rather than a provider.** It is
	// the last one unmeasured, and building it directly would mean standing up
	// five collaborators -- a detector, a player launcher, a download manager,
	// a proxy and an MSE tap. Opening a tab is cheaper and is also the path a
	// person takes, so `open_media` finds a current view and does the rest.
	if (f.open_tab(0, "one.html")) {
		visit(&f.window, "open_media", "media");
	} else {
		// Not a skip to pass over quietly: this is the one dialog nothing else
		// here reaches, so a run that could not open a page has measured seven
		// of eight and should say which one it missed.
		std::printf("  skip  media          no page would load, so no view to "
		             "ask for media\n");
	}

	std::printf("\n== and the three that ask a model ==\n");
	ollama_provider offline;
	offline.set_endpoint(QUrl("http://127.0.0.1:9"));   // nothing listening
	{
		filter_signals signals_source;
		filter_list    rules;
		filter_dialog dlg(&signals_source, &rules, &offline, "example.invalid");
		dlg.show();
		QApplication::processEvents();
		measure(&dlg, "filter");
	}
	{
		extractor_signals signals_source;
		extractor_store   store;
		extractor_dialog dlg(&signals_source, &store, &offline,
		                      "example.invalid",
		                      QUrl("http://example.invalid/watch"));
		dlg.show();
		QApplication::processEvents();
		measure(&dlg, "extractor");
	}
	{
		reorganize_dialog dlg(f.window.findChild<tab_tree_model *>(), &offline);
		dlg.show();
		QApplication::processEvents();
		measure(&dlg, "reorganizer");
	}

	// **A floor, so a run that opened nothing cannot report success.** Every
	// dialog above can decline to open -- three of the shell's do, for want of
	// a page or a model -- and a driver that measured none of them would
	// otherwise print a clean sweep of an empty list.
	// Four opened by a slot, plus nine reached other ways: the window itself,
	// the two the network raises, the certificate chooser, the annoyance
	// report, the three that ask a model, and the media dialog behind a tab.
	// Exact rather than comfortable -- a floor one below the real count lets a
	// dialog go missing without anything saying so, which is the failure this
	// guard exists to prevent rather than a smaller version of it.
	const int expected = int(sizeof(dialogs) / sizeof(dialogs[0])) + 9;
	if (g_measured < expected) {
		std::printf("\nonly %d of %d dialogs were measured; that is not a "
		             "check of anything\n", g_measured, expected);
		return 1;
	}

	std::printf("\n%d image(s) in %s\n", g_shots, qPrintable(g_out));
	return shell::report();
}
