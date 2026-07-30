// Do a learned extractor's headers actually reach the CDN? The proxy is the
// only thing that can put them there, so ask a server what it received.
#include "local_proxy.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QString upstream = argc > 1 ? argv[1] : "http://127.0.0.1:8850/stream";

	local_proxy proxy;
	check(proxy.start(), "proxy listening");

	stream_context ctx;
	ctx.referer    = "https://site.example/watch/1";
	ctx.user_agent = "Hydra/1.0 test";
	ctx.cookies    = "sid=abc123";
	ctx.extra.insert("X-Playback-Session-Id", "sess-42");
	ctx.extra.insert("Origin", "https://site.example");

	const QUrl via = proxy.publish(QUrl(upstream), ctx);
	check(via.isValid(), "published");

	QNetworkAccessManager net;
	QNetworkReply *r = net.get(QNetworkRequest(via));
	QEventLoop loop;
	QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QTimer::singleShot(15000, &loop, &QEventLoop::quit);
	loop.exec();

	const QJsonObject got = QJsonDocument::fromJson(r->readAll()).object();
	std::printf("  ..    upstream saw %d headers\n", got.size());

	check(got.value("referer").toString() == ctx.referer,
	      QString("Referer arrived (%1)").arg(got.value("referer").toString()));
	check(got.value("user-agent").toString() == ctx.user_agent,
	      QString("User-Agent arrived (%1)").arg(got.value("user-agent").toString()));
	check(got.value("cookie").toString() == ctx.cookies,
	      QString("Cookie arrived (%1)").arg(got.value("cookie").toString()));
	check(got.value("x-playback-session-id").toString() == "sess-42",
	      QString("an extractor's own header arrived (%1)")
	          .arg(got.value("x-playback-session-id").toString()));
	check(got.value("origin").toString() == "https://site.example",
	      "and a second one");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
