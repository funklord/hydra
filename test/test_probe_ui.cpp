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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
