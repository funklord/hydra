// The settings window's layout and its site-defaults page, driven.
//
// The layout question was settled by looking at what other browsers do rather
// than by taste: Firefox, Chrome and Vivaldi all present preferences as a
// *category list* beside a stack, not as tabs, and all three moved that way as
// the count grew past a handful. A tab strip runs out of width and starts
// eliding; a vertical list has room for a name that says what is inside.
//
// The page this checks hardest is the new one. Every per-site feature had a
// global default that only the shield could reach, so the answer to "what does
// this browser allow by default" lived in a popup attached to whichever page
// happened to be open. Firefox's privacy pane is the model for the grouping,
// including putting cookie-banner handling directly after site data — the
// banner is a question about cookies, and permissions are a different subject.
#include "settings_dialog.h"
#include "policy_engine.h"
#include "player_launcher.h"
#include "download_manager.h"

#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QSettings>
#include <QStackedWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// Never the user's real configuration.
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
	                    qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
	                        : QString("/tmp/hydra-test"));

	policy_engine   policy;
	player_launcher players;
	download_manager downloads;

	section("the window is a list of categories, not a tab strip");
	settings_dialog dlg(&players, &downloads, nullptr, nullptr, nullptr, &policy);
	dlg.show();

	auto *cats  = dlg.findChild<QListWidget *>("categories");
	auto *pages = dlg.findChild<QStackedWidget *>("pages");
	check(cats && pages, "it has a category list and a page stack");
	if (!cats || !pages) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	check(cats->count() == pages->count(),
	      QString("every category has a page (%1)").arg(cats->count()));
	check(cats->count() >= 4, "and there are enough of them to justify a list");
	check(cats->item(0)->text().startsWith("Privacy"),
	      "privacy comes first — it is the page about what the browser refuses "
	      "to do on your behalf; the rest are conveniences");

	// Selecting a category shows its page. Wired-and-never-clicked is this
	// project's most repeated defect, so the connection is exercised.
	cats->setCurrentRow(2);
	check(pages->currentIndex() == 2, "choosing a category shows its page");
	cats->setCurrentRow(0);
	check(pages->currentIndex() == 0, "and going back shows the first again");

	section("every per-site feature is reachable here");
	{
		// The point of the sweep in build_privacy_page: a feature added to the
		// model must appear even if nobody remembered to place it in a group.
		// Four were added to that enum in a week, so this is not hypothetical.
		int missing = 0;
		QString names;
		for (int i = 0; i < policy::feature_count(); ++i) {
			const auto f = static_cast<policy::feature>(i);
			const QString id = QString("feature_%1").arg(policy::feature_name(f));
			if (!dlg.findChild<QComboBox *>(id)) {
				++missing;
				names += QString(policy::feature_name(f)) + " ";
			}
		}
		check(missing == 0,
		      QString("all %1 features have a control%2")
		          .arg(policy::feature_count())
		          .arg(missing ? QString(" — missing: " + names) : QString()));

		auto *banners = dlg.findChild<QComboBox *>("feature_cookieNotices");
		check(banners != nullptr,
		      "including the cookie-banner blocker, which until now could only "
		      "be reached from the shield");

		// Global defaults are allow or block. "Default" is what a site says when
		// it has no opinion and falls through to here, so offering it at this
		// level would be a setting that points at itself.
		auto *js = dlg.findChild<QComboBox *>("feature_javascript");
		check(js && js->count() == 2,
		      "a global default is allow or block, never \"default\"");
		check(js && js->currentText() == "Allow",
		      "and it shows what the engine actually holds");
		auto *ads = dlg.findChild<QComboBox *>("feature_ads");
		check(ads && ads->currentText() == "Block",
		      "for each feature separately, not one guessed value");
	}

	section("changing one takes effect");
	{
		auto *ads = dlg.findChild<QComboBox *>("feature_ads");
		check(policy.global_default(policy::feature::ads) == policy::setting::block,
		      "ads start blocked");
		ads->setCurrentIndex(0);            // Allow
		dlg.accept();                        // OK applies
		check(policy.global_default(policy::feature::ads) == policy::setting::allow,
		      "accepting writes the change into the engine");
		check(policy.global_default(policy::feature::javascript) ==
		          policy::setting::allow,
		      "and leaves the ones nobody touched alone");
	}

	section("cancelling does not");
	{
		settings_dialog d2(&players, &downloads, nullptr, nullptr, nullptr, &policy);
		d2.show();
		auto *js = d2.findChild<QComboBox *>("feature_javascript");
		js->setCurrentIndex(1);             // Block
		d2.reject();
		check(policy.global_default(policy::feature::javascript) ==
		          policy::setting::allow,
		      "closing without accepting changes nothing");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
