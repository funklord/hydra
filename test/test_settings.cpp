// Settings persistence and the custom-player command template.
#include "settings_dialog.h"
#include "request_filter.h"
#include "scheme_rules.h"
#include "filter_list.h"
#include "cosmetic_filters.h"
#include "policy_engine.h"
#include "download_manager.h"
#include "player_launcher.h"
#include "media_detector.h"
#include "accept_language.h"
#include "user_agent.h"
#include "torrent_download_source.h"
#include "ollama_provider.h"
#include "claude_provider.h"
#include "ai_provider.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
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
	QCoreApplication app(argc, argv);

	// Never touch the real user config.
	const QString tmp = QDir::temp().filePath("hydra-settings-test");
	QDir(tmp).removeRecursively();
	QDir().mkpath(tmp);
	QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp);

	section("defaults when nothing has been saved");
	{
		download_manager m;
		auto *tor = new torrent_download_source;
		m.add_source(tor);
		player_launcher p;
		settings_store::load_into(&p, &m, tor, nullptr, nullptr);
		check(tor->connection_limit_global() == 800,
		      QString("global cap defaults high (%1)").arg(tor->connection_limit_global()));
		check(tor->connection_limit_per_torrent() == 200,
		      QString("per-torrent cap defaults high (%1)")
		          .arg(tor->connection_limit_per_torrent()));
		check(qFuzzyCompare(tor->seed_ratio(), 1.0), "seed ratio defaults to 1.0");
		check(!p.selected().isEmpty() || p.installed().isEmpty(),
		      "a player is resolved from what is installed");
	}

	section("round trip");
	{
		download_manager m;
		auto *tor = new torrent_download_source;
		m.add_source(tor);
		player_launcher p;

		m.set_directory(tmp + "/dl");
		tor->set_connection_limits(3000, 450);
		tor->set_seed_ratio(2.5);
		tor->set_listen_interfaces("tun0:6881");
		tor->set_sequential(true);
		p.set_custom_command("/bin/echo --play %U");
		p.set_selected(player_launcher::custom_id());
		settings_store::save_from(&p, &m, tor, nullptr, nullptr);
	}
	{
		download_manager m2;
		auto *tor2 = new torrent_download_source;
		m2.add_source(tor2);
		player_launcher p2;
		settings_store::load_into(&p2, &m2, tor2, nullptr, nullptr);

		check(m2.directory() == tmp + "/dl", "download directory persists");
		check(tor2->connection_limit_global() == 3000,
		      QString("global cap persists (%1)").arg(tor2->connection_limit_global()));
		check(tor2->connection_limit_per_torrent() == 450, "per-torrent cap persists");
		check(qFuzzyCompare(tor2->seed_ratio(), 2.5), "seed ratio persists");
		check(tor2->listen_interfaces() == "tun0:6881",
		      "listen interfaces persists — the system-VPN binding");
		check(tor2->sequential(), "sequential default persists");
		check(p2.custom_command() == "/bin/echo --play %U", "custom command persists");
		check(p2.selected() == QLatin1String(player_launcher::custom_id()),
		      QString("custom player stays selected (%1)").arg(p2.selected()));
	}

	section("a saved player that is no longer installed");
	{
		QSettings s(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
		s.setValue("player/selected", "definitely-not-a-real-player");
		s.setValue("player/custom_command", "");
		s.sync();

		player_launcher p3;
		settings_store::load_into(&p3, nullptr, nullptr, nullptr, nullptr);
		check(p3.selected() != "definitely-not-a-real-player",
		      QString("falls back rather than keeping a dead choice (%1)")
		          .arg(p3.selected()));
		// **Only where there is something to fall back to.** `ok` was
		// initialised from `selected().isEmpty()` -- which is precisely the
		// no-player case -- so on a machine with none this passed while its
		// message claimed an installation had been verified. That machine is
		// CI: the workflow installs no media player at all, so this has been
		// green there and checking nothing.
		//
		// The escape hatch belongs in a printed skip, not in the initialiser,
		// which is the same correction made to the icon-directory check in
		// test_theme and to try_import.
		if (p3.selected().isEmpty()) {
			std::printf("  --    (no media player installed, so there is "
			             "nothing to fall back to; not checked)\n");
		} else {
			bool ok = false;
			for (const player_entry &e : p3.players())
				if (e.id == p3.selected() && e.installed)
					ok = true;
			check(ok, "and what it falls back to is actually installed");
		}
	}

	section("the custom command template");
	{
		// A script that records the arguments it was handed.
		const QString script = tmp + "/record.sh";
		const QString out    = tmp + "/args.txt";
		{
			QFile f(script);
			f.open(QIODevice::WriteOnly);
			// No printf format string: '%%' there means a literal per cent and
			// silently ate the arguments on the first attempt.
			f.write(QString("#!/bin/sh\n: > %1\nfor a in \"$@\"; do "
			                 "echo \"$a\" >> %1; done\n").arg(out).toUtf8());
			f.close();
			QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner |
			                                  QFile::ExeOwner);
		}

		player_launcher p;
		p.set_custom_command(script + " --flag %U --after");
		p.set_selected(player_launcher::custom_id());
		bool found = false;
		for (const player_entry &e : p.players())
			if (e.id == QLatin1String(player_launcher::custom_id()) && e.installed)
				found = true;
		check(found, "a custom command resolves to an executable");

		media_item item;
		item.url = QUrl("http://example.com/x.mp4");
		QString err;
		check(p.play(item, &err, QUrl("http://127.0.0.1:9/stream")),
		      QString("it launches (%1)").arg(err));
		spin(900);

		QFile f(out);
		f.open(QIODevice::ReadOnly);
		const QStringList args = QString::fromUtf8(f.readAll())
		                              .split('\n', Qt::SkipEmptyParts);
		check(args == QStringList({"--flag", "http://127.0.0.1:9/stream", "--after"}),
		      QString("%U is substituted in place (%1)").arg(args.join(" | ")));

		// No placeholder: the URL is appended, which is what typing a bare
		// program name means.
		QFile::remove(out);
		p.set_custom_command(script + " --only");
		check(p.play(item, &err, QUrl("http://127.0.0.1:9/second")), "launches again");
		spin(900);
		QFile f2(out);
		f2.open(QIODevice::ReadOnly);
		const QStringList args2 = QString::fromUtf8(f2.readAll())
		                               .split('\n', Qt::SkipEmptyParts);
		check(args2 == QStringList({"--only", "http://127.0.0.1:9/second"}),
		      QString("without %U the URL is appended (%1)").arg(args2.join(" | ")));

		p.set_custom_command("/nonexistent/player");
		bool still = false;
		for (const player_entry &e : p.players())
			if (e.id == QLatin1String(player_launcher::custom_id()) && e.installed)
				still = true;
		check(!still, "a command that resolves to nothing is not 'installed'");
	}

	section("the AI backend choice");
	{
		check(settings_store::ai_mode() == ai_choice::automatic,
		      "defaults to automatic — local-first, as the design describes");

		for (auto mode : { ai_choice::local_only, ai_choice::external,
			                  ai_choice::automatic }) {
			settings_store::set_ai_mode(mode);
			check(settings_store::ai_mode() == mode, "the mode round-trips");
		}

		ollama_provider local_ai;
		claude_provider external_ai;
		local_ai.set_endpoint(QUrl("http://192.168.1.9:11434"));
		local_ai.set_model("mistral");
		local_ai.set_probe_timeout(750);
		external_ai.set_model("claude-sonnet-5");
		settings_store::save_from(nullptr, nullptr, nullptr, &local_ai, &external_ai);

		ollama_provider local2;
		claude_provider external2;
		settings_store::load_into(nullptr, nullptr, nullptr, &local2, &external2);
		check(local2.endpoint() == QUrl("http://192.168.1.9:11434"),
		      QString("the Ollama endpoint persists (%1)")
		          .arg(local2.endpoint().toString()));
		check(local2.model() == "mistral", "the local model name persists");
		check(local2.probe_timeout() == 750,
		      QString("the probe timeout persists (%1)").arg(local2.probe_timeout()));
		check(external2.model() == "claude-sonnet-5", "the Claude model persists");
	}

	section("the API key must never reach the disk");
	{
		// The settings file is plain INI, and claude_provider states the key is
		// held in memory only. This asserts on the file itself rather than on
		// the absence of a setValue call, because that is the property that
		// actually matters and it cannot be broken silently.
		const QString secret = "sk-ant-DO-NOT-PERSIST-ME-0123456789";
		claude_provider ext;
		ext.set_api_key(secret);
		check(ext.has_api_key(), "the key is held in memory");
		settings_store::save_from(nullptr, nullptr, nullptr, nullptr, &ext);

		QSettings probe(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
		probe.sync();
		// **A negative needs a subject.** `leaked` can only stay false if the
		// loop runs at all, so an empty key list passes this exactly as loudly
		// as a clean one -- and an empty key list is what a save that silently
		// did nothing leaves behind. So the check would be greenest in the one
		// case that should alarm anybody: the write never happened.
		const QStringList keys = probe.allKeys();
		check(!keys.isEmpty(),
		      QString("the settings store has keys to search (%1)")
		          .arg(keys.size()));
		bool leaked = false;
		for (const QString &k : keys)
			if (probe.value(k).toString().contains(secret))
				leaked = true;
		check(!leaked, "and never written to the settings store");

		// And not anywhere in the file, under any key spelling.
		QFile f(probe.fileName());
		f.open(QIODevice::ReadOnly);
		const QByteArray raw = f.readAll();
		check(!raw.contains(secret.toUtf8()),
		      QString("nor present anywhere in %1").arg(probe.fileName()));

		claude_provider reloaded;
		settings_store::load_into(nullptr, nullptr, nullptr, nullptr, &reloaded);
		check(!reloaded.has_api_key() ||
		          !qEnvironmentVariableIsEmpty("ANTHROPIC_API_KEY"),
		      "and a fresh provider does not get it back from storage");
	}

	section("what the player is claimed to handle");
	{
		player_launcher p;

		// A custom command is of unknown capability. Claiming it reads
		// manifests would route a raw HLS playlist to it and fail at playback;
		// claiming it does not means the stream is assembled first, which
		// works whatever the command is.
		p.set_custom_command("/bin/echo");
		p.set_selected(player_launcher::custom_id());
		check(!p.selected_handles_streams(),
		      "a custom player is not assumed to read manifests");

		media_item hls;  hls.kind  = media_kind::hls;
		media_item dash; dash.kind = media_kind::dash;
		media_item file; file.kind = media_kind::direct;

		check(p.warning_for(hls).isEmpty(),
		      QString("HLS warns about nothing — it is assembled first (%1)")
		          .arg(p.warning_for(hls)));
		check(p.warning_for(file).isEmpty(), "a direct file warns about nothing");
		const QString d = p.warning_for(dash);
		check(!d.isEmpty(), "DASH still warns — there is no assembly for it");
		check(!d.contains("cannot play"),
		      QString("and does not overclaim what a custom command cannot do (%1)")
		          .arg(d));
		check(d.contains("not implemented"),
		      "naming the real limitation instead");

		// A native player takes a manifest directly and needs no warning.
		bool tested_native = false;
		for (const player_entry &e : p.players()) {
			if (!e.installed || !e.native_streams)
				continue;
			p.set_selected(e.id);
			check(p.selected_handles_streams(),
			      QString("%1 is routed the manifest directly").arg(e.label));
			check(p.warning_for(dash).isEmpty(),
			      QString("%1 needs no DASH warning").arg(e.label));
			tested_native = true;
			break;
		}
		if (!tested_native)
			std::printf("  --    (no native-stream player installed to check)\n");

		p.set_selected("no-such-player");
		check(p.warning_for(file).contains("No external player"),
		      "no player at all is reported plainly");
	}

	// What a request is, when the engine will not say. Android's WebView hands
	// the interceptor a url and headers and nothing else, so the kind has to be
	// inferred -- and the inference is shared, because a wrong guess here turns
	// a per-origin script rule into either no rule or the wrong one.
	section("resource kind: inferred from Accept and the url");
	{
		struct row { const char *accept; const char *url; resource_kind want; const char *why; };
		const row rows[] = {
			{"image/avif,image/webp,*/*", "https://s.example/pixel", resource_kind::image,
			 "an image Accept, with no extension in sight"},
			{"*/*", "https://s.example/app.js", resource_kind::script,
			 "a .js path when the header says nothing useful"},
			{"*/*", "https://s.example/app.js?v=91ac3", resource_kind::script,
			 "a cache-busting query does not hide the .js"},
			{"application/javascript", "https://s.example/bundle", resource_kind::script,
			 "an explicit javascript type beats a bare path"},
			{"text/css", "https://s.example/js/theme", resource_kind::other,
			 "a stylesheet living under /js/ is still a stylesheet"},
			{"text/html", "https://s.example/page", resource_kind::other,
			 "a document is neither"},
			{"*/*", "https://api.example/v1/items", resource_kind::other,
			 "a bare */* is NOT taken as script: fetch and XHR send it too"},
			{"", "https://s.example/logo.SVG", resource_kind::image,
			 "an upper-case extension, with no header at all"},
			{"", "https://s.example/a.png/redirect", resource_kind::other,
			 "an extension mid-path is not an extension"},
		};
		for (const row &r : rows)
			check(kind_from_hints(QString::fromLatin1(r.accept), QUrl(r.url)) == r.want,
			      QString::fromLatin1(r.why));
	}

	// And that the inference actually reaches a decision, since the point of it
	// is to let one filter serve both engines.
	section("resource kind: drives the same rules on either engine");
	{
		policy_engine e;
		request_filter f(&e);
		e.set_setting("news.example", policy::feature::images, policy::setting::block);

		request_context img;
		img.site_host = "news.example";
		img.request_host = "cdn.example";
		img.url = QUrl("https://cdn.example/hero");
		img.kind = kind_from_hints("image/webp,*/*", img.url);
		check(f.decide(img).block, "a blocked image is blocked when only headers named it");

		request_context data = img;
		data.url = QUrl("https://cdn.example/hero");
		data.kind = kind_from_hints("*/*", data.url);
		check(!f.decide(data).block,
		      "the same url fetched as data is not blocked by the image rule");
	}

	// Which urls an engine renders itself, and which the shell has to place.
	// Both backends ask this, by different routes -- Qt WebEngine registers
	// scheme handlers, Android's WebView asks about every navigation and treats
	// silence as consent -- so a scheme on one list and not the other is a link
	// that works on the desktop and dead-ends on a phone.
	section("scheme routing: what a web engine renders itself");
	{
		const char *pages[] = { "https://example.com/a", "http://example.com",
			                       "file:///tmp/x.html", "about:blank",
			                       "data:text/html,<b>hi", "view-source:https://e.com" };
		for (const char *u : pages)
			check(renders_as_page(QUrl(u)), QString("%1 is a page").arg(u));

		const char *elsewhere[] = { "magnet:?xt=urn:btih:abc", "mailto:a@b.c",
			                           "tel:+123", "intent://x#Intent;end" };
		for (const char *u : elsewhere)
			check(!renders_as_page(QUrl(u)),
			      QString("%1 is the shell's problem, not the engine's").arg(u));

		check(renders_as_page(QUrl("HTTPS://example.com")),
		      "the scheme is matched case-insensitively, as urls allow");
		check(!renders_as_page(QUrl("")), "an empty url is not a page");
	}

	// The narrower half of the same question. Written against the *exclusions*
	// rather than the accepts, because every one of them is a case where the
	// action would have opened a tab showing nothing and the accepts are the
	// easy half.
	section("scheme routing: what has a source worth showing");
	{
		const char *fetched[] = { "https://example.com/a", "http://example.com",
			                         "file:///tmp/x.html" };
		for (const char *u : fetched)
			check(has_viewable_source(QUrl(u)),
			      QString("%1 was fetched, so its markup can differ from its "
			               "rendering").arg(u));

		// Each of these renders_as_page, which is exactly why the two
		// predicates cannot be one.
		const char *assembled[] = { "about:blank", "chrome://settings",
			                           "data:text/html,<b>hi",
			                           "blob:https://e.com/1234",
			                           "view-source:https://e.com" };
		for (const char *u : assembled) {
			check(renders_as_page(QUrl(u)),
			      QString("%1 is still a page").arg(u));
			check(!has_viewable_source(QUrl(u)),
			      QString("but %1 has no fetched source behind it").arg(u));
		}

		check(has_viewable_source(QUrl("HTTPS://example.com")),
		      "matched case-insensitively, like its neighbour");
		check(!has_viewable_source(QUrl("")), "and an empty url has no source");
		check(!has_viewable_source(QUrl("magnet:?xt=urn:btih:abc")),
		      "nor does something that is not a page at all");
	}

	// The ad-host predicate, which the notes have listed as "verified mechanism,
	// untested matching" for a long time because pointing a real ad name at a
	// local server needs root or a Chromium flag Qt mangles. The predicate itself
	// needs neither.
	section("ad hosts: what the seed list matches");
	{
		policy_engine e;
		request_filter f(&e);
		check(f.is_ad_host("doubleclick.net"), "the name itself");
		check(f.is_ad_host("stats.g.doubleclick.net"), "and any subdomain of it");
		check(!f.is_ad_host("notdoubleclick.net"),
		      "but not a name that merely ends with the same letters");
		check(!f.is_ad_host("doubleclick.net.evil.example"),
		      "and not a lookalike that puts it on the left — the trick this exists to stop");
		check(!f.is_ad_host("example.com"), "an unrelated host is not an ad host");
		check(!f.is_ad_host(""), "and neither is nothing");
	}

	// The filter list reaching a decision at all. It did not, for the whole life
	// of the filter-evolution loop: rules were proposed, checked, accepted, saved
	// and listed, and nothing asked them about a request.
	section("accepted filter rules are enforced");
	{
		policy_engine e;
		filter_list   list;
		request_filter f(&e);

		request_context ctx;
		ctx.site_host    = "news.example";
		ctx.request_host = "tracker.example";
		ctx.url          = QUrl("https://tracker.example/beacon.gif");

		check(!f.decide(ctx).block, "with no list, nothing new is blocked");

		filter_rule r;
		r.text = "||tracker.example^";
		list.add(r);
		f.set_filter_list(&list);
		check(f.decide(ctx).block, "an accepted rule blocks the request it describes");

		request_context other = ctx;
		other.request_host = "cdn.example";
		other.url = QUrl("https://cdn.example/logo.png");
		check(!f.decide(other).block, "and leaves everything else alone");

		// The shield's escape hatch has to turn this off too, or "allow ads here"
		// half-works and the page still breaks for a reason the user disabled.
		e.set_setting("news.example", policy::feature::ads, policy::setting::allow);
		check(!f.decide(ctx).block,
		      "allowing ads for the site stops the accepted rules too");
	}

	// What `scope` actually means, which is two things.
	//
	// `parse_rule` fills it with the site for a cosmetic rule and with the
	// *blocked host* for `||host^`, and `evaluate()`'s breadth check depends on
	// the second. Filtering requests by it -- which reads like an obvious fix for
	// "blocks() ignores scope" -- would compare a blocked host against the
	// visiting site, match almost nothing, and silently disable every network
	// rule. These pin the real behaviour so that fix is not made twice.
	section("network rules are global, and the field that looks like scope is not");
	{
		filter_rule parsed;
		check(filter_list::parse_rule("||tracker.example^", &parsed) &&
		          parsed.scope == "tracker.example",
		      "a network rule's scope is the host it blocks, not a site it applies on");
		check(filter_list::parse_rule("news.example##.ad", &parsed) &&
		          parsed.scope == "news.example",
		      "while a cosmetic rule's scope really is the site");

		policy_engine e;
		filter_list   list;
		request_filter f(&e);
		f.set_filter_list(&list);
		filter_rule r;
		filter_list::parse_rule("||tracker.example^", &r);
		list.add(r);

		request_context here;
		here.site_host    = "news.example";
		here.request_host = "tracker.example";
		here.url          = QUrl("https://tracker.example/beacon.gif");
		check(f.decide(here).block, "the rule blocks on the site where it was written");

		request_context elsewhere = here;
		elsewhere.site_host = "other.example";
		check(f.decide(elsewhere).block,
		      "and on every other site too, which is what EasyList syntax means");
	}

	section("cosmetic rules are not network rules");
	{
		policy_engine e;
		filter_list   list;
		request_filter f(&e);
		f.set_filter_list(&list);
		filter_rule r;
		r.text     = "news.example##.ad-banner";
		r.cosmetic = true;
		list.add(r);

		request_context ctx;
		ctx.site_host = "news.example";
		ctx.request_host = "news.example";
		ctx.url = QUrl("https://news.example/ad-banner.png");
		check(!f.decide(ctx).block,
		      "a cosmetic rule blocks no request, whatever its text happens to match");
	}

	// The cosmetic half, which was the other end of the same gap: accepted `##`
	// rules were stored and listed and hid nothing.
	section("cosmetic rules: which selectors reach which page");
	{
		filter_list list;
		auto rule = [&](const char *text) {
			filter_rule r;
			filter_list::parse_rule(QString::fromLatin1(text), &r);
			list.add(r);
		};
		rule("news.example##.ad-banner");
		rule("news.example##div#promo");
		rule("shop.example##.sponsored");
		rule("||tracker.example^");            // network, not cosmetic

		const QStringList here = cosmetic_filters::selectors_for(&list, "news.example");
		check(here.contains(".ad-banner") && here.contains("div#promo"),
		      "a site's own cosmetic rules are offered to it");
		check(here.size() == 2,
		      QString("and nothing else — not another site's, not the network rule (%1)")
		          .arg(here.join(", ")));
		check(cosmetic_filters::selectors_for(&list, "sport.news.example")
		          .contains(".ad-banner"),
		      "subdomains of the scope count, the way the rest of the policy model works");
		check(cosmetic_filters::selectors_for(&list, "other.example").isEmpty(),
		      "an unrelated site gets none");
		check(cosmetic_filters::selectors_for(&list, "").isEmpty(),
		      "and neither does a page with no host yet");
		check(cosmetic_filters::selectors_for(nullptr, "news.example").isEmpty(),
		      "no list, no selectors, rather than a crash");
	}

	section("cosmetic rules: an unscoped one is not applied everywhere");
	{
		// evaluate() rejects an unscoped cosmetic rule at accept time for being
		// unbounded, so one can only exist in a hand-edited file. Honouring it
		// globally would route around the check that exists to stop exactly that.
		filter_list list;
		filter_rule r;
		r.text = "##.ad";
		r.cosmetic = true;      // scope deliberately empty
		list.add(r);
		check(cosmetic_filters::selectors_for(&list, "news.example").isEmpty(),
		      "a cosmetic rule with no site is applied to no site");
	}

	// What a system player is told the stream is. Android's ACTION_VIEW needs a
	// media type or the chooser offers every app that claims http -- which means
	// a browser, and handing the stream to a browser is a loop back to here.
	section("media type: what an intent carries");
	{
		struct row { const char *url; const char *want; };
		const row rows[] = {
			{ "https://x/stream.m3u8",        "application/vnd.apple.mpegurl" },
			{ "https://x/manifest.mpd",       "application/dash+xml" },
			{ "https://x/clip.mp4",           "video/mp4" },
			{ "https://x/clip.MP4",           "video/mp4" },
			{ "https://x/clip.webm",          "video/webm" },
			{ "https://x/song.mp3",           "audio/mpeg" },
			{ "https://x/seg.ts",             "video/mp2t" },
			{ "https://x/master.m3u8?v=3",    "application/vnd.apple.mpegurl" },
			{ "https://x/watch?id=7",         "video/*" },
			{ "https://x/no-extension",       "video/*" },
		};
		for (const row &r : rows)
			check(media_mime_for(QUrl(r.url)) == QLatin1String(r.want),
			      QString("%1 -> %2").arg(r.url, media_mime_for(QUrl(r.url))));
		check(media_mime_for(QUrl("https://x/a.mp4/redirect")) == "video/*",
		      "an extension mid-path is not an extension, here as elsewhere");
	}

	section("the system player entry is Android's, and only Android's");
	{
		player_launcher p;
		bool has_system = false;
		for (const player_entry &e : p.players())
			if (e.id == player_launcher::system_id())
				has_system = true;
#ifdef Q_OS_ANDROID
		check(has_system, "on Android the system chooser is the entry");
#else
		check(!has_system,
		      "on the desktop there is no system entry — players are named and probed");
#endif
	}

	section("auto-detect media is per site, and refusing means not recording");
	{
		// The same request three times, changing only the policy. If the
		// counts did not differ this would be testing that a url is saveable,
		// which `test_streamtype` already does and which would pass whatever
		// the gate did.
		request_context ctx;
		ctx.site_host    = "video.test";
		ctx.request_host = "cdn.test";
		ctx.url          = QUrl("https://cdn.test/movie.mp4");

		policy_engine policy;
		{
			media_detector det(&policy);
			det.on_request(ctx, request_decision{});
			check(det.count_for("video.test") == 1,
			      "allowed by default, so the stream is recorded");
		}
		{
			policy.set_setting("video.test", policy::feature::media_detect,
			                    policy::setting::block);
			media_detector det(&policy);
			det.on_request(ctx, request_decision{});
			check(det.count_for("video.test") == 0,
			      "blocked for this site, so nothing is written down at all");
		}
		{
			// Another site must be unaffected -- a per-site rule that turned
			// the detector off everywhere would pass the check above.
			request_context other = ctx;
			other.site_host = "elsewhere.test";
			media_detector det(&policy);
			det.on_request(other, request_decision{});
			check(det.count_for("elsewhere.test") == 1,
			      "and the rule does not reach a site it was not set on");
		}
		{
			// The default-constructed path a driver or a test uses.
			media_detector det;
			det.on_request(ctx, request_decision{});
			check(det.count_for("video.test") == 1,
			      "a detector with no policy watches everything, as before");
		}
	}

	section("the languages this browser asks for");
	{
		// The real thing this machine reports -- duplicated, and carrying
		// script subtags no browser sends. Using the measured value rather
		// than a tidy invention is the point: a tidy one would have passed
		// against a transformation that did nothing.
		const QStringList measured = { "en-US", "en-Latn-US", "en", "en",
			                            "en-Latn-US", "en-US" };
		check(accept_language::header_for(measured) == "en-US,en;q=0.9",
		      "the messy system list becomes what Chrome sends");

		const QStringList sv = { "sv-SE", "sv", "en-US", "en" };
		check(accept_language::header_for(sv)
		        == "sv-SE,sv;q=0.9,en-US;q=0.8,en;q=0.7",
		      "a Swedish system asks for Swedish first, English after");

		check(accept_language::header_for({ "sv-SE" }) == "sv-SE",
		      "a single language carries no quality, which is what q=1 means");
		check(accept_language::header_for({}).isEmpty(),
		      "and no languages produce no header rather than an empty one");

		// A quality of zero means "not acceptable", so a long list must not
		// run down into saying it refuses its own languages.
		const QString many = accept_language::header_for(
		  { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l" });
		check(!many.contains("q=0;") && !many.endsWith("q=0"),
		      "a long list never reaches q=0, which would refuse a language "
		      "it had just asked for");
	}

	section("the user agent this browser sends");
	{
		const QString qt_default =
		  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
		  "QtWebEngine/6.8.2 Chrome/122.0.6261.171 Safari/537.36";
		const QString out = user_agent::corrected(qt_default, 140);

		check(!out.contains("QtWebEngine"),
		      "the token no real browser sends is gone");
		check(out.contains("Chrome/140.0.0.0"),
		      "and the version is Chrome's reduced form, not a made-up build");
		check(!out.contains("6261"),
		      "the old build numbers do not survive -- 140 wearing 122's "
		      "numbers is a combination that never shipped");
		check(out.contains("X11; Linux x86_64"),
		      "the platform is Qt's own and is left alone");
		check(out.startsWith("Mozilla/5.0 (") && out.endsWith("Safari/537.36"),
		      "and the string is still shaped like a user agent");
		check(!out.contains("  "),
		      "removing the token leaves no doubled space");

		// Idempotent: the factory derives from whatever the profile reports,
		// and a string that has already been through here must not be
		// mangled a second time.
		check(user_agent::corrected(out, 140) == out,
		      "running it twice changes nothing");

		// An Android-shaped string keeps its platform, since this project
		// builds for a phone and a transformation that hardcoded X11 would
		// quietly lie there.
		const QString droid =
		  "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) "
		  "QtWebEngine/6.8.2 Chrome/122.0.6261.171 Mobile Safari/537.36";
		const QString dout = user_agent::corrected(droid, 140);
		check(dout.contains("Linux; Android 10; K") && dout.contains("Mobile"),
		      "an Android string keeps its platform and its Mobile token");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
