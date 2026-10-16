// What the Annoyed button sees on the page: `ad_probe`, run through the real
// backend's `run_probe` against fixture pages in a real engine.
//
// **Both directions, because a detector is only worth its false positives.**
// A probe that finds an ad in every page's header is one nobody reads twice,
// so each kind of finding is checked to fire on an ad and to stay silent on
// the near miss: `header` and `shadow` beside `ad`, `Adsorption` beside
// `Ads`, an ad that is hidden, a wrapper and its inner divs.
//
// Offline: the pages are files, and nothing is fetched. An ad-host iframe is
// still an element with a size whether or not its source ever loads.
#include "ad_probe.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QFile>
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

// Loads `html` as a file and probes it. A load that never finishes, or an
// answer that never comes, ends at a deadline rather than hanging the suite.
static ad_probe::findings probe(web_view_backend *view, const QTemporaryDir &dir,
                                const char *name, const QString &html,
                                bool *answered) {
	const QString path = dir.filePath(QString::fromLatin1(name));
	QFile f(path);
	f.open(QIODevice::WriteOnly);
	f.write(html.toUtf8());
	f.close();

	QEventLoop loaded;
	auto c = QObject::connect(view, &web_view_backend::load_finished,
	                          &loaded, [&loaded](bool) { loaded.quit(); });
	QTimer::singleShot(10000, &loaded, &QEventLoop::quit);
	view->load(QUrl::fromLocalFile(path));
	loaded.exec();
	QObject::disconnect(c);

	QString json;
	bool got = false;
	QEventLoop answer;
	view->run_probe(ad_probe::source(), [&](const QString &j) {
		json = j;
		got = true;
		answer.quit();
	});
	QTimer::singleShot(10000, &answer, &QEventLoop::quit);
	if (!got)
		answer.exec();
	ad_probe::findings out;
	*answered = got && ad_probe::parse(json, &out);
	return out;
}

// Serves `/top` -- a page with a frame from `localhost`, another origin --
// and `/ad`, the frame: an ad-named element and a label, as a served ad is.
class frame_server : public QTcpServer {
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray head = s->readAll();
			QByteArray body;
			if (head.startsWith("GET /top"))
				body = "<html><body><p>The story.</p><iframe src=\"http://localhost:" +
				       QByteArray::number(serverPort()) +
				       "/ad\" width=320 height=270></iframe></body></html>";
			else
				body = "<html><body><div class=\"ad-banner\" style=\"width:300px;"
				       "height:250px\">Advertisement</div></body></html>";
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
			         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
			         body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

static const char *k_style =
  "<style>div{min-width:40px;min-height:40px}</style>";

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	QTemporaryDir dir;
	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	QWidget host;
	host.resize(1280, 900);
	web_view_backend *view = factory.create_view(&host);
	view->widget()->resize(1280, 900);
	host.show();

	section("a page with nothing on it finds nothing");
	{
		bool ok = false;
		const ad_probe::findings f = probe(view, dir, "clean.html",
		  QString("<html><body>%1<header class=\"site-header\">Hi</header>"
		          "<div class=\"shadow-box read-more\">text</div>"
		          "<p>Adsorption is a surface process.</p>"
		          "<div id=\"download\">get it</div></body></html>")
		      .arg(k_style), &ok);
		check(ok, "the probe answers");
		check(f.empty(), QString("and finds nothing in header, shadow, "
		                         "read-more, download or Adsorption (%1)")
		                     .arg(ad_probe::describe(f).join(" | ")));
	}

	section("a player says it is in an ad");
	{
		bool ok = false;
		const ad_probe::findings f = probe(view, dir, "player.html",
		  QString("<html><body>%1"
		          "<div id=\"movie_player\" class=\"html5-video-player "
		          "ad-created ad-showing ad-interrupting\">v</div>"
		          "<div class=\"html5-video-player ad-created\">v</div>"
		          "<div class=\"video-js vjs-ad-playing\">v</div>"
		          "<div class=\"video-js vjs-paused\">v</div>"
		          "</body></html>").arg(k_style), &ok);
		check(ok && f.players.size() == 3,
		       QString("two YouTube players and Video.js's, and not the "
		               "paused player (%1)").arg(f.players.join(" | ")));
		check(!f.players.isEmpty() &&
		          f.players.first().contains("ad-created ad-showing "
		                                     "ad-interrupting"),
		       QString("naming the classes that say so (%1)")
		           .arg(f.players.value(0)));
		// Measured on YouTube: the player's own `ad-created` class made the
		// element scan report the player as an ad-named element.
		check(f.elements.isEmpty(),
		       QString("and a player is not also reported as an ad-named "
		               "element (%1)").arg(f.elements.join(" | ")));
	}

