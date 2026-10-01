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
		check(asked.size() == 1 && asked.first() == QUrl(upstream),
		      QString("the resolver was asked about the upstream (%1)")
		          .arg(asked.isEmpty() ? QString("(not asked)")
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

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
