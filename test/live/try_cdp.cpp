// Whether this browser can set its whole outward identity itself, through
// Chromium's emulation layer, and what that costs.
//
//     QT_QPA_PLATFORM=offscreen ./test/build-make/try_cdp [skip|detach|hints|header]
//
// **Why this exists**: the user agent can be set through Qt and the client
// hints cannot -- the headers and `navigator.userAgentData` keep the engine's
// real Chrome 122 while the string claims 140, and a checker that compares
// them sees two browsers at once. The holder, 2026-10-17: an inability to
// change the whole outward surface undermines the purpose of this browser.
//
// **The route**: Qt attaches a DevTools front end to a page with
// `setDevToolsPage`, and that front end -- a page of its own, in this process
// -- sends protocol commands to the inspected page with
// `InspectorFrontendHost.sendMessageToBackend`. Driving it from here reaches
// `Emulation.setUserAgentOverride` with metadata, which Chromium applies to
// the headers and the page alike. Measured to work 2026-10-17.
//
// **Routes that do not**: `--remote-debugging-pipe` is named in Qt's library
// and does not answer through `QTWEBENGINE_CHROMIUM_FLAGS`; a debugging *port*
// would answer and is refused on principle, since any local process -- the
// other account on this machine included -- could then drive the browser.
//
// Measured here, with "current Chrome on Linux" as the identity, as the holder
// chose: what each surface reports (headers, page, a worker, a cross-origin
// frame, after navigating elsewhere), whether the attached front end can be
// seen from the page or freezes it on `debugger;`, and the memory a front end
// costs a tab.
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWebEngineClientHints>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QWidget>
#include <cstdio>
#include <unistd.h>

static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}
template <typename P> static bool until(P pred, int ms) {
	for (int i = 0; i < ms / 50 && !pred(); ++i)
		spin(50);
	return pred();
}

// The resident memory of this process and every process below it, in MB:
// the renderers a front end adds are children, not this process.
static double tree_rss_mb() {
	QMap<int, int> parent;
	QMap<int, long> rss;
	for (const QString &d : QDir("/proc").entryList(QDir::Dirs)) {
		bool ok = false;
		const int pid = d.toInt(&ok);
		if (!ok)
			continue;
		QFile st("/proc/" + d + "/status");
		if (!st.open(QIODevice::ReadOnly))
			continue;
		for (const QByteArray &line : st.readAll().split('\n')) {
			if (line.startsWith("PPid:"))
				parent[pid] = line.mid(5).trimmed().toInt();
			else if (line.startsWith("VmRSS:"))
				rss[pid] = line.mid(6).trimmed().split(' ').value(0).toLong();
		}
	}
	const int me = int(getpid());
	long kb = 0;
	for (auto it = rss.cbegin(); it != rss.cend(); ++it) {
		int p = it.key();
		for (int hop = 0; hop < 8 && p > 1; ++hop) {
			if (p == me) {
				kb += it.value();
				break;
			}
			p = parent.value(p, 0);
		}
	}
	return kb / 1024.0;
}

