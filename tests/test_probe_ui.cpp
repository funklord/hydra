// Probing in the settings window must be a button, never a side effect of
// opening it.
#include "settings_dialog.h"
#include "download_manager.h"
#include "player_launcher.h"
#include "torrent_download_source.h"
#include "ollama_provider.h"
#include "claude_provider.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QLabel>
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
	check(status->text().contains("reachable"),
	      QString("the status reports the running model (%1)").arg(status->text()));

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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
