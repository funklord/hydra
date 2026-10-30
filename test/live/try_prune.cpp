// Do `xml-prune` and `m3u-prune` take the ads out of a stream's manifest?
//
// The two read what a player fetches -- a DASH manifest, an HLS playlist --
// and take out the ad periods and the ad segments before the player sees
// them. Neither can be run in QJSEngine, which has no XML parser and no
// network, so this runs them in Qt WebEngine against a local server: the
// page fetches each document by fetch and by XMLHttpRequest, with the rules
// uBlock's lists use for Paramount+ and Fox, and reports what it read. Each
// document also carries what must survive, so a pruner that took everything
// would fail as surely as one that took nothing.
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "scriptlets.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static const char k_mpd[] = R"XML(<?xml version="1.0" encoding="UTF-8"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT10M30S">
<Period id="content-1" start="PT0S"><AdaptationSet><Representation id="main"/></AdaptationSet></Period>
<Period id="pre-roll-1-ad-1" start="PT10M"><AdaptationSet><Representation id="ad"/></AdaptationSet></Period>
</MPD>)XML";

static const char k_m3u[] = "#EXTM3U\n#EXT-X-TARGETDURATION:10\n"
                             "#EXTINF:10,\nhttps://cdn.example/content/seg1.ts\n"
                             "#EXTINF:10,\nhttps://redirector.googlevideo.com/videoplayback?dclk_video_ads=1\n"
                             "#EXT-X-CUE:TYPE=\"SpliceOut\",DURATION=30\n#EXT-X-CUE-IN\n"
                             "#EXTINF:10,\nhttps://cdn.example/content/seg2.ts\n#EXT-X-ENDLIST\n";

static const char k_blk[] = "#EXTM3U\n#EXTINF:10,\nhttps://cdn.example/content/seg1.ts\n"
                             "#EXT-X-DISCONTINUITY\n#EXTINF:5,\nhttps://ads.example/break/a.ts\n"
                             "#EXT-X-DISCONTINUITY\n#EXTINF:10,\nhttps://cdn.example/content/seg2.ts\n";

static const char k_page[] = R"HTML(<!doctype html><html><body>
<script>
window.__out = {};
var mpd_of = function (t) {
  return (t.indexOf('pre-roll-1-ad') >= 0 ? 'ad ' : 'no-ad ') +
         (t.indexOf('content-1') >= 0 ? 'content' : 'no-content');
};
var m3u_of = function (t) {
  return (t.indexOf('dclk_video_ads') >= 0 ? 'ad ' : 'no-ad ') +
         (t.indexOf('seg1.ts') >= 0 && t.indexOf('seg2.ts') >= 0 ? 'content' : 'no-content') +
         (t.split('#EXTINF').length - 1 === 2 ? ' 2-extinf' : ' extinf=' + (t.split('#EXTINF').length - 1)) +
         (t.indexOf('SpliceOut') >= 0 || t.indexOf('CUE-IN') >= 0 ? ' cue' : ' no-cue');
};
var xhr = function (url, f, key) {
  var x = new XMLHttpRequest();
  x.onload = function () { window.__out[key] = f(x.responseText); };
  x.open('GET', url); x.send();
};
fetch('/ondemand/manifest.mpd').then(r => r.text()).then(t => { window.__out.mpd_fetch = mpd_of(t); });
xhr('/ondemand/manifest.mpd', mpd_of, 'mpd_xhr');
fetch('/play/list.m3u8').then(r => r.text()).then(t => { window.__out.m3u_fetch = m3u_of(t); });
xhr('/play/list.m3u8', m3u_of, 'm3u_xhr');
fetch('/xp/manifest.mpd').then(r => r.text()).then(t => {
  window.__out.mpd_xpath = mpd_of(t) + (t.indexOf('mediaPresentationDuration') >= 0 ? ' duration' : ' no-duration');
});
fetch('/blk/list.m3u8').then(r => r.text()).then(t => {
  window.__out.m3u_block = (t.indexOf('ads.example') >= 0 ? 'ad ' : 'no-ad ') +
    (t.indexOf('seg1.ts') >= 0 && t.indexOf('seg2.ts') >= 0 ? 'content' : 'no-content') +
    ' discontinuities=' + (t.split('DISCONTINUITY').length - 1);
});
fetch('/other/manifest.mpd').then(r => r.text()).then(t => { window.__out.mpd_elsewhere = mpd_of(t); });
</script></body></html>)HTML";

