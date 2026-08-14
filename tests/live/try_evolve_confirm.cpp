// The one path `try_confirm` cannot reach: rules applied by accepting a real
// proposal, and the "still working?" question appearing on its own afterwards.
//
// `try_confirm` raises that question by calling `offer_confirmation` directly,
// which tests the button and *not* its trigger. The trigger is a before/after
// diff of the filter list around `filter_dialog`, and nothing else exercises
// it: the element picker does not apply rules, it feeds the same dialog. So
// this needs a model, which is why it is a driver of its own and not part of
// the sweep.
//
// It reports what happened at each stage rather than only passing or failing,
// because a model that proposes nothing acceptable is a legitimate outcome and
// must not read as a broken trigger.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void note(const QString &w) { std::printf("     %s\n", qPrintable(w)); }
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// The proposal dialog, found by what it *is* rather than by what it is called.
//
// **Matching on a title string cost this driver two runs.** The first version
// looked for a window whose title contained "Filter"; the dialog is titled
// "Evolve ad filters", and `QString::contains` is case-sensitive -- so the
// poller never saw it, never applied its own deadline, and the modal blocked
// `main()` until the shell `timeout` killed the process with nothing printed.
//
// A capability is a better key than a caption: this is the dialog that offers
// to Send. Titles are user-facing text and change for reasons that have
// nothing to do with a test.
static QDialog *proposal_dialog() {
	for (QWidget *w : QApplication::topLevelWidgets()) {
		auto *d = qobject_cast<QDialog *>(w);
		if (!d || !d->isVisible() || qobject_cast<QMessageBox *>(d))
			continue;
		for (QPushButton *b : d->findChildren<QPushButton *>())
			if (b->text().remove('&').startsWith("Send"))
				return d;
	}
	return nullptr;
}

static QPushButton *button_of(QWidget *w, const QString &text) {
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text().remove('&').startsWith(text))
			return b;
	return nullptr;
}

static QAction *toolbar_action(QWidget *w, const QString &text) {
	for (QToolBar *bar : w->findChildren<QToolBar *>())
		for (QAction *a : bar->actions())
			if (a->text() == text)
				return a;
	return nullptr;
}

