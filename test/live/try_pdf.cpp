// A PDF opened in a tab is shown there, and one sent as an attachment is
// still downloaded -- through the real factory and a real engine.
//
// The first half is the one that was broken: Qt's PDF viewer needs plugins
// on, which Qt defaults off, so every PDF in a tab became a silent download
// into ~/Downloads and the tab stayed blank. Found on a bank's e-invoice. The
// second half is the control: turning the viewer on must not swallow a file
// the server asked to have saved.
//
// Offline: a local server, and the download directory, data and cache all
// in a temporary directory, so nothing reaches the real profile or the real
// Downloads folder.
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
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

// The smallest document the viewer accepts: one empty page.
static const char k_pdf[] =
  "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
  "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
  "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\n"
  "trailer<</Root 1 0 R>>\n%%EOF\n";

// `/inline.pdf` as a document, `/attached.pdf` as an attachment.
class pdf_server : public QTcpServer {
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [s] {
			const QByteArray head = s->readAll();
			const bool attach = head.contains("GET /attached.pdf");
			const QByteArray body(k_pdf);
			QByteArray r = "HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\n";
			if (attach)
				r += "Content-Disposition: attachment; filename=\"a.pdf\"\r\n";
			r += "Content-Length: " + QByteArray::number(body.size()) +
			     "\r\nConnection: close\r\n\r\n" + body;
			s->write(r);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

static QString ask(web_view_backend *view, const char *js) {
	QString out;
	bool got = false;
	view->run_probe(QString::fromLatin1(js), [&](const QString &v) {
		out = v;
		got = true;
	});
	for (int i = 0; i < 100 && !got; ++i)
		spin(50);
	return got ? out : QStringLiteral("(no answer)");
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	// **HOME as well, because a `user-dirs.dirs` naming the download
	// directory was not honoured** -- measured: the first run's attachment
	// landed in the real ~/Downloads, and was removed by hand. With HOME
	// here, the download location is under it whatever the desktop says.
	QDir().mkpath(home.path() + "/Downloads");
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	pdf_server srv;
	if (!srv.listen(QHostAddress::LocalHost, 0)) {
		std::printf("no local server\n");
		return 1;
	}
	const QString origin = QString("http://127.0.0.1:%1").arg(srv.serverPort());

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	int downloads = 0;
	factory.set_download_handler([&downloads](const web_view_factory::engine_download &e) {
		if (e.finished)
			++downloads;
	});
	QWidget host;
	host.resize(1000, 800);
	web_view_backend *view = factory.create_view(&host);
	view->widget()->resize(1000, 800);
	host.show();
	view_settings vs;
	view->apply_settings(vs);

	section("a PDF opened in a tab is shown there");
	{
		view->load(QUrl(origin + "/inline.pdf"));
		spin(4000);
		const QString type = ask(view, "document.contentType");
		check(type == "application/pdf",
		       QString("the tab holds the document (%1)").arg(type));
		check(view->url().path() == "/inline.pdf",
		       QString("and stays on it (%1)").arg(view->url().toString()));
		check(downloads == 0,
		       QString("and nothing was downloaded (%1)").arg(downloads));
	}

	section("one sent as an attachment is still saved");
	{
		view->load(QUrl(origin + "/attached.pdf"));
		for (int i = 0; i < 100 && downloads == 0; ++i)
			spin(50);
		check(downloads == 1,
		       QString("it is downloaded (%1)").arg(downloads));
		check(QFile::exists(home.path() + "/Downloads/a.pdf"),
		       "into the download directory, not shown");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
