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
// including putting cookie-banner handling directly after site data -- the
// banner is a question about cookies, and permissions are a different subject.
#include "settings_dialog.h"
#include "policy_engine.h"
#include "filter_list.h"
#include "consent_blocker.h"
#include "site_rules.h"
#include "player_launcher.h"
#include "download_manager.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QPushButton>
#include <QTreeWidget>
#include "settings_bundle.h"
#include "theme.h"
#include <QComboBox>
#include <QLineEdit>
#include <QEventLoop>
#include <QTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QDir>
#include <QLabel>
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

// Let the stack finish switching before a picture is taken; everything else in
// this driver is synchronous and needs no waiting at all.
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

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
	dlg.resize(920, 740);
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

	section("finding a setting without knowing which page it is on");
	{
		// What makes a category list scale. Six pages is already more than
		// anyone will read through to find one switch, which is why Firefox and
		// Chrome both have this.
		auto *search  = dlg.findChild<QLineEdit *>("settings_search");
		auto *results = dlg.findChild<QListWidget *>("settings_results");
		check(search && results, "there is a search box");
		check(results && !results->isVisible(),
		      "showing nothing until something is typed");

		search->setText("cookie");
		check(results->count() > 0, "typing finds matches");
		bool mentions_page = false;
		for (int i = 0; i < results->count(); ++i)
			if (results->item(i)->text().contains("Privacy"))
				mentions_page = true;
		check(mentions_page,
		      "and each says which page it is on, since that is the question");

		// A result has to take you there, or it has answered nothing.
		search->setText("Kiosk");
		bool jumped = false;
		for (int i = 0; i < results->count(); ++i) {
			const int page = results->item(i)->data(Qt::UserRole).toInt();
			if (cats->item(page)->text() == "Kiosk") {
				emit results->itemActivated(results->item(i));
				jumped = (pages->currentIndex() == page);
				break;
			}
		}
		check(jumped, "choosing a result switches to its page");

		search->setText("zzzzz");
		check(results->count() == 0 && !results->isVisible(),
		      "and nothing matching hides the list rather than leaving it stale");
		search->clear();
		cats->setCurrentRow(0);
	}

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

	section("filters, which could be accepted but never taken back");
	{
		const QString fpath = QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT")) +
		                       "/filters-ai.txt";
		QFile::remove(fpath);
		filter_list filters;
		filter_rule a; a.text = "||ads.example.com^"; a.note = "beacon";
		filter_rule b; b.text = "shop.example##.promo"; b.cosmetic = true;
		b.scope = "shop.example"; b.note = "leaked banner";
		filters.add(a);
		filters.add(b);
		filters.save(fpath);

		settings_dialog df(&players, &downloads, nullptr, nullptr, nullptr,
		                    &policy, &filters, fpath);
		df.show();
		auto *view = df.findChild<QTreeWidget *>("filters");
		auto *rm   = df.findChild<QPushButton *>("filter_remove");
		check(view && view->topLevelItemCount() == 2,
		      "both accepted rules are listed");
		check(view && view->topLevelItem(0)->text(1) == "every site",
		      "a rule with no scope says so rather than showing an empty cell");
		check(rm && !rm->isEnabled(), "nothing can be removed until one is picked");

		view->setCurrentItem(view->topLevelItem(0));
		check(rm && rm->isEnabled(), "picking one enables it");
		rm->click();
		check(filters.rules().size() == 1, "removing takes it out of the list");
		check(!filters.blocks("https://ads.example.com/x.gif", "site.example"),
		      "and it stops blocking immediately, not at the next restart");
		check(view->topLevelItemCount() == 1, "the view keeps up");

		// Written through at once. The alternative is a window where Cancel
		// silently restores rules the user watched disappear.
		filter_list reloaded;
		check(reloaded.load(fpath) && reloaded.rules().size() == 1,
		      "and the file on disk agrees without waiting for OK");
	}

	section("finding the rules that should be shipped as built-ins");
	{
		// The requirement this serves: a generic rule learned locally is flagged
		// for the next release. Flagging is worth nothing if the only way to find
		// the flagged ones is reading a JSON file by hand.
		const QString rpath = QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT")) +
		                       "/site-rules.ini";
		QFile::remove(rpath);
		consent_blocker blocker(&policy);
		site_rules rules = site_rules::defaults();
		rules.add(blocker.rule_from_label("Avvis alle", "reject"));
		site_rule host_only;
		host_only.kind = "container";
		host_only.value = "#weird-banner";
		host_only.host = "one.example";
		rules.add(host_only);
		blocker.set_rules(rules);

		settings_dialog dr(&players, &downloads, nullptr, nullptr, nullptr,
		                    &policy, nullptr, QString(), &blocker, rpath);
		dr.show();
		auto *view = dr.findChild<QTreeWidget *>("site_rules");
		check(view && view->topLevelItemCount() == 2,
		      "the learned rules are listed");
		check(view && view->topLevelItemCount() ==
		          [&]{ int n = 0; for (const site_rule &r : blocker.rules().all())
				               if (!r.builtin) ++n; return n; }(),
				  "and the built-ins are not, since they come from the program");

		QString flagged_text, host_text;
		for (int i = 0; i < view->topLevelItemCount(); ++i) {
			if (view->topLevelItem(i)->text(0).contains("Avvis"))
				flagged_text = view->topLevelItem(i)->text(3);
			if (view->topLevelItem(i)->text(0).contains("weird"))
				host_text = view->topLevelItem(i)->text(3);
		}
		check(flagged_text.contains("built-in"),
		      "a generic rule says in words that it should be shipped");
		check(host_text.isEmpty(),
		      "and a rule tied to one site does not, because it belongs to that "
		      "site rather than to everyone");

		auto *copy = dr.findChild<QPushButton *>("rules_copy");
		check(copy != nullptr, "there is a way to get the flagged ones out");
		copy->click();
		const QString clip = QGuiApplication::clipboard()->text();
		check(clip.contains("Avvis"), "which yields the flagged rule");
		check(!clip.contains("weird-banner"),
		      "and only the flagged ones");
		check(clip.contains("builtin("),
		      "in the form defaults() is written in, so it can be pasted there");

		auto *rm = dr.findChild<QPushButton *>("rules_remove");
		view->setCurrentItem(view->topLevelItem(0));
		rm->click();
		check(view->topLevelItemCount() == 1, "a learned rule can be taken back");
		site_rules after;
		check(after.load(rpath), "and the file is rewritten at once");
	}

	section("kiosk, which had no way to be configured at all");
	{
		settings_dialog d3(&players, &downloads, nullptr, nullptr, nullptr, &policy);
		d3.show();
		auto *cats3 = d3.findChild<QListWidget *>("categories");
		bool has_kiosk = false;
		for (int i = 0; i < cats3->count(); ++i)
			if (cats3->item(i)->text() == "Kiosk") has_kiosk = true;
		check(has_kiosk, "it has a page now");

		auto *scale = d3.findChild<QComboBox *>("kiosk_scale");
		auto *esc   = d3.findChild<QCheckBox *>("kiosk_escape");
		check(scale && esc, "with the scaling path and the lockdown flag on it");
		check(esc && esc->isChecked(),
		      "and Esc leaves by default — the lockdown is opt-in, because "
		      "switching it on can leave no way out but killing the process");

		// Round trip through storage, since the point of the page is that the
		// value survives to the next run.
		scale->setCurrentIndex(scale->findData(int(scale_mode::geometric)));
		esc->setChecked(false);
		d3.accept();
		const kiosk_config saved = settings_store::kiosk();
		check(saved.scale == scale_mode::geometric, "the scaling path is saved");
		check(!saved.allow_escape, "and so is the lockdown flag");

		settings_dialog d4(&players, &downloads, nullptr, nullptr, nullptr, &policy);
		d4.show();
		auto *esc4 = d4.findChild<QCheckBox *>("kiosk_escape");
		check(esc4 && !esc4->isChecked(),
		      "and a freshly opened window shows what was stored, not the "
		      "compiled-in default");
		// Leave storage as found.
		esc4->setChecked(true);
		d4.findChild<QComboBox *>("kiosk_scale")
		    ->setCurrentIndex(0);
		d4.accept();
	}

	// A picture of every page, because this is a *layout* driver and layout is
	// the one thing assertions are bad at. A row can be present, correctly
	// parented and searchable while sitting on top of its own description.
	//
	// Not asserted on -- nothing here compares pixels, which would fail on a
	// different font before it ever caught a real regression. They are written
	// out so a change to these pages can be looked at.
	{
		section("pictures of each page");
		// A dialog of its own, with a couple of exceptions and a filter rule in
		// it: a screenshot of every list in its empty state shows the furniture
		// and none of the content, which is the half that is hard to get right.
		// Both schemes, because a dark palette is exactly the kind of change that
		// looks fine in a passing test and wrong on a screen.
		theme::apply(theme::choice::dark);
		policy_engine shot_policy;
		shot_policy.set_setting("news.example", policy::feature::javascript,
		                         policy::setting::block);
		shot_policy.set_setting("news.example", policy::feature::cookies,
		                         policy::setting::allow);
		shot_policy.set_setting("*.tracker.example", policy::feature::ads,
		                         policy::setting::block);
		settings_dialog shot(&players, &downloads, nullptr, nullptr, nullptr,
		                      &shot_policy);
		shot.resize(920, 740);
		shot.show();
		spin(200);
		auto *shot_cats = shot.findChild<QListWidget *>("categories");
		auto *shot_pages = shot.findChild<QStackedWidget *>("pages");
		const QString base = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
		                         ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
		                         : QString("/tmp/hydra-test");
		const QString shots = base + "/settings";
		QDir().mkpath(shots);
		int saved = 0, too_wide = 0;
		for (int i = 0; i < shot_cats->count(); ++i) {
			shot_cats->setCurrentRow(i);
			spin(250);
			QString name = shot_cats->item(i)->text();
			name.replace(" & ", "-").replace(' ', '-');
			if (shot.grab().save(QString("%1/%2.png").arg(shots, name.toLower())))
				++saved;
			// And the bottom of anything that scrolls. A picture of the top of a
			// long page shows the part that was already easy to get right; the
			// exceptions list sits below the fold on the privacy page and would
			// never have appeared in a review.
			if (auto *area = qobject_cast<QScrollArea *>(shot_pages->widget(i))) {
				QScrollBar *bar = area->verticalScrollBar();
				if (bar && bar->maximum() > 0) {
					bar->setValue(bar->maximum());
					spin(150);
					shot.grab().save(
					  QString("%1/%2-bottom.png").arg(shots, name.toLower()));
					bar->setValue(0);
				}
			}
			// No page may insist on being wider than the window it lives in.
			//
			// A settings page is text that wraps and controls that do not need
			// to grow, so a page that wants more width is always one widget
			// refusing to shrink -- and the symptom is a horizontal scrollbar
			// across a page that looks like it fits, or a control sitting under
			// the vertical scrollbar. Both happened here; neither was visible to
			// any other assertion.
			if (auto *area = qobject_cast<QScrollArea *>(shot_pages->widget(i))) {
				const int need = area->widget()->minimumSizeHint().width();
				const int have = area->viewport()->width();
				if (need > have) {
					++too_wide;
					std::printf("  --    %s needs %d px, has %d\n",
					            qPrintable(shot_cats->item(i)->text()), need, have);
					for (QWidget *w : area->widget()->findChildren<QWidget *>())
						if (w->minimumSizeHint().width() > have - 40)
							std::printf("        %s (%s) wants %d\n",
							            qPrintable(w->objectName().isEmpty()
							                           ? QString(w->metaObject()->className())
							                           : w->objectName()),
							            w->metaObject()->className(),
							            w->minimumSizeHint().width());
				}
			}
		}
		// And again in light, for comparison.
		theme::apply(theme::choice::light);
		spin(200);
		for (int i = 0; i < shot_cats->count(); ++i) {
			shot_cats->setCurrentRow(i);
			spin(180);
			QString name = shot_cats->item(i)->text();
			name.replace(" & ", "-").replace(' ', '-');
			shot.grab().save(QString("%1/%2-light.png").arg(shots, name.toLower()));
		}
		theme::apply(settings_store::appearance());

		check(saved == shot_cats->count(),
		      QString("every page was captured to %1 (%2)").arg(shots).arg(saved));
		check(too_wide == 0,
		      QString("and no page demands more width than the window gives it "
		               "(%1 that do)").arg(too_wide));
	}

	section("descriptions stay legible in both schemes");
	{
		// The bug this exists for: the descriptions were dimmed by computing a
		// lighter version of the text colour and writing it into the widget's
		// palette. That is a one-time snapshot -- the labels are built once, so
		// a colour worked out under a dark theme stayed put when the theme went
		// light, and the help text under every row went white on white. It
		// passed every assertion there was, and was obvious the moment anyone
		// looked at the screenshot.
		//
		// So: composite what each description would actually be painted in over
		// the window behind it, and insist the two are far enough apart to read.
		// Returns the worst gap and, just as importantly, how many labels it
		// found: a check that silently matches nothing is not a check. The old
		// buggy version dimmed by writing a colour rather than by role, so a
		// role-based search would have found zero labels and passed.
		auto worst_contrast = [](QWidget *w, int *seen) {
			int worst = 255;
			const QColor bg = w->palette().color(QPalette::Window);
			const auto labels = w->findChildren<QLabel *>("help");
			for (QLabel *l : labels) {
				// Whatever role it paints with, and whatever palette it is
				// carrying -- which is the point: a frozen palette is the bug.
				const QColor raw = l->palette().color(l->foregroundRole());
				// Alpha is how a light theme usually dims: blend it out by hand,
				// because a colour at 50% is not 50% of the way to unreadable.
				const qreal a = raw.alphaF();
				const QColor fg(qRound(raw.red()   * a + bg.red()   * (1 - a)),
				                 qRound(raw.green() * a + bg.green() * (1 - a)),
				                 qRound(raw.blue()  * a + bg.blue()  * (1 - a)));
				worst = std::min(worst, std::abs(fg.lightness() - bg.lightness()));
				++*seen;
			}
			return worst;
		};

		// Built under one scheme, then switched to the other -- which is the
		// sequence that breaks, and the one that happens for real: the dialog is
		// open, the user changes the colour scheme, and it previews live.
		// Building it fresh under each scheme hides the bug completely, because
		// a colour frozen under the theme that is still current looks fine.
		const theme::choice both[] = {theme::choice::dark, theme::choice::light};
		for (theme::choice built_under : both) {
			for (theme::choice then : both) {
				if (built_under == then)
					continue;
				theme::apply(built_under);
				policy_engine   probe_policy;
				settings_dialog probe(&players, &downloads, nullptr, nullptr,
				                       nullptr, &probe_policy);
				probe.show();
				spin(200);
				theme::apply(then);
				spin(200);

				auto *cats = probe.findChild<QListWidget *>("categories");
				int worst = 255, seen = 0;
				for (int i = 0; cats && i < cats->count(); ++i) {
					cats->setCurrentRow(i);
					spin(60);
					worst = std::min(worst, worst_contrast(&probe, &seen));
				}
				probe.close();
				check(seen > 20,
				      QString("there is help text to check at all (%1 labels)")
				          .arg(seen));
				check(worst >= 40,
				      QString("help text built %1 stays readable after switching "
				               "to %2 (worst gap %3 of 255)")
				          .arg(theme::name_of(built_under), theme::name_of(then))
				          .arg(worst));
			}
		}
		theme::apply(settings_store::appearance());
	}

	// The exceptions list: what the shield has been used to say, in one place.
	//
	// Before this there was nowhere to see a per-site rule. The page even said
	// "an exception always wins over what is chosen here" while offering no way
	// to find out whether you had any.
	section("site exceptions can be reviewed and undone");
	{
		policy_engine p;
		p.set_setting("news.example", policy::feature::javascript,
		               policy::setting::block);
		p.set_setting("news.example", policy::feature::cookies,
		               policy::setting::allow);
		p.set_setting("shop.example", policy::feature::images,
		               policy::setting::block);
		// A rule whose last feature was cleared: present in the engine, saying
		// nothing. Offering it for removal would be offering something that is
		// not there.
		p.set_setting("empty.example", policy::feature::popups,
		               policy::setting::block);
		p.set_setting("empty.example", policy::feature::popups,
		               policy::setting::unset);

		settings_dialog d(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d.show();
		auto *list = d.findChild<QTreeWidget *>("site_exceptions");
		auto *drop = d.findChild<QPushButton *>("drop_exception");
		check(list && drop, "the privacy page has an exceptions list");
		if (!list || !drop) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

		check(list->topLevelItemCount() == 2,
		      QString("both sites with something to say are listed (%1)")
		          .arg(list->topLevelItemCount()));
		QStringList sites;
		for (int i = 0; i < list->topLevelItemCount(); ++i)
			sites << list->topLevelItem(i)->text(0);
		check(!sites.contains("empty.example"),
		      "and a rule that expresses nothing is not offered as an exception");

		QString summary;
		for (int i = 0; i < list->topLevelItemCount(); ++i)
			if (list->topLevelItem(i)->text(0) == "news.example")
				summary = list->topLevelItem(i)->text(1);
		check(summary.contains("JavaScript: block") && summary.contains("Cookies: allow"),
		      QString("each row says what differs, not merely that something does (%1)")
		          .arg(summary));

		check(!drop->isEnabled(), "Remove is off until something is selected");
		list->topLevelItem(0)->setSelected(true);
		check(drop->isEnabled(), "and on once it is");

		const QString gone = list->topLevelItem(0)->text(0);
		drop->click();
		check(list->topLevelItemCount() == 1,
		      "removing takes the row out of the list at once");
		check(p.setting_for(gone, policy::feature::javascript) !=
		              policy::setting::unset ||
		          p.setting_for(gone, policy::feature::images) !=
		              policy::setting::unset,
		      "but the rule itself is untouched until OK — Cancel must mean it");

		d.accept();
		for (int i = 0; i < policy::feature_count(); ++i)
			if (p.setting_for(gone, static_cast<policy::feature>(i)) !=
			    policy::setting::unset) {
				check(false, QString("%1 still has a setting after OK").arg(gone));
				break;
			}
		check(p.effective_setting(policy::feature::javascript, gone) ==
		          p.global_default(policy::feature::javascript),
		      "and the site falls back to the defaults, which is what removal means");
	}

	section("restoring a page's defaults");
	{
		policy_engine p;
		p.set_setting("news.example", policy::feature::javascript,
		               policy::setting::block);

		settings_dialog d(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d.show();
		auto *cats2 = d.findChild<QListWidget *>("categories");
		auto *restore = d.findChild<QPushButton *>("restore_defaults");
		check(restore != nullptr, "there is a restore-defaults button");
		if (!restore || !cats2) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }

		// It names the page it acts on, which is what makes one button rather
		// than six unambiguous.
		cats2->setCurrentRow(0);
		check(restore->text().contains("Privacy"),
		      QString("and it names the page (%1)").arg(restore->text()));

		// Change something, restore, and check it went back.
		auto *js = d.findChild<QComboBox *>("feature_javascript");
		check(js != nullptr, "the privacy page has the JavaScript control");
		const int was = js->currentIndex();
		js->setCurrentIndex(was == 0 ? 1 : 0);
		check(js->currentIndex() != was, "changing it takes effect in the dialog");
		restore->click();
		check(js->currentIndex() == was,
		      "restoring puts it back to what a fresh install shows");

		// And the page's *own* rules are not what "defaults" means.
		auto *list = d.findChild<QTreeWidget *>("site_exceptions");
		check(list && list->topLevelItemCount() == 1,
		      "a site exception survives a defaults restore — it is a decision "
		      "about one site, not a default");

		// Nothing is written until OK, so Cancel is the undo. This is why the
		// button needs no confirmation dialog.
		js->setCurrentIndex(was == 0 ? 1 : 0);
		const policy::setting before =
		  p.global_default(policy::feature::javascript);
		restore->click();
		check(p.global_default(policy::feature::javascript) == before,
		      "restoring writes nothing on its own");
		d.reject();
		check(p.global_default(policy::feature::javascript) == before,
		      "and Cancel leaves the stored settings exactly as they were");
	}

	section("the filters page has no defaults to restore");
	{
		// It holds rules learned on this machine rather than preferences.
		// "Restore defaults" there would mean deleting them, which is not what
		// the button means anywhere else -- so it is off, and says why.
		policy_engine p;
		settings_dialog d(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d.show();
		auto *cats3 = d.findChild<QListWidget *>("categories");
		auto *restore = d.findChild<QPushButton *>("restore_defaults");
		int filters_row = -1;
		for (int i = 0; i < cats3->count(); ++i)
			if (cats3->item(i)->text() == "Filters")
				filters_row = i;
		check(filters_row >= 0, "there is a filters page");
		cats3->setCurrentRow(filters_row);
		check(!restore->isEnabled(), "restore is off there");
		check(restore->toolTip().contains("learned"),
		      "and the tooltip says why rather than leaving it mysterious");
		cats3->setCurrentRow(0);
		check(restore->isEnabled(), "and on again on a page that has defaults");
	}

	section("all the settings in one file, through the dialog");
	{
		// The bundle itself is covered by test_bundle; what is checked here is
		// the wiring: that the buttons exist, that exporting writes what the
		// *controls* say rather than what was stored when the window opened, and
		// that importing refills the window instead of leaving it showing the
		// old answer over the new settings.
		const QString base = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
		                         ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
		                         : QString("/tmp/hydra-test");
		const QString file = base + "/exported.ini";
		QFile::remove(file);

		policy_engine p;
		p.set_setting("news.example", policy::feature::javascript,
		               policy::setting::block);
		settings_dialog d(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d.show();
		check(d.findChild<QPushButton *>("settings_export") &&
		          d.findChild<QPushButton *>("settings_import"),
		      "the privacy page offers export and import");

		// Change a control without pressing OK, then export.
		auto *js = d.findChild<QComboBox *>("feature_javascript");
		const policy::setting stored = p.global_default(policy::feature::javascript);
		js->setCurrentIndex(stored == policy::setting::block ? 0 : 1);
		settings_bundle::write(file, &p, nullptr);   // before apply: the old value
		{
			QSettings f(file, QSettings::IniFormat);
			check(f.value("defaults/javascript").toString() ==
			          (stored == policy::setting::block ? "block" : "allow"),
			      "a file written before applying holds the stored value");
		}

		d.accept();   // apply() runs, so the engine now has the changed value
		settings_bundle::write(file, &p, nullptr);
		{
			QSettings f(file, QSettings::IniFormat);
			check(f.value("defaults/javascript").toString() !=
			          (stored == policy::setting::block ? "block" : "allow"),
			      "and after applying it holds what the control said — which is "
			      "why Export applies first");
		}
		QFile::remove(file);
	}

	section("the colour scheme, chosen and previewed");
	{
		// The decision logic has its own suite; what is checked here is the part
		// only the dialog can get wrong -- that choosing repaints at once, and
		// that Cancel puts back what was stored rather than leaving the window
		// in a theme nobody agreed to.
		settings_store::set_appearance(theme::choice::light);
		theme::apply(theme::choice::light);

		policy_engine p;
		settings_dialog d(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d.show();
		auto *pick = d.findChild<QComboBox *>("appearance");
		check(pick != nullptr, "there is a colour-scheme control");
		if (!pick) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
		check(pick->count() == 3, "with system, light and dark");
		check(pick->currentData().toInt() == int(theme::choice::light),
		      "showing what is stored");

		const int light_window =
		  QApplication::palette().color(QPalette::Window).lightness();
		pick->setCurrentIndex(pick->findData(int(theme::choice::dark)));
		check(QApplication::palette().color(QPalette::Window).lightness() <
		          light_window,
		      "choosing dark repaints immediately, so you can see what you chose");

		d.reject();
		check(QApplication::palette().color(QPalette::Window).lightness() ==
		          light_window,
		      "and Cancel puts the old scheme back");
		check(settings_store::appearance() == theme::choice::light,
		      "with nothing stored");

		settings_dialog d2(&players, &downloads, nullptr, nullptr, nullptr, &p);
		d2.show();
		auto *pick2 = d2.findChild<QComboBox *>("appearance");
		pick2->setCurrentIndex(pick2->findData(int(theme::choice::dark)));
		d2.accept();
		check(settings_store::appearance() == theme::choice::dark,
		      "while OK stores it");

		settings_store::set_appearance(theme::choice::system);
		theme::apply(theme::choice::system);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
