// All the settings in one file, and back again.
//
// The file is an INI on purpose: everything in it is a value or a list of flat
// records, so a key=value file a person can read and a tool can diff is worth
// more than the ability to nest. That choice is only worth anything if the file
// really does round-trip, which is what this checks — along with the refusals,
// since an import that quietly applies nothing looks exactly like one that
// worked.
#include "settings_bundle.h"
#include "filter_list.h"
#include "policy_engine.h"
#include "site_rules.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static QString slurp(const QString &path) {
	QFile f(path);
	return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString dir = QDir::tempPath() + "/hydra-bundle-test";
	QDir(dir).removeRecursively();
	QDir().mkpath(dir);
	// Never the real configuration.
	QSettings::setDefaultFormat(QSettings::IniFormat);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir);

	const QString path = dir + "/hydra-settings.ini";

	section("what an export looks like");
	{
		policy_engine p;
		p.set_global_default(policy::feature::javascript, policy::setting::allow);
		p.set_global_default(policy::feature::popups, policy::setting::block);
		p.set_setting("news.example", policy::feature::javascript,
		               policy::setting::block);
		p.set_setting("news.example", policy::feature::cookies,
		               policy::setting::allow);
		p.set_setting("*.tracker.example", policy::feature::ads,
		               policy::setting::block);

		filter_list fl;
		filter_rule r;
		filter_list::parse_rule("||ads.example^", &r);
		r.note = "leaked banner";
		fl.add(r);

		const settings_bundle::summary s = settings_bundle::write(path, &p, &fl);
		check(s.ok(), QString("it writes (%1)").arg(s.error));
		check(QFile::exists(path), "and the file is there");

		const QString text = slurp(path);
		// The point of choosing INI: someone can read this without the program.
		check(text.contains("[hydra]") && text.contains("format=1"),
		      "it says what it is and which format");
		check(text.contains("[defaults]") && text.contains("javascript=allow"),
		      "defaults are one plain line each");
		// Quoted, because a comma in an INI value means "list" and QSettings
		// quotes anything that would otherwise be read as one. Still a line a
		// person can read, which was the point; the quotes are the format being
		// correct rather than the format getting in the way.
		check(text.contains("news.example=\"javascript:block, cookies:allow\"") ||
		          text.contains("news.example=\"cookies:allow, javascript:block\""),
		      QString("a site exception is one readable line"));
		check(text.contains("%2A.tracker.example"),
		      "and a wildcard's * is escaped in the key, as an INI key must be");
		check(text.contains("ads.example"), "and the filter rules are in it");
		check(s.sites == 2 && s.filters == 1,
		      QString("the summary counts what went (%1)").arg(s.describe()));
	}

	section("and what comes back");
	{
		policy_engine fresh;
		filter_list fl;
		const settings_bundle::summary s = settings_bundle::read(path, &fresh, &fl);
		check(s.ok(), QString("it reads (%1)").arg(s.error));

		check(fresh.global_default(policy::feature::popups) == policy::setting::block,
		      "a global default comes back");
		check(fresh.setting_for("news.example", policy::feature::javascript) ==
		          policy::setting::block,
		      "a site exception comes back");
		check(fresh.setting_for("news.example", policy::feature::cookies) ==
		          policy::setting::allow,
		      "with each of its features, not just the first");
		check(fresh.setting_for("*.tracker.example", policy::feature::ads) ==
		          policy::setting::block,
		      "and a wildcard pattern survives being a key");
		check(fl.contains("||ads.example^"), "the filter rule comes back");
		check(fl.rules().first().note == "leaked banner",
		      "with the note that says why it exists");
	}

	section("reading it twice changes nothing the second time");
	{
		policy_engine p;
		filter_list fl;
		settings_bundle::read(path, &p, &fl);
		const int after_one = fl.rules().size();
		const settings_bundle::summary again = settings_bundle::read(path, &p, &fl);
		check(fl.rules().size() == after_one,
		      QString("no duplicate rules on a second read (%1)").arg(fl.rules().size()));
		check(again.filters == 0,
		      "and the summary says it added none rather than claiming it did");
	}

	section("a restore does not discard what happened since");
	{
		// Merging rather than replacing: someone who takes a backup, accepts a
		// new rule, then restores that backup should not silently lose the rule.
		policy_engine p;
		filter_list fl;
		filter_rule mine;
		filter_list::parse_rule("||later.example^", &mine);
		fl.add(mine);
		p.set_setting("mine.example", policy::feature::images, policy::setting::block);

		settings_bundle::read(path, &p, &fl);
		check(fl.contains("||later.example^"),
		      "a rule accepted after the backup survives the restore");
		check(p.setting_for("mine.example", policy::feature::images) ==
		          policy::setting::block,
		      "and so does an exception made after it");
		check(fl.contains("||ads.example^"), "while the backup's rules arrive");
	}

	section("what it refuses");
	{
		policy_engine p;
		filter_list fl;

		const settings_bundle::summary missing =
		  settings_bundle::read(dir + "/not-here.ini", &p, &fl);
		check(!missing.ok() && missing.error.contains("No such file"),
		      "a file that is not there");

		// Any INI at all would otherwise be accepted and apply nothing, which
		// looks exactly like a successful import of an empty backup.
		const QString foreign = dir + "/foreign.ini";
		{
			QSettings other(foreign, QSettings::IniFormat);
			other.setValue("something/else", 1);
			other.sync();
		}
		const settings_bundle::summary wrong = settings_bundle::read(foreign, &p, &fl);
		check(!wrong.ok() && wrong.error.contains("Hydra"),
		      QString("someone else's INI (%1)").arg(wrong.error));

		const QString future = dir + "/future.ini";
		{
			QSettings other(future, QSettings::IniFormat);
			other.setValue("hydra/format", settings_bundle::current_format() + 5);
			other.sync();
		}
		const settings_bundle::summary newer = settings_bundle::read(future, &p, &fl);
		check(!newer.ok() && newer.error.contains("newer version"),
		      QString("and a file from a later build, rather than half-applying it "
		               "(%1)").arg(newer.error));
	}

	section("a hand-edited file is still read");
	{
		// The reason for choosing a format people can edit is that they will.
		// Nonsense in one line must not cost the lines around it.
		const QString hand = dir + "/hand.ini";
		QFile f(hand);
		f.open(QIODevice::WriteOnly | QIODevice::Truncate);
		f.write("[hydra]\nformat=1\n\n"
		         "[defaults]\njavascript=block\nnosuchfeature=allow\ncookies=sideways\n\n"
		         "[sites]\ngood.example=images:block\nbad.example=nonsense\n"
		         "half.example=images:block, garbage, ads:allow\n");
		f.close();

		policy_engine p;
		filter_list fl;
		const settings_bundle::summary s = settings_bundle::read(hand, &p, &fl);
		check(s.ok(), "it reads");
		check(p.global_default(policy::feature::javascript) == policy::setting::block,
		      "the line that made sense was applied");
		check(p.setting_for("good.example", policy::feature::images) ==
		          policy::setting::block,
		      "and so was the site that made sense");
		check(p.setting_for("half.example", policy::feature::images) ==
		          policy::setting::block &&
		          p.setting_for("half.example", policy::feature::ads) ==
		              policy::setting::allow,
		      "a line with one bad field keeps its good ones");
		check(s.sites == 2,
		      QString("and the site with nothing usable is not counted (%1)")
		          .arg(s.sites));
	}

	section("the policy file is an INI, and reads the JSON it used to be");
	{
		const QString ini = dir + "/policy.ini";
		QFile::remove(ini);

		policy_engine p;
		p.set_global_default(policy::feature::popups, policy::setting::block);
		p.set_setting("news.example", policy::feature::javascript,
		               policy::setting::block);
		p.set_setting("*.ads.example", policy::feature::ads, policy::setting::block);
		check(p.save(ini), "it saves");

		const QString text = slurp(ini);
		check(text.contains("kind=policy"), "the file says what it is");
		check(text.contains("[defaults]") && text.contains("popups=block"),
		      "defaults are readable lines");
		check(text.contains("news.example"), "and so are the site rules");

		policy_engine back;
		check(back.load(ini), "it loads");
		check(back.global_default(policy::feature::popups) == policy::setting::block,
		      "the default comes back");
		check(back.setting_for("news.example", policy::feature::javascript) ==
		          policy::setting::block, "and the site rule");
		check(back.setting_for("*.ads.example", policy::feature::ads) ==
		          policy::setting::block, "wildcards included");

		// **The migration**, which is the part that could lose somebody's rules.
		// A policy.json written by an older build must still be read, once,
		// without anybody being told to convert anything.
		const QString legacy = dir + "/legacy-policy.json";
		{
			QFile f(legacy);
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write("{\n"
			         "  \"globalDefaults\": { \"images\": \"block\" },\n"
			         "  \"rules\": [ { \"pattern\": \"old.example\",\n"
			         "      \"settings\": { \"javascript\": \"block\" } } ]\n"
			         "}\n");
		}
		policy_engine migrated;
		check(migrated.load(legacy), "an old JSON policy file still loads");
		check(migrated.global_default(policy::feature::images) == policy::setting::block,
		      "with its defaults");
		check(migrated.setting_for("old.example", policy::feature::javascript) ==
		          policy::setting::block, "and its site rules");

		// And the same call given the *new* name finds the old file beside it,
		// which is what happens on the first run after an upgrade.
		const QString renamed = dir + "/legacy-policy.ini";
		QFile::remove(renamed);
		policy_engine after_upgrade;
		check(after_upgrade.load(renamed),
		      "asking for the .ini finds the .json left next to it");
		check(after_upgrade.setting_for("old.example", policy::feature::javascript) ==
		          policy::setting::block,
		      "and nothing is lost by the rename");
		check(after_upgrade.save(renamed) && slurp(renamed).contains("kind=policy"),
		      "the next save writes the new format");
	}

	section("hand-editing the policy file works, which is why it is an INI");
	{
		const QString hand = dir + "/hand-policy.ini";
		QFile f(hand);
		f.open(QIODevice::WriteOnly | QIODevice::Truncate);
		// Unquoted commas, as a person would write them.
		f.write("[hydra]\nformat=1\nkind=policy\n\n"
		         "[defaults]\njavascript=block\n\n"
		         "[sites]\nmine.example=images:block, ads:allow\n");
		f.close();

		policy_engine p;
		check(p.load(hand), "a file written by hand loads");
		check(p.global_default(policy::feature::javascript) == policy::setting::block,
		      "its default is applied");
		check(p.setting_for("mine.example", policy::feature::images) ==
		              policy::setting::block &&
		          p.setting_for("mine.example", policy::feature::ads) ==
		              policy::setting::allow,
		      "and both halves of an unquoted line are read");
	}

	section("the site-rules file is an INI too");
	{
		const QString ini = dir + "/site-rules.ini";
		QFile::remove(ini);

		site_rules r = site_rules::defaults();
		const int builtins = r.all().size();
		site_rule learned;
		learned.kind  = "container";
		learned.value = "#cookie-wall";
		learned.host  = "news.example";
		learned.note  = "seen here";
		r.add(learned);
		check(r.save(ini), "it saves");

		const QString text = slurp(ini);
		check(text.contains("kind=siteRules"), "the file says what it is");
		check(text.contains("cookie-wall"), "and the learned rule is in it");
		check(!text.contains("size=0"), "with something in the array");

		site_rules back;
		check(back.load(ini), "it loads");
		check(back.all().size() == builtins + 1,
		      QString("the built-ins are still there and the learned one arrives "
		               "(%1, wanted %2)").arg(back.all().size()).arg(builtins + 1));
		bool found = false;
		for (const site_rule &x : back.all())
			if (x.value == "#cookie-wall" && x.host == "news.example" &&
			    x.note == "seen here" && !x.builtin)
				found = true;
		check(found, "with its host and note, and not marked built-in");

		// A built-in is not written to the file -- it comes from the binary, and
		// a copy would be a stale duplicate the day one changes.
		check(!text.contains("builtin=true"), "built-ins are not written out");

		// The migration, again the part that matters.
		const QString legacy = dir + "/legacy-rules.json";
		{
			QFile f(legacy);
			f.open(QIODevice::WriteOnly | QIODevice::Truncate);
			f.write("{\"version\":1,\"rules\":[{\"kind\":\"detector\","
			         "\"value\":\"adblockDetector\",\"note\":\"old file\"}]}");
		}
		site_rules migrated;
		check(migrated.load(legacy), "an old JSON rules file still loads");
		check(migrated.detectors().contains("adblockDetector"),
		      "and its rules are there");

		const QString renamed = dir + "/legacy-rules.ini";
		QFile::remove(renamed);
		site_rules after_upgrade;
		check(after_upgrade.load(renamed),
		      "asking for the .ini finds the .json beside it");
		check(after_upgrade.detectors().contains("adblockDetector"),
		      "so an upgrade costs nothing");
	}

	if (!qEnvironmentVariableIsSet("HYDRA_KEEP_BUNDLE"))
		QDir(dir).removeRecursively();
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
