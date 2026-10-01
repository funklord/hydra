// Do a learned extractor's headers actually reach the CDN? The proxy is the
// only thing that can put them there, so ask a server what it received.
//
// **The server is in-process**, from `echo_server.h`. It used to be
// `test/echohdr.py` on port 8850, which is why these five checks -- the only
// thing in the tree that asks whether a `stream_context` survives the hop
// through the proxy at all -- ran outside `make test` for whoever started it
// by hand.
#include "local_proxy.h"
#include "echo_server.h"

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

	echo_server server;
	const QString base = server.start();
	if (base.isEmpty()) {
		std::printf("could not listen\n");
		return 1;
	}
	const QString upstream = base + "/stream";

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
	std::printf("  ..    upstream saw %lld headers\n", (long long)got.size());

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

	// **The resolver is asked about the stream's url, not the page's.**
	// `ctx.cookies` was filled once from `v->url()` and sent to whatever host
	// the entry fetched, so a page whose video sits on an unrelated CDN handed
	// that CDN its session cookie. The field is a resolver now and the proxy
	// asks it at publish time, for the upstream.
	{
		stream_context jar;
		jar.referer = "https://site.example/watch/1";
		QList<QUrl> asked;
		jar.cookies_for = [&asked](const QUrl &to) {
			asked << to;
			// What a jar answers: the page's cookies for the page's host, and
			// nothing for a stranger. The echo server is on 127.0.0.1, so a
			// resolver that behaves like `cookiesForUrl` says nothing here.
			return to.host() == "site.example" ? QString("sid=abc123")
			                                    : QString();
		};

		const QUrl second = proxy.publish(QUrl(upstream), jar);
		check(second.isValid(), "published with a resolver instead of a header");
		// **Nothing is asked at publish**, deliberately: a header cached then
		// is a cookie for the host before a redirect, sent to the host after.
		check(asked.isEmpty(),
		      QString("publishing asks nobody (%1)")
		          .arg(asked.isEmpty() ? QStringLiteral("asked nobody")
		                                : asked.first().toString()));

		QNetworkReply *r2 = net.get(QNetworkRequest(second));
		QEventLoop l2;
		QObject::connect(r2, &QNetworkReply::finished, &l2, &QEventLoop::quit);
		QTimer::singleShot(15000, &l2, &QEventLoop::quit);
		l2.exec();
		const QJsonObject saw = QJsonDocument::fromJson(r2->readAll()).object();

		// **The check the old shape could not pass.** A resolver that answers
		// only for the page sends nothing to a CDN on another host -- where the
		// page-filled field sent `sid=abc123` to it.
		check(!saw.contains("cookie") ||
		        saw.value("cookie").toString().isEmpty(),
		      QString("and a host the page's cookies do not belong to gets none "
		               "(%1)").arg(saw.value("cookie").toString()));
		// Not silence about everything: the rest of the context still arrives,
		// so the check above is about cookies and not about a dead entry.
		check(saw.value("referer").toString() == jar.referer,
		      QString("while the Referer still arrives (%1)")
		          .arg(saw.value("referer").toString()));
		check(asked.size() == 1 && asked.first() == QUrl(upstream),
		      QString("and the fetch asked about the upstream (%1)")
		          .arg(asked.isEmpty() ? QStringLiteral("(not asked)")
		                                : asked.first().toString()));
		r2->deleteLater();
	}

	// And an extractor's named header wins over the resolver, which is what
	// the two fields are for: a rule written for one CDN knows more than a jar.
	{
		stream_context named;
		named.cookies = "named=by-extractor";
		bool asked = false;
		named.cookies_for = [&asked](const QUrl &) {
			asked = true;
			return QString("from=the-jar");
		};
		const QUrl third = proxy.publish(QUrl(upstream), named);
		QNetworkReply *r3 = net.get(QNetworkRequest(third));
		QEventLoop l3;
		QObject::connect(r3, &QNetworkReply::finished, &l3, &QEventLoop::quit);
		QTimer::singleShot(15000, &l3, &QEventLoop::quit);
		l3.exec();
		const QJsonObject saw = QJsonDocument::fromJson(r3->readAll()).object();
		check(saw.value("cookie").toString() == "named=by-extractor",
		      QString("a named Cookie wins (%1)")
		          .arg(saw.value("cookie").toString()));
		check(!asked, "and the jar is not even asked");
		r3->deleteLater();
	}

	// **What the proxy writes when upstream redirects**, which nothing asked.
	// The upstream path sets no redirect policy, so it takes the manager's
	// default -- and it writes a status line and headers from inside
	// `metaDataChanged`. Every sibling in the tree is explicit about redirects:
	// `stream_probe` and `network_fetcher` set `NoLessSafeRedirectPolicy`, and
	// `hls_assembler` counts them itself. This one is the exception, and a
	// signed CDN url redirecting to a regional edge is the ordinary case.
	//
	// Asked as an experiment rather than asserted from reading: if that signal
	// fires for the 30x as well as for the final response, a player gets two
	// status lines in one response and shows nothing.
	std::printf("\n== a redirecting upstream ==\n");
	{
		server.redirect_from = "/via";
		server.redirect_to   = "/stream";
		server.redirects_served = 0;

		stream_context ctx2;
		ctx2.referer = "https://site.example/watch/1";
		const QUrl via2 = proxy.publish(QUrl(base + "/via/signed"), ctx2);
		check(via2.isValid(), "published a url that redirects");

		QNetworkReply *r4 = net.get(QNetworkRequest(via2));
		QEventLoop l4;
		QObject::connect(r4, &QNetworkReply::finished, &l4, &QEventLoop::quit);
		QTimer::singleShot(15000, &l4, &QEventLoop::quit);
		l4.exec();
		const QByteArray raw = r4->readAll();
		const int code4 =
		  r4->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QString err4 = r4->errorString();
		std::printf("  ..    client saw %d, %lld byte(s), err=%s\n", code4,
		             (long long)raw.size(), qPrintable(err4));
		r4->deleteLater();

		check(server.redirects_served == 1,
		      QString("the fixture served the redirect (%1)")
		          .arg(server.redirects_served));

		// **The property that matters**, read off the bytes the client got:
		// exactly one response. `QNetworkAccessManager` on this side parses
		// what the proxy wrote, so a second status line inside the body is
		// what a second header write looks like from here.
		check(!raw.contains("HTTP/1.1"),
		      QString("the client's body carries no second status line (%1)")
		          .arg(QString::fromUtf8(raw.left(60))));
		// And the redirect was actually followed to something: the echo
		// server's JSON, not an empty 302 body.
		const QJsonObject after = QJsonDocument::fromJson(raw).object();
		check(!after.isEmpty(),
		      QString("and the final response arrived (%1 header(s) echoed)")
		          .arg(after.size()));
		check(after.value("referer").toString() == ctx2.referer,
		      QString("with the context still on it (%1)")
		          .arg(after.value("referer").toString()));
	}

	// **The hop asks the jar again**, which is what keeps the cookie fix true
	// across a redirect. Qt's own `RedirectPolicyAttribute` would re-send the
	// raw headers it was handed, including a Cookie built for the host before
	// the hop -- which is why this is followed by hand.
	std::printf("\n== the context is re-asked per hop ==\n");
	{
		server.redirects_served = 0;
		QList<QUrl> asked;
		stream_context ctx3;
		ctx3.referer = "https://site.example/watch/1";
		ctx3.cookies_for = [&asked](const QUrl &to) {
			asked << to;
			return QString();
		};
		const QUrl via3 = proxy.publish(QUrl(base + "/via/two"), ctx3);
		QNetworkReply *r5 = net.get(QNetworkRequest(via3));
		QEventLoop l5;
		QObject::connect(r5, &QNetworkReply::finished, &l5, &QEventLoop::quit);
		QTimer::singleShot(15000, &l5, &QEventLoop::quit);
		l5.exec();
		r5->deleteLater();

		// Once at publish for the first url, once for the hop's.
		QStringList paths;
		for (const QUrl &u : asked)
			paths << u.path();
		check(paths.size() == 2 && paths.at(0) == "/via/two" &&
		        paths.at(1) == "/stream",
		      QString("asked about each url in turn (%1)")
		          .arg(paths.isEmpty() ? QStringLiteral("never asked")
		                                : paths.join(" then ")));
	}

	// **A 30x the proxy cannot act on is an error the player can report.**
	// It used to be relayed as a bare status with no Location, which is zero
	// bytes and no diagnosis.
	std::printf("\n== a redirect with nowhere to go ==\n");
	{
		server.redirect_from = "/nowhere";
		server.redirect_to   = "";          // a 302 with an empty Location
		stream_context ctx4;
		const QUrl via4 = proxy.publish(QUrl(base + "/nowhere/x"), ctx4);
		QNetworkReply *r6 = net.get(QNetworkRequest(via4));
		QEventLoop l6;
		QObject::connect(r6, &QNetworkReply::finished, &l6, &QEventLoop::quit);
		QTimer::singleShot(15000, &l6, &QEventLoop::quit);
		l6.exec();
		const int code6 =
		  r6->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		r6->deleteLater();
		check(code6 == 502,
		      QString("the player is told upstream redirected nowhere (%1)")
		          .arg(code6));
	}

	// And a loop is bounded rather than chased.
	std::printf("\n== a redirect loop ==\n");
	{
		server.redirect_from = "/loop";
		server.redirect_to   = "/loop/again";
		server.redirects_served = 0;
		stream_context ctx5;
		const QUrl via5 = proxy.publish(QUrl(base + "/loop/start"), ctx5);
		QNetworkReply *r7 = net.get(QNetworkRequest(via5));
		QEventLoop l7;
		QObject::connect(r7, &QNetworkReply::finished, &l7, &QEventLoop::quit);
		QTimer::singleShot(20000, &l7, &QEventLoop::quit);
		l7.exec();
		const int code7 =
		  r7->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		r7->deleteLater();
		check(code7 == 508,
		      QString("a loop ends in an error rather than for ever (%1)")
		          .arg(code7));
		check(server.redirects_served <= 6,
		      QString("after a bounded number of hops (%1)")
		          .arg(server.redirects_served));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
