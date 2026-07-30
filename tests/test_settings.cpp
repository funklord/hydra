// Settings persistence and the custom-player command template.
#include "settings_dialog.h"
#include "download_manager.h"
#include "player_launcher.h"
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
		bool ok = p3.selected().isEmpty();
		for (const player_entry &e : p3.players())
			if (e.id == p3.selected() && e.installed)
				ok = true;
		check(ok, "and what it falls back to is actually installed");
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
		bool leaked = false;
		for (const QString &k : probe.allKeys())
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
