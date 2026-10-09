// Probing in the settings window must be a button, never a side effect of
// opening it.
//
// **The backend is in-process**, from `ollama_stub.h`. It used to be a stub
// Ollama on port 8811 started by hand, which is why this suite sat outside
// `make test` -- and why the reachable branch below said so and checked
// nothing: the file's own comment explains that it asserts the shape of both
// answers because "this suite is meant to need nothing but a build". It now
// needs nothing but a build AND takes the reachable branch, which is the one
// a person sees when their model is running.
#include "settings_dialog.h"
#include "filter_subscription.h"
#include "subscription_updater.h"
#include "ollama_stub.h"
#include "filter_list.h"
#include "download_manager.h"
#include "player_launcher.h"
#include "torrent_download_source.h"
#include "ollama_provider.h"
#include "claude_provider.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	ollama_stub stub;
	const QString up = stub.start();
	if (up.isEmpty()) {
		std::printf("could not listen\n");
		return 1;
	}

	const QString tmp = QDir::temp().filePath("hydra-probeui-test");
	QDir(tmp).removeRecursively();
	QDir().mkpath(tmp);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp);

	player_launcher players;
	download_manager downloads;
	auto *tor = new torrent_download_source;
	downloads.add_source(tor);
	ollama_provider local_ai;
	claude_provider external_ai;
	local_ai.set_endpoint(QUrl(up));
	// Named from the provider rather than repeated here: `ready()` separates
	// "running" from "running and has the model you asked for", and a stub
	// offering some other name would put the dialog in the third state.
	stub.models = { local_ai.model() };

	QSignalSpy probes(&local_ai, &ollama_provider::probe_finished);

	settings_dialog dlg(&players, &downloads, tor, &local_ai, &external_ai);
	dlg.show();
	spin(1200);

	section("opening the window probes nothing");
	check(probes.count() == 0,
	      QString("no probe was issued on open (%1)").arg(probes.count()));
	auto *status = dlg.findChild<QLabel *>("ai_status");
	check(status && status->text().contains("not checked yet"),
	      QString("and it says so rather than claiming unavailable (%1)")
	          .arg(status ? status->text() : QString()));

	section("the button does the probing");
	auto *btn = dlg.findChild<QPushButton *>("check_local");
	check(btn != nullptr, "there is a Check now button");
	btn->click();
	check(!btn->isEnabled(), "it disables itself while in flight");
	spin(2000);
	check(probes.count() == 1, QString("one probe ran (%1)").arg(probes.count()));
	check(btn->isEnabled(), "and it comes back");
	// **The reachable branch, asserted rather than accommodated.** This used
	// to accept either answer, because whether a backend was up depended on
	// what the person running it had started -- so the case a person actually
	// meets, a model that answers, was the one never checked. The stub is up
	// by construction now, so the status must say so; the failure this guards
	// against is unchanged, a status that stays on "not checked yet" after a
	// probe completes, which is what a dropped signal looks like.
	check(status->text().contains("reachable"),
	      QString("the status reports the running model (%1)").arg(status->text()));
	check(stub.seen.contains("/api/tags"),
	      QString("and the probe really asked it (%1)").arg(stub.seen.join(", ")));

	section("and it tests what is in the field, not what was saved");
	auto *url = dlg.findChildren<QLineEdit *>().value(0);
	// Find the endpoint field by its current contents.
	for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
		if (e->text() == up)
			url = e;
	url->setText("http://127.0.0.1:9");     // nothing listening
	btn->click();
	spin(2500);
	check(probes.count() == 2, "a second probe ran");
	// With Automatic selected and no API key the honest message is that
	// neither backend is available, which is more useful than naming only the
	// local one. Either wording is a correct report of an unreachable probe.
	check(!status->text().contains("is reachable"),
	      "it no longer claims the model is reachable");
	check(status->text().contains("did not answer") ||
	          status->text().contains("Neither backend"),
	      QString("and reports the edited endpoint's failure (%1)")
	          .arg(status->text()));

	section("rescanning for players is a button too");
	auto *rescan = dlg.findChild<QPushButton *>("rescan_players");
	check(rescan != nullptr, "there is a Rescan button");
	const int before = dlg.findChildren<QRadioButton *>().size();
	rescan->click();
	spin(300);
	check(dlg.findChildren<QRadioButton *>().size() == before,
	      "rescanning keeps the list consistent rather than duplicating it");

	// **A write that did not happen used to look exactly like one that did.**
	// Every store here reports honestly -- `QSaveFile`, `return f.commit()` --
	// and every caller dropped the answer, so a full disk or an unwritable
	// profile lost the change in silence. It matters most on the controls that
	// apply *immediately* rather than at OK, and this is one: the rule stops
	// blocking the moment the button is pressed, so the list on screen is
	// already correct and nothing looks wrong until the next launch puts the
	// rule back.
	//
	// Driven through the button, because the defect was never in the store. It
	// was in the caller, and a test that asks the store whether it agrees with
	// itself cannot see a caller that ignores the answer.
	section("a filter list that cannot be written says so");
	{
		auto with_one_rule = [](filter_list *f) {
			filter_rule r;
			r.text = "||ads.example.com^";
			r.note = "test";
			f->add(r);
		};
		auto remove_first = [](settings_dialog *d) -> QString {
			auto *view   = d->findChild<QTreeWidget *>("filters");
			auto *remove = d->findChild<QPushButton *>("filter_remove");
			auto *note   = d->findChild<QLabel *>("filter_note");
			if (!view || !remove || !note || view->topLevelItemCount() == 0)
				return QString();
			view->setCurrentItem(view->topLevelItem(0));
			remove->click();
			return note->text();
		};

		// A path whose *parent* does not exist, so `QSaveFile` cannot place its
		// temporary alongside the target. Chosen over a read-only directory
		// because it fails for root too, and a check root cannot fail is not a
		// check.
		filter_list bad_list;
		with_one_rule(&bad_list);
		settings_dialog bad(&players, &downloads, tor, &local_ai, &external_ai,
		                     nullptr, &bad_list,
		                     QDir(tmp).filePath("no-such-dir/filters.txt"));
		const QString said = remove_first(&bad);
		check(!said.isEmpty(), "the filter controls are there and removable");
		check(said.contains("could not be saved"),
		      QString("a failed write is reported (%1)").arg(said));

		// **The control.** The same click against a path that works must not
		// report a failure, or the assertion above would pass equally for a
		// message that is simply always shown.
		const QString fine = QDir(tmp).filePath("filters.txt");
		filter_list good_list;
		with_one_rule(&good_list);
		settings_dialog good(&players, &downloads, tor, &local_ai, &external_ai,
		                      nullptr, &good_list, fine);
		const QString ok_said = remove_first(&good);
		check(!ok_said.isEmpty() && !ok_said.contains("could not be saved"),
		      QString("and a write that works is not (%1)").arg(ok_said));
		check(QFile::exists(fine), "the file really was written");
	}

	section("a download folder that cannot be created is not adopted");
	{
		// `apply()` called `mkpath` on whatever was typed and dropped the
		// answer, so an uncreatable path was stored and every download after it
		// failed separately, with nothing to connect those failures to the
		// setting. `set_directory` is a bare setter and refuses nothing.
		//
		// The unwritable path is a directory *under a regular file*, which
		// nothing can create -- including root, so this needs no euid guard and
		// cannot go quiet on a machine where the suite runs privileged.
		const QString blocker = QDir(tmp).filePath("a-file-not-a-folder");
		{
			QFile f(blocker);
			f.open(QIODevice::WriteOnly);
			f.write("x");
		}
		const QString impossible = blocker + "/downloads";
		const QString good_dir   = QDir(tmp).filePath("downloads-that-work");

		download_manager dm;
		dm.set_directory(good_dir);
		QDir().mkpath(good_dir);
		settings_dialog d(&players, &dm, nullptr, &local_ai, &external_ai);
		QSignalSpy refused(&d, &settings_dialog::could_not_apply);

		auto *box = d.findChild<QLineEdit *>("download_dir");
		check(box != nullptr, "the folder box is there to type into");
		check(box && box->text() == good_dir,
		       "showing the folder the manager already has");

		box->setText(impossible);
		// Through `accept()`, which is the button a person presses: `apply()`
		// is private, and the public route proves the whole path rather than
		// one method of it.
		d.accept();
		check(refused.count() == 1,
		       QString("the dialog says it could not be applied (%1 time(s))")
		         .arg(refused.count()));
		check(refused.count() == 1 &&
		          refused.first().first().toString().contains(impossible),
		       QString("naming the folder that was asked for (%1)")
		         .arg(refused.count() ? refused.first().first().toString()
		                              : QString()));
		check(dm.directory() == good_dir,
		       QString("and downloads still go where they were going (%1)")
		         .arg(dm.directory()));
		check(box && box->text() == good_dir,
		       "with the box back to the folder in use, rather than showing one "
		       "nothing can write to");

		// The control: a folder that can be made is adopted, and silently.
		const QString fresh = QDir(tmp).filePath("downloads-new");
		box->setText(fresh);
		d.accept();
		check(refused.count() == 1,
		       QString("a folder that can be created says nothing (%1)")
		         .arg(refused.count()));
		check(dm.directory() == fresh,
		       QString("and is adopted (%1)").arg(dm.directory()));
		check(QFileInfo(fresh).isDir(), "and created");
	}

	section("the trusted box is wired, and is what turns the power on");
	{
		// **The one thing that can set `subscription::trusted`, and nothing
		// exercised it.** Everything underneath has its own cases -- the read
		// drops an untrusted list's trusted call and counts it, `source_for`
		// refuses it again -- while the checkbox that decides the flag had no
		// caller in any suite. An interface is only as wired as its
		// least-used method, and a column added to a table is exactly the
		// shape that looks done because it compiles.
		//
		// Driven through the item, not through `set_subscriptions`: a test
		// that sets the flag itself would assert that the updater stores what
		// it is handed, which was never the question.
		const QString subs_dir = QDir(tmp).filePath("subs");
		check(QDir().mkpath(subs_dir), "a scratch directory for the cache");
		const QString index = QDir(subs_dir).filePath("subs.json");
		const QString body =
		  "player.test##+js(set-constant, cfg.ads, false)\n"
		  "player.test##+js(trusted-set-cookie, consent, yes, 7)\n";
		{
			QFile f(QDir(subs_dir).filePath("list.txt"));
			check(f.open(QIODevice::WriteOnly | QIODevice::Truncate),
			       "with one cached body in it");
			f.write(body.toUtf8());
		}
		subscription one;
		one.name = "A list with one powerful rule";
		one.url  = QUrl("https://list.test/l.txt");
		one.file = "list.txt";
		subscription_updater up(index, subs_dir);
		check(up.set_subscriptions({ one }), "the index saves");

		settings_dialog d(&players, &downloads, tor, &local_ai, &external_ai);
		d.set_subscription_updater(&up);
		auto *view = d.findChild<QTreeWidget *>("subscriptions");
		check(view && view->topLevelItemCount() == 1,
		       QString("the subscription row is on screen (%1)")
		           .arg(view ? view->topLevelItemCount() : -1));
		if (!view || view->topLevelItemCount() != 1) {
			std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
			return g_fail == 0 ? 0 : 1;
		}
		QTreeWidgetItem *row = view->topLevelItem(0);
		check(row->checkState(1) == Qt::Unchecked,
		       "and its trusted box is clear, which is the default");
		check(!row->toolTip(1).isEmpty() &&
		          row->toolTip(1).contains("cookie"),
		       QString("with a tooltip saying what it allows (%1)")
		           .arg(row->toolTip(1).left(40)));

		// **The row Qt is editing has to survive the click.** `itemChanged`
		// is emitted from inside `QStyledItemDelegate::editorEvent` with
		// `QAbstractItemView::edit` still on the stack, so a handler that
		// rebuilds the tree deletes the item Qt resumes on -- which is the
		// SIGSEGV the holder hit on the Trusted box, with no frame of ours
		// below `QDialog::exec()`.
		//
		// A sentinel in a column the handler ignores is what answers it,
		// rather than comparing pointers: `clear()` frees the row and the
		// refill allocates one the same size, so the old address is likely
		// to come straight back and a pointer check would pass on the
		// allocator's luck. A refilled row is a fresh object and carries no
		// sentinel, whatever address it lands on.
		constexpr int sentinel = Qt::UserRole + 7;
		row->setData(2, sentinel, QString("still here"));

		QSignalSpy told(&d, &settings_dialog::subscriptions_changed);
		row->setCheckState(1, Qt::Checked);
		check(view->topLevelItemCount() == 1 &&
		          view->topLevelItem(0)->data(2, sentinel).toString() ==
		              "still here",
		       "ticking it leaves the row Qt was editing alive");
		// **And it has to still happen, only later.** A deferral that
		// dropped the rebuild would pass the check above just as
		// quietly, and the column would then stop reflecting a fetch.
		QCoreApplication::processEvents();
		check(view->topLevelItemCount() == 1 &&
		          view->topLevelItem(0)->data(2, sentinel).isNull(),
		       "and the rebuild lands once the event has finished");
		row = view->topLevelItem(0);
		check(row->checkState(1) == Qt::Checked,
		       "with the box it redraws agreeing with what was written");
		check(!up.subscriptions().isEmpty() &&
		          up.subscriptions().first().trusted,
		       "ticking it sets the flag on the subscription");
		check(told.count() == 1,
		       QString("and tells the shell to re-read the cached bodies "
		                "(%1)").arg(told.count()));
		check(!filter_subscription::load_index(index).isEmpty() &&
		          filter_subscription::load_index(index).first().trusted,
		       "and it survives in the index on disk");

		// **The box joined up to the effect**, which is the half a wiring
		// test usually leaves out: the flag is only worth setting if the read
		// keeps the call it was blocking.
		const subscription_read now =
		  filter_subscription::read(body, 0,
		                             up.subscriptions().first().trusted);
		check(now.scriptlets == 2 && now.needs_trust == 0,
		       QString("the cached body now yields both scriptlets (%1)")
		           .arg(now.summary()));

		// **The control, and it is the case that separates reading the column
		// from writing whichever box was touched.** A handler that fell
		// through would set `enabled` from an untouched box -- harmless and
		// invisible -- and leave `trusted` alone, so the two boxes have to be
		// shown not to be one wire.
		row->setCheckState(0, Qt::Unchecked);
		check(!up.subscriptions().first().enabled &&
		          up.subscriptions().first().trusted,
		       "unticking enabled leaves trusted alone");
		row->setCheckState(1, Qt::Unchecked);
		check(!up.subscriptions().first().trusted &&
		          !up.subscriptions().first().enabled,
		       "and unticking trusted clears only that");
		const subscription_read back =
		  filter_subscription::read(body, 0,
		                             up.subscriptions().first().trusted);
		check(back.scriptlets == 1 && back.needs_trust == 1,
		       QString("so the trusted call is dropped again (%1)")
		           .arg(back.summary()));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