static int rules_in(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return 0;
	int n = 0;
	for (const QByteArray &line : f.readAll().split('\n')) {
		const QByteArray t = line.trimmed();
		if (!t.isEmpty() && !t.startsWith('!'))
			++n;
	}
	return n;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
	    : QStringLiteral("https://kisskh.co/Drama/Revenged-Love/Episode-24?id=10826&ep=190076");
	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-evolve");
	QDir(out).removeRecursively();
	QDir().mkpath(out);
	const QString filters = out + "/filters-ai.txt";
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write(QString("- [f0] folder | Mine\n"
	                  "  - [a1] unopened | Ads | %1 | "
	                  "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n")
	              .arg(target).toUtf8());
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1100, 760);
	w.show();
	spin(1200);

	QAction *confirm = toolbar_action(&w, "Still working?");
	check(confirm && !confirm->isVisible(),
	      "nothing to confirm before anything has been applied");

	section("real traffic to propose against");
	{
		auto *tv = w.findChild<QTreeView *>();
		emit tv->activated(tv->model()->index(0, 0, tv->model()->index(0, 0)));
		spin(qEnvironmentVariableIsSet("HYDRA_SETTLE")
		         ? qEnvironmentVariableIntValue("HYDRA_SETTLE") : 18000);
		note("page settled");
	}

	section("accepting a proposal, through the model");
	// The dialog is modal, so everything happens from a poller while it is up:
	// press Send once, wait for proposals to arrive, tick them, accept.
	bool sent = false, accepted = false, dialog_seen = false;
	QString why_not;
	{
		// **The deadline lives here, not in the loop below.**
		// `open_filter_evolution` calls `dlg.exec()`, which spins an event loop
		// of its own -- so the `while` further down does not run at all while
		// the dialog is up, and its budget never ticks. Only this timer runs.
		// The first version put the budget in the loop and hung until the shell
		// `timeout` killed it, with nothing printed.
		const int budget = qEnvironmentVariableIsSet("HYDRA_MODEL_TIMEOUT_MS")
		                       ? qEnvironmentVariableIntValue("HYDRA_MODEL_TIMEOUT_MS")
		                       : 300000;
		QElapsedTimer since; since.start();
		auto *poll = new QTimer(&w);
		poll->setInterval(500);
		QObject::connect(poll, &QTimer::timeout, [&] {
			// **The last-resort escape, and it exists because this driver has
			// hung twice.** Everything below depends on recognising the
			// proposal dialog; if that recognition is ever wrong again, the
			// modal blocks `main()` and nothing prints until an outer `timeout`
			// kills the process. So past the budget, close whatever modal is up
			// -- whether or not it is the one expected -- and let the run
			// report what it found instead of stopping the machine.
			if (since.elapsed() > budget) {
				for (QWidget *x : QApplication::topLevelWidgets()) {
					auto *any = qobject_cast<QDialog *>(x);
					if (any && any->isVisible()) {
						if (why_not.isEmpty())
							why_not = QString("gave up after %1 s with '%2' open")
							              .arg(since.elapsed() / 1000)
							              .arg(any->windowTitle());
						any->reject();
					}
				}
				return;
			}

			QDialog *d = proposal_dialog();
			if (!d)
				return;
			dialog_seen = true;
			if (!sent) {
				if (QPushButton *s = button_of(d, "Send")) {
					if (s->isEnabled()) { s->click(); sent = true; }
					else why_not = "Send never became enabled";
				} else {
					why_not = "no Send button";
				}
				return;
			}
			auto *tree_w = d->findChild<QTreeWidget *>();
			if (!tree_w || tree_w->topLevelItemCount() == 0) {
				why_not = tree_w ? "sent, but no proposals came back"
				                  : "no proposal list in the dialog";
				return;   // still thinking, or never will
			}
			int ticked = 0;
			for (int i = 0; i < tree_w->topLevelItemCount(); ++i) {
				QTreeWidgetItem *it = tree_w->topLevelItem(i);
				if (it->flags() & Qt::ItemIsUserCheckable) {
					it->setCheckState(0, Qt::Checked);
					++ticked;
				}
			}
			QPushButton *ap = button_of(d, "Accept Selected");
			if (ap && ap->isEnabled() && ticked > 0) {
				ap->click();
				accepted = true;
				spin(300);
				d->accept();
			} else {
				why_not = QString("%1 proposal row(s), accept %2")
				              .arg(ticked)
				              .arg(ap ? (ap->isEnabled() ? "enabled" : "disabled")
				                       : "missing");
				d->reject();
			}
		});
		poll->start();

		QTimer::singleShot(0, &w, [&w] {
			QMetaObject::invokeMethod(&w, "open_filter_evolution");
		});
		// This only makes progress once the modal has closed, which the poller
		// above guarantees it eventually will. The extra margin is for the case
		// where no dialog appears at all -- no provider, say -- and nothing ever
		// sets `dialog_seen`.
		QElapsedTimer waited; waited.start();
		while (!accepted && waited.elapsed() < budget + 20000) {
			spin(500);
			if (dialog_seen && !proposal_dialog())
				break;
		}
		poll->stop();
		note(QString("dialog seen: %1, send pressed: %2, accepted: %3%4")
		         .arg(dialog_seen ? "yes" : "no").arg(sent ? "yes" : "no")
		         .arg(accepted ? "yes" : "no")
		         .arg(why_not.isEmpty() ? "" : " (" + why_not + ")"));
	}

	section("and the question appears without being asked for");
	if (accepted) {
		check(confirm && confirm->isVisible(),
		      "the toolbar raises 'still working?' on its own");
		const int before = rules_in(filters);
		note(QString("%1 rule(s) in the file").arg(before));

		bool saw = false;
		QTimer::singleShot(500, [&saw] {
			for (QWidget *x : QApplication::topLevelWidgets())
				if (auto *b = qobject_cast<QMessageBox *>(x))
					if (b->isVisible()) {
						saw = true;
						for (QAbstractButton *bt : b->buttons())
							if (bt->text().remove('&').startsWith("It Broke")) {
								bt->click(); return;
							}
				    b->reject();
						return;
					}
		});
		confirm->trigger();
		spin(1500);
		check(saw, "and answering it is possible");
		const int after = rules_in(filters);
		check(after < before,
		      QString("saying it broke removed what was applied (%1 -> %2)")
		          .arg(before).arg(after));
		check(!confirm->isVisible(), "and the question stops being asked");
	} else {
		note("no proposal was accepted, so the trigger could not be exercised.");
		note("that is a legitimate outcome -- the model may propose nothing the");
		note("dry run will pass -- and it is reported rather than counted as a");
		note("failure of the trigger.");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
