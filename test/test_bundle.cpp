// All the settings in one file, and back again.
//
// The file is an INI on purpose: everything in it is a value or a list of flat
// records, so a key=value file a person can read and a tool can diff is worth
// more than the ability to nest. That choice is only worth anything if the file
// really does round-trip, which is what this checks -- along with the refusals,
// since an import that quietly applies nothing looks exactly like one that
// worked.
#include "settings_bundle.h"
#include "cosmetic_filters.h"
#include "filter_list.h"
#include "policy_engine.h"
#include "site_rules.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
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

	section("a hand-edited preference with a comma is not emptied");
	{
		// **The `[sites]` group has read both spellings for a while; the
		// `[preferences]` group did not.** A comma in an INI value means
		// "list" to QSettings. It quotes what the bundle writes, so an export
		// and import round-trips -- and a person editing the file by hand
		// will not quote, so `value()` hands back a `QStringList`. Stored as
		// one, `toString()` on more than one element is **empty**: the
		// preference is lost rather than mistyped.
		//
		// `torrent/listen_interfaces` is why this is concrete. libtorrent's
		// format for it is comma-separated, so a hand-edited bundle carrying
		// one empties the setting it was meant to carry.
		QTemporaryDir scratch;
		check(scratch.isValid(), "there is a scratch directory");
		const QString hand = scratch.path() + "/by-hand.ini";
		{
			QFile f(hand);
			check(f.open(QIODevice::WriteOnly | QIODevice::Text),
			      "a bundle is written by hand");
			QTextStream out(&f);
			out << "[hydra]\nformat=1\nkind=settings\n\n"
			     << "[preferences]\n"
			     // Unquoted on purpose: this is what a person types.
			     << "torrent/listen_interfaces=0.0.0.0:6881,[::]:6881\n"
			     << "downloads/directory=/home/someone/Films, TV\n";
		}

		policy_engine pe;
		filter_list fl;
		const settings_bundle::summary s =
		  settings_bundle::read(hand, &pe, &fl);
		check(s.ok(), QString("it reads (%1)").arg(s.error));

		QSettings app(QSettings::IniFormat, QSettings::UserScope,
		               "hydra", "hydra");
		check(app.value("torrent/listen_interfaces").toString() ==
		          "0.0.0.0:6881,[::]:6881",
		      QString("and the comma-separated interfaces come back whole "
		               "(%1)")
		          .arg(app.value("torrent/listen_interfaces").toString()));
		// **And the limit, which the rejoin cannot lift.** QSettings trims
		// whitespace around the separator when it parses a list, so
		// `Films, TV` arrives as `["...Films", "TV"]` and the space is gone
		// before anything here sees it. Rejoining gives `Films,TV` -- the
		// structure back, not the bytes.
		//
		// Still better than what it replaced: as a stored list, `toString()`
		// on two elements is empty, so the preference was lost outright. A
		// path that is wrong by one space is at least visible. What a person
		// editing a bundle by hand should do is quote a value containing a
		// comma, which is what the file's own writer does.
		check(app.value("downloads/directory").toString() ==
		          "/home/someone/Films,TV",
		      QString("while a comma followed by a space cannot be put back, "
		               "the space being gone before this is reached (%1)")
		          .arg(app.value("downloads/directory").toString()));

		// The quoted spelling, which is what an export writes and what the
		// advice above amounts to: that one is exact.
		const QString quoted = scratch.path() + "/quoted.ini";
		{
			QFile f(quoted);
			check(f.open(QIODevice::WriteOnly | QIODevice::Text),
			      "a bundle written with the value quoted");
			QTextStream out(&f);
			out << "[hydra]\nformat=1\nkind=settings\n\n"
			     << "[preferences]\n"
			     << "downloads/directory=\"/home/someone/Films, TV\"\n";
		}
		policy_engine pe2;
		filter_list fl2;
		check(settings_bundle::read(quoted, &pe2, &fl2).ok(), "reads");
		check(app.value("downloads/directory").toString() ==
		          "/home/someone/Films, TV",
		      QString("comes back exactly, space and all (%1)")
		          .arg(app.value("downloads/directory").toString()));
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

		// **A damaged file, and the message is the whole point of the check.**
		// Refusing it was never in doubt: an unparseable INI answers every
		// `value()` with the default, so the format marker reads 0 and the
		// unmarked-file arm turns it away. What that hides is that the status
		// check above it is inert -- `QSettings::status()` is lazy and answers
		// NoError until something forces the parse, so asked before the first
		// access it has never fired and could not.
		//
		// `annoyance_log`, `policy_engine` and `site_rules` each carry that
		// lesson in a comment, having each been written the wrong way round
		// first. This is the fourth site and the only one where the wrong
		// answer is merely a wrong sentence rather than a lost file -- so the
		// assertion is on which sentence, because "it was refused" cannot tell
		// a working check from one that has never run.
		// **The damage is `test_annoyance`'s, taken rather than invented.**
		// That suite measured six inputs to find one QSettings actually
		// refuses: binary NULs are tolerated and report NoError, while a plain
		// line of prose gives FormatError. The first draft here was three
		// lines of prose and QSettings accepted it -- one of the lines carried
		// an `=`, which is enough for its parser to recover -- so this test
		// passed a damaged file and would have reported the fix broken.
		const QString damaged = dir + "/damaged.ini";
		{
			QFile bad(damaged);
			bad.open(QIODevice::WriteOnly | QIODevice::Truncate);
			bad.write("this is not an ini file at all\n");
		}
		const settings_bundle::summary broken =
		  settings_bundle::read(damaged, &p, &fl);
		check(!broken.ok(), "a damaged file is refused");
		check(broken.error.contains("not readable"),
		      QString("and told apart from someone else's INI, which is what "
		               "says the status check ran at all (%1)").arg(broken.error));
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
	section("a rule dropped from the policy does not come back on the next save");
	{
		// **This is what `save()` clearing the object is for, and nothing
		// tested it.** The writer used to `QFile::remove` the file before
		// asking QSettings to write it, which dropped keys the previous save
		// had made -- a rule the user deleted -- because `setValue` alone only
		// adds and overwrites. Removing the file first also left a window with
		// no policy file at all, which is why it became `clear()` instead.
		//
		// Swapping the mechanism kept the hole shut and could have lost the
		// behaviour the hole was paying for. A deleted rule silently returning
		// on the next launch is the failure that would follow, so it is
		// checked rather than assumed.
		const QString path = dir + "/drop-policy.ini";
		QFile::remove(path);

		policy_engine first;
		first.set_setting("keep.example", policy::feature::javascript,
		                   policy::setting::block);
		first.set_setting("gone.example", policy::feature::popups,
		                   policy::setting::block);
		check(first.save(path), "a policy with two site rules saves");

		policy_engine reread;
		check(reread.load(path), "and loads again");
		check(reread.setting_for("gone.example", policy::feature::popups)
		        == policy::setting::block,
		      "with the rule that is about to be dropped still in it");

		// Drop one by writing a policy that never had it.
		policy_engine second;
		second.set_setting("keep.example", policy::feature::javascript,
		                    policy::setting::block);
		check(second.save(path), "a policy without it saves over the first");

		policy_engine after;
		check(after.load(path), "and loads");
		check(after.setting_for("keep.example", policy::feature::javascript)
		        == policy::setting::block,
		      "the rule that stayed is still there");
		check(after.setting_for("gone.example", policy::feature::popups)
		        != policy::setting::block,
		      "and the dropped rule did not survive the write");

		// The file is never absent: it exists before and after, which is the
		// property `QFile::remove` gave up and `clear()` keeps.
		check(QFileInfo::exists(path),
		      "and the policy file exists throughout, never removed to be rewritten");
	}

	section("a cosmetic selector cannot say anything but which element");
	{
		// The selectors end up in a stylesheet on the page. One carrying `}`
		// closes the rule it was placed in and everything after it becomes CSS
		// of its own choosing: an `@import` from a remote host, a full-page
		// overlay, a `background: url(...)` that sends somewhere what an
		// attribute selector matched. Not code execution, and a great deal more
		// than hiding an element.
		//
		// Unreachable today -- the only source is the evolution loop, one rule
		// at a time, accepted by a person. It stops being unreachable the
		// moment rules arrive from anywhere else.
		check(filter_list::why_selector_unsafe(".ad-banner").isEmpty(),
		       "an ordinary selector is a selector");
		check(filter_list::why_selector_unsafe(
		         "#a > .b:not(.c)[data-x=\"y\"]").isEmpty(),
		       "and so is a complicated one");
		check(!filter_list::why_selector_unsafe(
		          "x{}*{background:url(https://evil.invalid/)}").isEmpty(),
		       "one that closes the rule and opens another is not");
		check(!filter_list::why_selector_unsafe("x/*").isEmpty(),
		       "nor one that comments out the brace that would have stopped it");
		check(!filter_list::why_selector_unsafe("@import url(x)").isEmpty(),
		       "nor an at-rule");

		// The accept-time gate and the delivery-time one, because a rule can
		// reach the file without passing the first.
		filter_list fl;
		filter_rule r;
		check(filter_list::parse_rule(
		         "evil.example##x{}*{background:url(https://evil.invalid/)}", &r),
		       "the hostile rule parses, which is why a syntax check is not one");
		const dry_run v = filter_list::evaluate(r, {}, "other.example");
		check(v.rejected, "and the review refuses it");
		check(v.reason.contains("selector"),
		       QString("saying what is wrong (%1)").arg(v.reason));

		fl.add(r);
		const QStringList shipped =
		  cosmetic_filters::selectors_for(&fl, "evil.example");
		check(shipped.isEmpty(),
		       "and if it is in the file anyway, nothing ships it to the page");
	}

	section("a scriptlet in the person's own list is a third kind of rule");
	{
		// **`##` is all a scriptlet rule and a cosmetic rule share.** Before
		// `filter_rule::scriptlet` existed, `parse_rule` saw the separator and
		// called this cosmetic with a selector of `+js(...)` -- which the CSS
		// parser refuses, so the rule sat in the list looking accepted and did
		// nothing whatever. Silently inert is the failure this section is
		// written against, and it is worse than a refusal because nothing
		// says so.
		filter_rule r;
		check(filter_list::parse_rule(
		         "youtube.com##+js(json-prune, adPlacements playerAds)", &r),
		       "a scriptlet rule parses");
		check(r.scriptlet && !r.cosmetic,
		       QString("as a scriptlet and not as a cosmetic rule (%1/%2)")
		           .arg(r.scriptlet).arg(r.cosmetic));
		check(r.scope == "youtube.com",
		       QString("scoped to the site it names (%1)").arg(r.scope));
		const dry_run ok = filter_list::evaluate(r, {}, "youtube.com");
		check(!ok.rejected,
		       QString("the review accepts it (%1)").arg(ok.reason));
		// **An empty dry run is the honest answer here, not a finding.** A
		// scriptlet matches no URL and hides no element, so there is nothing
		// to simulate -- and `would_block` being empty must not be read as
		// "this rule does nothing".
		check(ok.would_block.isEmpty() && !ok.cosmetic_checked,
		       "with nothing to simulate, because it matches no request");

		// Delivery-time, both halves, because a rule can reach the file
		// without passing the review.
		filter_list fl;
		fl.add(r);
		check(cosmetic_filters::selectors_for(&fl, "youtube.com").isEmpty(),
		       "nothing ships it to the page as a selector");
		// **The fixture is the rule's own text as a URL, which is contrived
		// on purpose: it is the only input that separates the two
		// implementations.** Without the flag this line became a substring
		// network rule whose needle was the whole of it, so a URL containing
		// that text matched. A realistic URL could not tell the two apart --
		// the needle has spaces and parentheses in it and would never fire --
		// which would make a plausible-looking assertion pass either way.
		check(!fl.blocks("https://x.test/youtube.com##+js(json-prune, "
		                  "adPlacements playerAds)", "x.test"),
		       "and it is not in the network index as a substring needle");
		check(!fl.blocks("https://youtube.com/watch?v=aaaaaaaaaaa",
		                  "youtube.com"),
		       "nor does it block the page it patches");

		// The three refusals, each with its own reason.
		filter_rule bare;
		check(filter_list::parse_rule("##+js(json-prune, x)", &bare),
		       "an unscoped scriptlet parses");
		const dry_run no_scope = filter_list::evaluate(bare, {}, "x.test");
		check(no_scope.rejected && no_scope.reason.contains("every page"),
		       QString("and is refused for patching every page (%1)")
		           .arg(no_scope.reason));

		filter_rule unknown;
		check(filter_list::parse_rule("x.test##+js(eval-this, window.X=1)",
		                               &unknown),
		       "a scriptlet outside the catalog parses");
		const dry_run not_ours = filter_list::evaluate(unknown, {}, "x.test");
		check(not_ours.rejected,
		       QString("and is refused (%1)").arg(not_ours.reason));

		// **The judgement, asserted so it is not quietly widened.** Trust is
		// a statement about a publisher and the settings page asks it per
		// subscription. This list has no publisher: it holds what a person
		// typed and what they accepted from the model, and once written
		// nothing in the file tells the two apart.
		filter_rule powerful;
		check(filter_list::parse_rule(
		         "x.test##+js(trusted-set-cookie, consent, yes)", &powerful),
		       "a trusted scriptlet parses here too");
		const dry_run refused = filter_list::evaluate(powerful, {}, "x.test");
		check(refused.rejected,
		       QString("and is refused in this list (%1)").arg(refused.reason));
		check(refused.reason.contains("trusted"),
		       QString("saying where it would have to come from (%1)")
		           .arg(refused.reason));

		// The shared pattern refusal reaches here as well, by the same call.
		filter_rule slow;
		check(filter_list::parse_rule("x.test##+js(nostif, /^(a+)+$/)", &slow),
		       "a backtracking argument parses");
		check(filter_list::evaluate(slow, {}, "x.test").rejected,
		       "and is refused by the rule site_rules already had");
	}

	section("a real list has to be answerable per request");
	{
		// `blocks()` was a linear scan calling `matches()` on every rule, and
		// `matches()` built a whole `QUrl` for each host-anchored one. On the
		// handful of rules the evolution loop produces that is free; on a
		// subscription it is a URL parsed tens of thousands of times per
		// request. This is the measurement that decides whether a real list can
		// be loaded at all.
		filter_list fl;
		const int hosts = 20000, subs = 5000;
		for (int i = 0; i < hosts; ++i) {
			filter_rule r;
			filter_list::parse_rule(QString("||ads%1.example^").arg(i), &r);
			fl.add(r);
		}
		for (int i = 0; i < subs; ++i) {
			filter_rule r;
			filter_list::parse_rule(QString("/banner%1/track").arg(i), &r);
			fl.add(r);
		}
		check(fl.rules().size() == hosts + subs,
		       QString("%1 rules loaded").arg(fl.rules().size()));

		// **Forty, not two hundred.** The oracle below is the algorithm this
		// replaced, and it costs 30 ms per url -- the whole point of the
		// measurement -- so checking parity on two hundred of them added six
		// seconds to every run of the suite to prove nothing the fortieth had
		// not already proved.
		QStringList urls;
		for (int i = 0; i < 40; ++i)
			urls << QString("https://cdn%1.site.example/a/b/c?q=%2").arg(i).arg(i);
		urls << "https://ads17.example/x" << "https://x.example/banner42/track";

		// **The control is the old algorithm, run here.** A time on its own
		// says nothing, and a fast wrong answer is not an improvement -- so the
		// naive scan is kept as the oracle and every URL has to agree.
		auto naive = [&](const QString &u) {
			for (const filter_rule &r : fl.rules()) {
				if (r.cosmetic)
					continue;
				if (filter_list::matches(r.text, u))
					return true;
			}
			return false;
		};

		int disagreements = 0, blocked = 0;
		for (const QString &u : urls) {
			const bool a = fl.blocks(u, QString());
			const bool b = naive(u);
			if (a != b) ++disagreements;
			if (a) ++blocked;
		}
		check(disagreements == 0,
		       QString("the index agrees with the scan on every url (%1 blocked)")
		           .arg(blocked));
		check(blocked == 2, "and the two that should block, do");


		QElapsedTimer t;
		t.start();
		for (const QString &u : urls)
			fl.blocks(u, QString());
		const qint64 fast_us = t.nsecsElapsed() / 1000;
		// Timed over a handful, because each one is 30 ms.
		t.restart();
		int timed = 0;
		for (const QString &u : urls) {
			if (timed++ >= 5) break;
			naive(u);
		}
		const qint64 slow_us = t.nsecsElapsed() / 1000 / qMax(1, timed);

		std::printf("        %d rules: per request indexed %.1f us, "
		             "scan %lld us\n", hosts + subs,
		             double(fast_us) / urls.size(), (long long)slow_us);

		// **Wildcards, which this fixture had none of.** Every rule above is
		// host-anchored or a plain substring whose token happens to be a
		// whole run in the URL, so two of the index's three buckets were
		// compared and the `wildcard` one -- the only kind with matching
		// logic of its own -- was absent. Putting six in found a real miss at
		// once: `*banner*.gif` was filed under `banner` while
		// `other.example/a/banner9.gif` offers the token `banner9`, so the
		// rule never fired. See `token_of` for the rule that fixed it.
		{
			const QStringList wild = {
				"ads*.example/banner*", "*/track/*", "||cdn*.site.example^",
				"*banner*.gif", "promo*pixel*", "||x*.test^",
			};
			for (const QString &w : wild) {
				filter_rule r;
				if (filter_list::parse_rule(w, &r))
					fl.add(r);
			}
			const QStringList probes = {
				"https://ads9.example/banner/x.png",
				"https://cdn7.site.example/a",
				"https://other.example/track/me",
				"https://other.example/promo/1/pixel/2",
				"https://other.example/a/banner9.gif",
				"https://xq.test/y",
				"https://nothing.example/plain",
				"https://ads.example/bannerless",
			};
			QStringList disagree;
			for (const QString &u : probes)
				if (fl.blocks(u, QString()) != naive(u))
					disagree << QString("%1 (index says %2)")
					                .arg(u).arg(fl.blocks(u, QString()));
			check(disagree.isEmpty(),
			      QString("the index agrees with the scan on wildcards too "
			               "(%1)")
			          .arg(disagree.isEmpty() ? QStringLiteral("all eight")
			                                   : disagree.join(", ")));

			// **Which of them fire, printed once and recorded, because
			// parity alone does not say whether a rule works.** Both sides
			// agreeing that nothing matched is parity too. Measured:
			//
			//   ads9.example/banner/x.png        blocked by ads*.example/banner*
			//   other.example/track/me           blocked by */track/*
			//   other.example/promo/1/pixel/2    blocked by promo*pixel*
			//   other.example/a/banner9.gif      blocked by *banner*.gif
			//   cdn7.site.example/a              NOT blocked, by either
			//   xq.test/y                        NOT blocked, by either
			//
			// The last two are `||cdn*.site.example^` and `||x*.test^`: a
			// host-anchored rule containing a `*` is filed under the literal
			// host `cdn*.site.example` and looked up by exact host, so it
			// never fires -- and `matches()` does not expand it either, so
			// **the scan agrees and a differential check cannot see it.**
			// Recorded rather than fixed: host rules are the large majority
			// and making their bucket handle a wildcard is a change to the
			// shape of the index, not a rule about tokens.
			check(!fl.blocks("https://cdn7.site.example/a", QString()),
			      "while a host rule with a star in it fires for neither, "
			      "which is its own gap");
		}

		// **What the fix costs, since the index's speed is the reason a real
		// list can be loaded at all.** A rule whose every run touches a `*`
		// or an end cannot be indexed and goes to the always-tested list, so
		// the price is linear in how many of those a list has. Measured here
		// rather than argued, because nothing in this tree knows EasyList's
		// distribution.
		{
			const int unpinned = 2000;
			for (int i = 0; i < unpinned; ++i) {
				filter_rule r;
				if (filter_list::parse_rule(QString("*unpinned%1*").arg(i), &r))
					fl.add(r);
			}
			QElapsedTimer u;
			u.start();
			for (const QString &url : urls)
				fl.blocks(url, QString());
			const double per = double(u.nsecsElapsed()) / 1000.0 / urls.size();
			std::printf("        plus %d unpinned rules: %.1f us per request\n",
			             unpinned, per);
			// A ceiling rather than a target: the point is that it stays in
			// microseconds, three orders off the scan, not that it is any
			// particular number on this machine.
			check(per < 500.0,
			      QString("which stays in microseconds (%.1f us)").arg(per));
		}

		// **A ratio after all, and the premise this used to carry was
		// backwards.** It said an absolute budget was the stable measure
		// because "a ratio would vary with the machine". Measured on one
		// machine minutes apart, under thirteen concurrent compilers and then
		// idle:
		//
		//     loaded   indexed 578.3 us   scan 258172 us
		//     idle     indexed   6.3 us   scan 250990 us
		//
		// The *scan* barely moved and the indexed figure moved ninety-fold,
		// because a few microseconds is all scheduling noise and a quarter of
		// a second averages it out. So the absolute is the load-sensitive
		// half, and it failed this suite for the machine rather than for the
		// code -- in a tree where more than one session builds at once, which
		// is not a rare condition.
		//
		// What the feature actually claims is that the index answers instead
		// of the scan, and 50x is far under the 446x the loaded run still
		// managed while asserting nothing about the microseconds. The two
		// absolutes are printed above and stay worth reading.
		const double ratio = slow_us / qMax(0.001, double(fast_us) / urls.size());
		check(ratio > 50.0,
		       QString("the index answers rather than the scan (%1x)")
		           .arg(ratio, 0, 'f', 0));
		// Printed, not checked. A `check(true, ...)` would be a line in the
		// output that cannot fail, which is the thing this suite exists to
		// refuse.
		std::printf("        a request is decided in %.1f us\n",
		             double(fast_us) / urls.size());
	}

	section("a save drops what an older format left behind");

	// **What `clear()` is for, pinned so a rewrite cannot lose it.**
	// `setValue` alone adds and overwrites and never removes, so a save that
	// did nothing else would leave a rule the person deleted sitting in the
	// file. Both this store and `policy_engine` drop the old keys first; the
	// difference, and the reason this one changed, is HOW: it used
	// `QFile::remove`, which unlinks the file immediately and leaves the
	// machine with no consent rules at all until `sync()` -- a process killed
	// in that window comes back having forgotten every banner ever dismissed.
	//
	// **This assertion does not discriminate that window**, and saying so
	// matters: it passes with either implementation. The argument for the
	// change is `policy_engine::save`'s own recorded reasoning in the same
	// tree, not this check. What this check defends is the erasure itself,
	// which the change must not lose while fixing the hole.
	{
		const QString dir = QDir::temp().filePath("hydra-rules-clear");
		QDir(dir).removeRecursively();
		QDir().mkpath(dir);
		const QString path = dir + "/site-rules.ini";

		site_rules first;
		site_rule a; a.kind = "reject"; a.value = "#cookie-wall"; a.host = "a.test";
		site_rule b; b.kind = "accept"; b.value = "#ok";          b.host = "b.test";
		first.add(a); first.add(b);
		check(first.save(path), "two rules saved");

		site_rules second;
		second.add(a);
		check(second.save(path), "and then one rule saved over them");

		site_rules back;
		check(back.load(path), "the file still loads");

		// **Counted by host on `all()`, not through `for_host`.** The first
		// version of this asked `for_host("a.test").all().size() == 1` and got
		// 15, because `load` starts from `site_rules::defaults()` -- built-ins
		// are not in the file and must not be dropped by reading one -- and
		// `for_host` includes every generic rule as well as the host's own.
		// The premise was wrong, not the store; the numbers said so by being
		// 15 and 14 rather than 1 and 0, one apart, which is the shape of "the
		// same shared set plus one".
		auto for_exactly = [&back](const QString &host) {
			int n = 0;
			for (const site_rule &r : back.all())
				if (r.host.compare(host, Qt::CaseInsensitive) == 0)
					++n;
			return n;
		};
		check(for_exactly("a.test") == 1,
		       QString("the rule that stayed is there (%1)")
		           .arg(for_exactly("a.test")));
		check(for_exactly("b.test") == 0,
		       QString("and the one dropped is gone rather than left behind "
		                "(%1)").arg(for_exactly("b.test")));

		// **And the half that can actually fail.** The two checks above pass
		// with the erasure removed entirely -- sabotaged and measured, 85
		// passed either way -- because `beginWriteArray` drops the previous
		// array itself, so nothing in `rules` depends on `clear()`. A check
		// that cannot fail is not a check, and the honest question is what
		// `clear()` is FOR: keys outside the array, which `setValue` alone
		// would leave sitting there. A file this store used to write in
		// another shape is exactly that.
		{
			{
				QSettings stale(path, QSettings::IniFormat);
				stale.setValue("legacy/leftover", "from an older format");
				stale.sync();
			}
			site_rules third;
			third.add(a);
			check(third.save(path), "saved again over a file with a stray key");

			QSettings after(path, QSettings::IniFormat);
			after.allKeys();
			check(!after.contains("legacy/leftover"),
			       QString("the stray key is gone (%1)")
			           .arg(after.value("legacy/leftover").toString()));
			check(after.value("hydra/kind").toString() == "siteRules",
			       "and the file is still this store's own");
		}

		// **And a file from a newer build, which `clear()` makes dangerous.**
		// The stray-key check above is the good half of `clear()`: it drops
		// what an older shape left behind. The other half is that it drops
		// what a *newer* shape put there deliberately -- and `hydra/format`
		// was written by `save()` and read by nobody, so a file carrying rules
		// this build cannot represent was read in part and rewritten without
		// the rest. `settings_bundle` has refused a newer format all along;
		// this store did not.
		{
			const QString future = dir + "/future.ini";
			{
				QSettings f(future, QSettings::IniFormat);
				f.setValue("hydra/format", 99);
				f.setValue("hydra/kind", "siteRules");
				f.sync();
			}
			site_rules newer;
			check(!newer.load(future),
			       "a consent-rules file from a newer build is refused");

			// The control: the same file at this build's format is read, so the
			// refusal is about the number rather than about the file.
			const QString same = dir + "/same.ini";
			{
				QSettings f(same, QSettings::IniFormat);
				f.setValue("hydra/format", 1);
				f.setValue("hydra/kind", "siteRules");
				f.sync();
			}
			site_rules present;
			check(present.load(same),
			       "and the same file at this build's format is read");
		}
		QDir(dir).removeRecursively();
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