	section("visible ad-named elements, once each");
	{
		bool ok = false;
		const ad_probe::findings f = probe(view, dir, "elements.html",
		  QString("<html><body>%1"
		          "<div class=\"ad-wrapper\" style=\"width:300px;height:250px\">"
		          "<div class=\"ad-inner\">inner</div></div>"
		          "<aside id=\"sponsored-box\" style=\"display:block;"
		          "width:200px;height:100px\">s</aside>"
		          "<div class=\"ad-hidden-slot\" style=\"display:none\">h</div>"
		          "<div class=\"ads tiny\" style=\"width:5px;height:5px;"
		          "min-width:0;min-height:0\">t</div>"
		          "</body></html>").arg(k_style), &ok);
		const QString all = f.elements.join(" | ");
		check(ok && f.elements.size() == 2,
		       QString("the wrapper and the sponsored box, not the inner div, "
		               "the hidden slot or the 5px one (%1)").arg(all));
		check(all.contains("div.ad-wrapper") && all.contains("300x250") &&
		          all.contains("aside#sponsored-box"),
		       QString("each with a selector and its size (%1)").arg(all));
	}

	section("frames from ad hosts, and labels");
	{
		bool ok = false;
		const ad_probe::findings f = probe(view, dir, "frames.html",
		  QString("<html><body>%1"
		          // No border, so the size reported is the one the ad was
		          // given -- a default iframe adds two pixels each side.
		          "<iframe src=\"https://googleads.g.doubleclick.net/pagead/x\" "
		          "width=\"728\" height=\"90\" style=\"border:0\"></iframe>"
		          "<iframe src=\"https://www.example.org/embed\" width=\"300\" "
		          "height=\"200\"></iframe>"
		          "<span>Sponsored</span><button>Skip Ad</button>"
		          "<span>Ad 1 of 2</span><span>Adsorption</span>"
		          "<span>Reading ads is fun</span>"
		          "</body></html>").arg(k_style), &ok);
		check(ok && f.frames.size() == 1 &&
		          f.frames.first().contains("doubleclick.net") &&
		          f.frames.first().contains("728x90"),
		       QString("the ad-host frame and not the other (%1)")
		           .arg(f.frames.join(" | ")));
		const QString labels = f.labels.join(" | ");
		check(f.labels.size() == 3 && labels.contains("Sponsored") &&
		          labels.contains("Skip Ad") && labels.contains("Ad 1 of 2"),
		       QString("three labels, and neither Adsorption nor a sentence "
		               "that mentions ads (%1)").arg(labels));
	}

	// **An ad in a frame of its own, from another origin**, which is how ads
	// are served. Asked of the top document alone, the probe could say only
	// that there is a frame; asked in every frame, it says what is in it.
	section("an ad inside a frame from another origin is seen there");
	{
		frame_server srv;
		srv.listen(QHostAddress::LocalHost, 0);
		QEventLoop loaded;
		auto c = QObject::connect(view, &web_view_backend::load_finished,
		                          &loaded, [&loaded](bool) { loaded.quit(); });
		QTimer::singleShot(10000, &loaded, &QEventLoop::quit);
		view->load(QUrl(QString("http://127.0.0.1:%1/top").arg(srv.serverPort())));
		loaded.exec();
		QObject::disconnect(c);
		QEventLoop settle;
		QTimer::singleShot(1500, &settle, &QEventLoop::quit);
		settle.exec();

		QString top_json;
		bool top_got = false;
		view->run_probe(ad_probe::source(), [&](const QString &j) { top_json = j; top_got = true; });
		QList<web_view_backend::frame_answer> answers;
		bool all_got = false;
		view->run_probe_in_frames(ad_probe::source(),
		    [&](const QList<web_view_backend::frame_answer> &a) { answers = a; all_got = true; });
		QEventLoop wait;
		QTimer poll;
		QObject::connect(&poll, &QTimer::timeout, [&] { if (top_got && all_got) wait.quit(); });
		poll.start(50);
		QTimer::singleShot(10000, &wait, &QEventLoop::quit);
		wait.exec();

		ad_probe::findings top;
		ad_probe::parse(top_json, &top);
		check(top.elements.isEmpty(),
		      QString("asked of the top document, the element inside is not seen "
		               "(%1)").arg(ad_probe::describe(top).join(" | ")));
		ad_probe::findings all;
		for (const web_view_backend::frame_answer &a : answers) {
			ad_probe::findings f;
			if (ad_probe::parse(a.answer, &f))
				ad_probe::add(&all, f, a.url.host(), a.main);
		}
		const QString said = ad_probe::describe(all).join(" | ");
		check(answers.size() == 2 && answers.first().main,
		      QString("asked in every frame, both answer, the top first (%1)")
		          .arg(answers.size()));
		check(said.contains("in a frame from localhost: ") && said.contains("ad-banner"),
		      QString("and the ad inside is found, with the frame it is in (%1)").arg(said));
	}

	section("described with the decisive finding first");
	{
		ad_probe::findings f;
		f.elements << "div.ad, 300x250, named \"ad\"";
		f.players << "youtube player #movie_player says it is in an ad (ad-showing)";
		const QStringList d = ad_probe::describe(f);
		check(d.size() == 2 && d.first().startsWith("player: "),
		       QString("a player before an element (%1)").arg(d.join(" | ")));
		check(!ad_probe::parse(QString(), nullptr) &&
		          !ad_probe::parse("{\"x\":1}", nullptr),
		       "and an empty answer or a stranger's JSON is not a finding");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
