// What a page sends to somebody else (`agent_fetch`).
//
// The reading is checked on fixtures shaped like what was met: Dagens
// ETC's teaser, which declares its paywall in JSON-LD and does not send the
// paywalled part; the same page with that part present; and the bot check
// the crawlers were given instead (status 247, 516 bytes, all script). The
// fetcher is checked against a local server that answers each client by its
// User-Agent and sets a cookie, so a second run that sent one back would
// show it.
#include "agent_fetch.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static const char k_teaser[] = R"HTML(<!doctype html><html><head>
<script type="application/ld+json">{"@context":"https://schema.org","@type":"NewsArticle",
"isAccessibleForFree":"False","hasPart":{"@type":"WebPageElement",
"isAccessibleForFree":"False","cssSelector":".paywalled"}}</script></head><body>
<p>Short.</p>
<p>The opening paragraph of the teaser, long enough to count as one of the article's.</p>
<p>A second paragraph of the teaser, and then the subscription box takes over the page.</p>
</body></html>)HTML";

static const char k_full[] = R"HTML(<!doctype html><html><head>
<script type="application/ld+json">{"@graph":[{"@type":"WebSite"},{"@type":"NewsArticle",
"isAccessibleForFree":false,"hasPart":{"@type":"WebPageElement","isAccessibleForFree":false,
"cssSelector":".paywalled"}}]}</script></head><body>
<p>The opening paragraph of the teaser, long enough to count as one of the article's.</p>
<div class="body paywalled">
<p>The paywalled part, sent in full to this client, first paragraph of several.</p>
<p>The paywalled part, sent in full to this client, second paragraph of several.</p>
<p>The paywalled part, sent in full to this client, third paragraph of several.</p>
</div></body></html>)HTML";

static const char k_challenge[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><script type="text/javascript" src="/kramericaindustries.ac_v2.lib.js"></script><script type="text/javascript">
;;window.rbzns={"bereshit":"1","location_host":"www.etc.se","seed":"x"};winsocks();</script></head><body></body></html>)HTML";

static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Answers by User-Agent: Googlebot the full article, bingbot the bot check,
// everybody else the teaser. Records each request's headers.
class by_agent : public QTcpServer {
public:
	QList<QByteArray> heads;
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			const QByteArray head = s->readAll();
			heads << head;
			QByteArray body = k_teaser, status = "200 OK";
			if (head.contains("Googlebot")) body = k_full;
			if (head.contains("bingbot")) { body = k_challenge; status = "247 Challenge"; }
			s->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html\r\n"
			         "Set-Cookie: seen=1; Path=/\r\nContent-Length: " +
			         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	using namespace agent_fetch;

	section("a teaser that declares its paywall and does not send it");
	{
		const reading r = read("browser", 200, k_teaser);
		check(r.paragraphs == 2 && r.text_chars > 150,
		      QString("two paragraphs; the short one does not count (%1)").arg(r.paragraphs));
		check(r.declared_paywall && r.paywalled_selector == ".paywalled" && !r.paywalled_present,
		      "declared not free, the rest under .paywalled, which is not in the answer");
		check(!r.challenge, "and it is the page, not a bot check");
		check(describe(r).contains("which is not in this answer"), describe(r));
	}

	section("the same page with the paywalled part in it");
	{
		const reading r = read("googlebot", 200, k_full);
		check(r.paragraphs == 4 && r.declared_paywall && r.paywalled_present,
		      "read through @graph, and the part is found by its class: " + describe(r));
	}

	section("the paid part named by a bare class, as a minifier writes it");
	{
		const reading r = read("googlebot", 200,
		  "<script type=\"application/ld+json\">{\"isAccessibleForFree\":false,\"hasPart\":"
		  "{\"isAccessibleForFree\":false,\"cssSelector\":\".paywalled\"}}</script>"
		  "<div class=paywalled><p>The paid part, with its class written without quotes.</p></div>");
		check(r.paywalled_present, "class=paywalled is the paid part: " + describe(r));
		const reading other = read("googlebot", 200,
		  "<script type=\"application/ld+json\">{\"isAccessibleForFree\":false,\"hasPart\":"
		  "{\"isAccessibleForFree\":false,\"cssSelector\":\".paywalled\"}}</script>"
		  "<div class=paywalled-banner><p>A banner whose class only begins the same way.</p></div>");
		check(!other.paywalled_present, "and class=paywalled-banner is not");
	}

	section("a bot check instead of the page");
	{
		const reading r = read("bingbot", 247, k_challenge);
		check(r.challenge && r.paragraphs == 0, describe(r));
		check(describe(r).contains("bot check"), "said as such");
	}

