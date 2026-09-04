// Probing in the settings window must be a button, never a side effect of
// opening it.
#include "settings_dialog.h"
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
	const QString up = argc > 1 ? argv[1] : "http://127.0.0.1:8811";

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
	// Which answer is correct depends on whether anything is actually running,
	// and this suite is meant to need nothing but a build. So assert the shape
	// of both answers rather than assuming a backend is up: the failure this
	// guards against is a status that stays on "not checked yet" after a probe
	// completes, which is what a dropped signal looks like.
	if (status->text().contains("reachable")) {
		check(true, QString("the status reports the running model (%1)").arg(status->text()));
	} else {
		check(status->text().contains("Neither backend is available"),
		      QString("no backend is up, and the status says so (%1)").arg(status->text()));
		std::printf("  --    (no AI backend running; the reachable path went unchecked)\n");
	}

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