// `/page` on one origin reports, into its own root element, what it sees: the
// brands, a worker's brands, whether a console getter fired (a front end
// listening), and how long `debugger;` held it. `/frame` does the same for a
// frame from another origin, and the server keeps the client hint each path
// was sent.
class page_server : public QTcpServer {
public:
	QMap<QByteArray, QByteArray> hint;   // path -> Sec-CH-UA as received
	QList<QByteArray> reports;           // what pages sent back, in order
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray head = s->readAll();
			const QByteArray path = head.split(' ').value(1);
			if (path.startsWith("/report?"))
				reports << QByteArray::fromPercentEncoding(path.mid(8));
			for (const QByteArray &line : head.split('\n'))
				if (line.toLower().startsWith("sec-ch-ua:"))
					hint[path.split('?').value(0)] = line.mid(10).trimmed();
			QByteArray report =
			  "<script>"
			  "function brands(){var d=navigator.userAgentData;return d?d.brands.map("
			  "function(b){return b.brand+' '+b.version;}).join(','):'none';}"
			  "document.documentElement.dataset.brands=brands();"
			  "try{var w=new Worker(URL.createObjectURL(new Blob(['postMessage("
			  "navigator.userAgentData?navigator.userAgentData.brands.map(function(b)"
			  "{return b.brand+\" \"+b.version;}).join(\",\"):\"none\")'])));"
			  "w.onmessage=function(e){document.documentElement.dataset.worker=e.data;};}"
			  "catch(x){document.documentElement.dataset.worker='error '+x;}"
			  "var hit=false,e=new Error('x');Object.defineProperty(e,'stack',"
			  "{get:function(){hit=true;return '';}});console.log(e);"
			  "setTimeout(function(){document.documentElement.dataset.getter=hit;"
			  "var t=performance.now();DEBUGGER"
			  "document.documentElement.dataset.debuggerms=Math.round(performance.now()-t);"
			  "},300);"
			  // Reported to the server too, since an attached front end blocks
			  // the isolated world: a page frozen on `debugger;` reports
			  // nothing after it.
			  "setTimeout(function(){fetch('/report?'+encodeURIComponent(location.pathname+"
			  "location.search+' '+JSON.stringify(document.documentElement.dataset)));},1200);"
			  "</script>";
			// `nodebug` leaves the statement out, to tell a page frozen on it
			// from a front end that blocks the probe.
			report.replace("DEBUGGER", path.contains("nodebug") ? "" : "debugger;");
			QByteArray body;
			if (path.startsWith("/page"))
				body = "<!doctype html><title>page</title>" + report +
				       "<iframe src=\"http://localhost:" + QByteArray::number(serverPort()) +
				       "/frame\"></iframe>";
			else
				body = "<!doctype html><title>frame</title>" + report;
			s->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nAccept-CH: "
			         "Sec-CH-UA-Full-Version-List\r\nContent-Length: " +
			         QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

// Overwrites the brand hint on every request, to see whether Chromium lets an
// interceptor set it or puts its own back afterwards.
class brand_header : public QWebEngineUrlRequestInterceptor {
public:
	void interceptRequest(QWebEngineUrlRequestInfo &info) override {
		info.setHttpHeader("Sec-CH-UA",
		    "\"Chromium\";v=\"140\", \"Not=A?Brand\";v=\"24\", \"Google Chrome\";v=\"140\"");
	}
};

// "Current Chrome on Linux", as the holder chose: the brands and versions
// Chrome 140 sends, with the user agent string the factory already writes.
static QJsonObject identity(const QString &ua) {
	QJsonArray brands, full;
	auto add = [&](const char *brand, const char *major, const char *version) {
		brands.append(QJsonObject{ { "brand", brand }, { "version", major } });
		full.append(QJsonObject{ { "brand", brand }, { "version", version } });
	};
	add("Chromium", "140", "140.0.7339.80");
	add("Not=A?Brand", "24", "24.0.0.0");
	add("Google Chrome", "140", "140.0.7339.80");
	QJsonObject meta;
	meta.insert("brands", brands);
	meta.insert("fullVersionList", full);
	meta.insert("platform", "Linux");
	meta.insert("platformVersion", "");
	meta.insert("architecture", "x86");
	meta.insert("bitness", "64");
	meta.insert("model", "");
	meta.insert("mobile", false);
	return QJsonObject{ { "userAgent", ua }, { "userAgentMetadata", meta } };
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	page_server srv;
	srv.listen(QHostAddress::LocalHost, 0);
	const QString origin = QString("http://127.0.0.1:%1").arg(srv.serverPort());

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	// `hints`: Qt 6.8's own client-hints API on the profile, before anything
	// loads and with no front end at all -- the route to prefer if it holds.
	const bool header_mode = argc > 1 && QByteArray(argv[1]) == "header";
	brand_header rewrite;
	if (header_mode) {
		factory.profile()->setUrlRequestInterceptor(&rewrite);
		std::printf("brand header rewritten by an interceptor\n");
	}
	const bool hints_mode = (argc > 1 && QByteArray(argv[1]) == "hints") || header_mode;
	if (hints_mode) {
		QWebEngineClientHints *h = factory.profile()->clientHints();
		h->setFullVersion("140.0.7339.80");
		h->setFullVersionList(QVariantMap{ { "Chromium", "140.0.7339.80" },
		                                   { "Not=A?Brand", "24.0.0.0" },
		                                   { "Google Chrome", "140.0.7339.80" } });
		h->setPlatform("Linux");
		h->setPlatformVersion("");
		h->setArch("x86");
		h->setBitness("64");
		h->setModel("");
		h->setIsMobile(false);
		std::printf("client hints set through the profile\n");
	}
	QWidget host;
	host.resize(900, 700);
	QList<web_view_backend *> views;
	for (int i = 0; i < 3; ++i)
		views << factory.create_view(&host);
	host.show();

	auto load = [&](web_view_backend *v, const QString &url) {
		bool done = false;
		auto c = QObject::connect(v, &web_view_backend::load_finished,
		                          [&](bool) { done = true; });
		v->load(QUrl(url));
		until([&] { return done; }, 10000);
		QObject::disconnect(c);
		spin(1500);
	};
	auto read_frames = [&](web_view_backend *v) {
		QList<web_view_backend::frame_answer> got;
		bool have = false;
		v->run_probe_in_frames("JSON.stringify(document.documentElement.dataset)",
		    [&](const QList<web_view_backend::frame_answer> &a) { got = a; have = true; });
		until([&] { return have; }, 8000);
		for (const web_view_backend::frame_answer &a : got)
			std::printf("  %-5s %-16s %s\n", a.main ? "top" : "frame",
			            qPrintable(a.url.host()), qPrintable(a.answer));
		std::printf("  headers: /page %s | /frame %s\n", srv.hint.value("/page").constData(),
		            srv.hint.value("/frame").constData());
		spin(1500);
		for (const QByteArray &r : srv.reports)
			std::printf("  report: %s\n", r.constData());
		srv.reports.clear();
	};

	for (web_view_backend *v : views)
		load(v, origin + "/page");
	std::printf("--- before, as the engine sends it\n");
	read_frames(views.first());
	if (hints_mode) {
		load(views.first(), QString("http://localhost:%1/page?elsewhere").arg(srv.serverPort()));
		std::printf("--- after navigating to another origin\n");
		read_frames(views.first());
		std::printf("--- memory, three tabs: %.0f MB\n", tree_rss_mb());
		return 0;
	}
	const double before_mb = tree_rss_mb();

	// The front ends: one per tab, each attached, and the identity sent.
	QList<QWebEnginePage *> inspected;
	for (QWebEngineView *w : host.findChildren<QWebEngineView *>())
		inspected << w->page();
	// A protocol command as a JavaScript string literal, which a JSON string is.
	auto command = [](const QString &method, const QJsonObject &params, int id) {
		const QJsonObject m{ { "id", id }, { "method", method }, { "params", params } };
		const QString text = QString::fromUtf8(QJsonDocument(m).toJson(QJsonDocument::Compact));
		const QByteArray quoted = QJsonDocument(QJsonArray{ text }).toJson(QJsonDocument::Compact);
		return QString::fromUtf8(quoted.mid(1, quoted.size() - 2));
	};
	const QString literal = command("Emulation.setUserAgentOverride",
	                                identity(factory.profile()->httpUserAgent()), 900001);
	// `skip`: also tell the debugger never to pause, so `debugger;` in a page
	// cannot hold it. `detach`: take the front ends away once the identity is
	// sent, to see whether it outlives them -- which is what memory needs.
	const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
	std::printf("mode: %s\n", mode.isEmpty() ? "plain" : qPrintable(mode));
	QList<QWebEnginePage *> fronts;
	for (QWebEnginePage *p : inspected) {
		auto *front = new QWebEnginePage(factory.profile(), &host);
		bool ready = false;
		QObject::connect(front, &QWebEnginePage::loadFinished, [&](bool) { ready = true; });
		p->setDevToolsPage(front);
		until([&] { return ready; }, 15000);
		spin(1500);
		front->runJavaScript("InspectorFrontendHost.sendMessageToBackend(" + literal + ")");
		if (mode == "skip") {
			front->runJavaScript("InspectorFrontendHost.sendMessageToBackend(" +
			                     command("Debugger.enable", {}, 900002) + ")");
			front->runJavaScript("InspectorFrontendHost.sendMessageToBackend(" +
			                     command("Debugger.setSkipAllPauses",
			                             QJsonObject{ { "skip", true } }, 900003) + ")");
		}
		fronts << front;
	}
	spin(2000);
	if (mode == "detach") {
		for (int i = 0; i < inspected.size(); ++i) {
			inspected[i]->setDevToolsPage(nullptr);
			delete fronts[i];
		}
		spin(3000);
		std::printf("front ends removed\n");
	}
	const double after_mb = tree_rss_mb();
	std::printf("--- memory, three tabs: %.0f MB before front ends, %.0f MB after "
	            "(%.0f MB a tab)\n", before_mb, after_mb, (after_mb - before_mb) / 3);

	for (web_view_backend *v : views)
		load(v, origin + "/page?nodebug");
	std::printf("--- after, reloaded, no debugger statement\n");
	read_frames(views.first());
	for (web_view_backend *v : views)
		load(v, origin + "/page?again");
	std::printf("--- after, reloaded\n");
	read_frames(views.first());

	load(views.first(), QString("http://localhost:%1/page?elsewhere").arg(srv.serverPort()));
	std::printf("--- after navigating to another origin\n");
	read_frames(views.first());
	return 0;
}