	section("what counts as more, and what is shown");
	{
		const reading tab = read("this tab", 200, k_teaser);
		// More text with no paid part in it: helps only when it is clearly
		// more -- a longer sidebar is not the article.
		auto with = [](int paragraphs) {
			QByteArray b = "<html><body>";
			for (int i = 0; i < paragraphs; ++i)
				b += "<p>A paragraph of some other text, long enough to be counted here.</p>";
			return read("bingbot", 200, b + "</body></html>");
		};
		check(helps(tab, { with(3) }) == -1, "a little more text is not taken for the article");
		check(helps(tab, { with(30) }) == 0, "a great deal more is");
		reading blocked = with(30);
		blocked.challenge = true;
		reading failed = with(30);
		failed.status = 403;
		check(helps(tab, { blocked, failed }) == -1, "a bot check or a refusal never is");
		check(helps(tab, { with(20), with(30), with(25) }) == 1, "of several, the longest");

		reading r = read("googlebot", 200,
		  QByteArray("<html><head><script>putTheWallBack()</script>"
		             "<script type=\"application/ld+json\">{\"isAccessibleForFree\":false,"
		             "\"hasPart\":{\"isAccessibleForFree\":false,\"cssSelector\":\".paywalled\"}}"
		             "</script></head><body><div class=\"paywalled\"><p>The rest.</p></div>"
		             "<SCRIPT src=\"/wall.js\"></SCRIPT></body></html>"));
		r.body = QByteArray("<html><head><script>putTheWallBack()</script>"
		             "<script type=\"application/ld+json\">{}</script></head><body>"
		             "<div class=\"paywalled\"><p>The rest.</p></div>"
		             "<SCRIPT src=\"/wall.js\"></SCRIPT></body></html>");
		const QString shown = servable(r);
		check(!shown.contains("putTheWallBack") && !shown.contains("wall.js"),
		      "the copy is shown without its scripts: " + shown.left(160));
		check(shown.contains("application/ld+json") && shown.contains("The rest."),
		      "its JSON-LD and its text stay");
		check(shown.contains(".paywalled{display:block!important") &&
		        shown.indexOf(".paywalled{") < shown.indexOf("</head>"),
		      "and the paid part is forced visible, in the head");
		r.body = QByteArray(1600 * 1000, 'x');
		check(servable(r).isEmpty(), "past what a view takes, nothing is offered");
	}

	section("the catalog is closed and named");
	{
		QStringList names;
		for (const agent &a : catalog())
			names << a.name;
		const QStringList want = { "browser", "from-google", "googlebot", "bingbot",
		                           "facebook", "twitterbot" };
		check(names == want, names.join(", "));
	}

	section("fetched as every client, each told apart, no cookie carried");
	{
		by_agent server;
		check(server.listen(QHostAddress::LocalHost, 0), "a local server");
		const QUrl url(QStringLiteral("http://127.0.0.1:%1/story").arg(server.serverPort()));
		agent_fetcher f;
		QList<reading> got;
		bool done = false;
		f.fetch_all(url, [&](const QList<reading> &r) { got = r; done = true; });
		for (int i = 0; i < 200 && !done; ++i)
			spin(50);
		check(done && got.size() == catalog().size(), QString("%1 readings").arg(got.size()));
		auto of = [&](const char *n) {
			for (const reading &r : got)
				if (r.agent == n)
					return r;
			return reading();
		};
		check(of("googlebot").paywalled_present && of("googlebot").paragraphs == 4,
		      "googlebot got the article: " + describe(of("googlebot")));
		check(of("bingbot").challenge && of("bingbot").status == 247,
		      "bingbot the bot check: " + describe(of("bingbot")));
		check(of("browser").paragraphs == 2 && !of("browser").paywalled_present,
		      "the browser the teaser");
		bool referer = false;
		for (const QByteArray &h : server.heads)
			// Lower case: Qt sends header names that way.
			referer = referer || h.toLower().contains("\r\nreferer: https://www.google.com/");
		check(referer, "from-google sent the Referer");

		// A second run: the server set a cookie on every answer above.
		const int before = server.heads.size();
		done = false;
		f.fetch_all(url, [&](const QList<reading> &r) { got = r; done = true; });
		for (int i = 0; i < 200 && !done; ++i)
			spin(50);
		bool cookie = false;
		for (int i = before; i < server.heads.size(); ++i)
			cookie = cookie || server.heads[i].toLower().contains("\r\ncookie:");
		check(done && server.heads.size() == before + catalog().size() && !cookie,
		      "a second run sends no cookie the first was given");

		// **Which answer to show** (`helps`): from a teaser, the crawler's
		// with the paid part; from a tab that has it, none.
		const reading teaser = read("this tab", 200, k_teaser);
		const int pick = helps(teaser, got);
		check(pick >= 0 && got[pick].agent == QLatin1String("googlebot"),
		      QString("from the teaser, googlebot's answer is the one that helps (%1)")
		        .arg(pick >= 0 ? got[pick].agent : QStringLiteral("none")));
		check(helps(read("this tab", 200, k_full), got) == -1,
		      "and from a tab that already has the paid part, none does");
		check(!got[pick].body.isEmpty() && got[pick].url == url,
		      "the answer keeps what was sent and where from");

		done = false;
		f.fetch_all(QUrl("file:///etc/passwd"), [&](const QList<reading> &r) { got = r; done = true; });
		check(done && got.size() == 1 && got[0].status == 0 && !got[0].error.isEmpty(),
		      "an address that is not on the web is refused: " + describe(got.value(0)));
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