class origin : public QTcpServer {
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [s] {
			const QByteArray head = s->readAll();
			const QByteArray target = head.mid(4, head.indexOf(' ', 4) - 4);
			QByteArray body = k_page, type = "text/html";
			if (target.endsWith(".mpd")) { body = k_mpd; type = "application/dash+xml"; }
			else if (target.startsWith("/blk/")) { body = k_blk; type = "application/vnd.apple.mpegurl"; }
			else if (target.endsWith(".m3u8")) { body = k_m3u; type = "application/vnd.apple.mpegurl"; }
			s->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
			         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	origin server;
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		std::printf("could not listen\n");
		return 1;
	}

	section("the rules are read, as uBlock's lists write them");
	QList<scriptlet_call> calls;
	const char *const rules[] = {
		"127.0.0.1##+js(xml-prune, Period[id*=\"-roll-\"][id*=\"-ad-\"], , ondemand)",
		"127.0.0.1##+js(m3u-prune, dclk_video_ads, .m3u8)",
		"127.0.0.1##+js(xml-prune, 'xpath(//*[name()=\"MPD\"]/@mediaPresentationDuration "
		"| //*[name()=\"Period\"][contains(@id\\,\"-ad-\")])', , /xp/)",
		"127.0.0.1##+js(m3u-prune, /#EXT-X-DISCONTINUITY\\n#EXTINF[^\\n]*\\nhttps:\\/\\/ads[^\\n]*\\n"
		"#EXT-X-DISCONTINUITY\\n/gm, /blk/)"
	};
	for (const char *line : rules) {
		filter_rule r;
		scriptlet_call c;
		QString why;
		const QString text = QString::fromLatin1(line);
		const QString inside = text.mid(text.indexOf("+js(") + 4).chopped(1);
		const bool ok = scriptlets::parse_call(inside, &c, &why);
		c.scope = "127.0.0.1";
		check(ok, QString("%1 %2").arg(text, why));
		calls << c;
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	QWidget host;
	host.resize(800, 600);
	web_view_backend *view = factory.create_view(&host);
	view->widget()->resize(800, 600);
	host.show();
	view->apply_settings(view_settings());
	view->inject_main_world_script("hydra-scriptlets", scriptlets::source_for(calls));

	bool loaded = false;
	QObject::connect(view, &web_view_backend::load_finished, view, [&](bool) { loaded = true; });
	view->load(QUrl(QString("http://127.0.0.1:%1/page").arg(server.serverPort())));
	for (int i = 0; i < 300 && !loaded; ++i)
		spin(50);
	spin(2000);

	QString out;
	bool got = false;
	view->run_page_probe(QStringLiteral("JSON.stringify(window.__out)"),
	                     [&](const QString &s) { out = s; got = true; });
	for (int i = 0; i < 200 && !got; ++i)
		spin(50);
	std::printf("  page read: %s\n", qPrintable(out));

	section("the ad is gone from each document, by each transport, and nothing else is");
	check(out.contains("\"mpd_fetch\":\"no-ad content\""), "xml-prune, by fetch");
	check(out.contains("\"mpd_xhr\":\"no-ad content\""), "xml-prune, by XMLHttpRequest");
	check(out.contains("\"m3u_fetch\":\"no-ad content 2-extinf no-cue\""),
	      "m3u-prune, by fetch, the segment's #EXTINF with it, and a SpliceOut cue block");
	check(out.contains("\"m3u_xhr\":\"no-ad content 2-extinf no-cue\""), "m3u-prune, by XMLHttpRequest");
	check(out.contains("\"mpd_xpath\":\"no-ad content no-duration\""),
	      "xml-prune by xpath, quoted, with an escaped comma: an attribute and a period, "
	      "in the manifest's own namespace");
	check(out.contains("\"m3u_block\":\"no-ad content discontinuities=0\""),
	      "m3u-prune with a multi-line pattern cuts the ad's whole block");
	check(out.contains("\"mpd_elsewhere\":\"ad content\""),
	      "and a manifest from an address the rule does not name is left alone");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
