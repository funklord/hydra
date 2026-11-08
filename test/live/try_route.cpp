// A download a page starts, while downloads have a route of their own.
//
// Chromium's network cannot be bound, so the shell drops the engine's copy and
// fetches the file again by the route. This reads that off the server and the
// job list, through the real shell: what the second request carried, which
// source took the job, and the two cases that must not be fetched again -- a
// `blob:`, which has no network, and a form, which cannot be repeated.
//
// The route is bound to `lo` here, so that nothing needs a tunnel: where the
// bytes leave is `test_route`'s question, and this one's is who fetches them.
//
// **The unbound click is the control.** Without it, a page that never asked for
// a download would read as a download correctly withheld from the engine.
#include "shell_fixture.h"

#include "download_manager.h"
#include "download_route.h"

#include <QApplication>
#include <QFile>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QMouseEvent>

using namespace shell;

static const QByteArray k_file(5000, 'f');

class page_server : public QTcpServer {
public:
	QList<QByteArray> asked;   // each request's whole head

	int count(const QByteArray &start) const {
		int n = 0;
		for (const QByteArray &head : asked)
			n += head.startsWith(start);
		return n;
	}
	QByteArray last(const QByteArray &start) const {
		for (qsizetype i = asked.size() - 1; i >= 0; --i)
			if (asked[i].startsWith(start))
				return asked[i];
		return {};
	}

protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray head = s->readAll();
			if (!head.contains("\r\n\r\n"))
				return;
			asked << head;
			QByteArray r;
			if (head.startsWith("GET /page")) {
				const QByteArray body =
				  "<!doctype html><title>routed</title>"
				  "<a id=control href=/control.bin>c</a>"
				  "<a id=file href=/file.bin>f</a>"
				  "<form id=form method=post action=/form.bin>"
				  "<input name=a value=1></form>";
				r = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
				    "Set-Cookie: who=me; Path=/\r\nContent-Length: " +
				    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
			} else if (head.startsWith("GET /favicon")) {
				r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
				    "Connection: close\r\n\r\n";
			} else {
				r = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
				    "Content-Disposition: attachment\r\nContent-Length: " +
				    QByteArray::number(k_file.size()) + "\r\nConnection: close\r\n\r\n" +
				    k_file;
			}
			s->write(r);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	const QString saved_in = home.path() + "/Downloads";
	QDir().mkpath(saved_in);
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	page_server web;
	if (!web.listen(QHostAddress::LocalHost, 0)) {
		std::printf("no local server\n");
		return 1;
	}
	const QString origin = QString("http://127.0.0.1:%1").arg(web.serverPort());

	fixture fx(home.path() + "/out");
	auto *manager = fx.window.findChild<download_manager *>();
	check(manager, "the window has a download manager");
	manager->set_directory(saved_in);
	check(fx.open_tab(0, "one.html"), "a tab is open");

	fx.address->setText(origin + "/page");
	emit fx.address->returnPressed();
	check(wait_for(fx.address, "/page"), "the page loads");
	fx.wait_idle();
	spin(800);

	QWebEngineView *shown = nullptr;
	for (QWebEngineView *v : fx.window.findChildren<QWebEngineView *>())
		if (v->isVisible())
			shown = v;
	check(shown, "and is on screen");
	if (!shown)
		return report();
	const auto run = [&shown](const QString &js) {
		shown->page()->runJavaScript(js);
	};
	const auto job_for = [manager](const QString &path) {
		const QList<download_job> &jobs = manager->jobs();
		for (const download_job &j : jobs)
			if (j.url.path() == path)
				return j;
		return download_job();
	};

	section("unbound: the engine keeps its own download");
	run("document.getElementById('control').click()");
	for (int i = 0; i < 40 && job_for("/control.bin").source_id.isEmpty(); ++i)
		spin(100);
	check(job_for("/control.bin").source_id == "engine",
	      QString("the engine runs it (%1)").arg(job_for("/control.bin").source_id));
	spin(600);
	check(web.count("GET /control.bin") == 1, "having asked once");

	download_route::shared().set_interface_name("lo");

	section("bound: the file is fetched again by the route");
	run("document.getElementById('file').click()");
	for (int i = 0; i < 60 && web.count("GET /file.bin") < 2; ++i)
		spin(100);
	spin(800);
	const download_job file = job_for("/file.bin");
	check(file.source_id == "http",
	      QString("the HTTP source runs it (%1)").arg(file.source_id));
	check(web.count("GET /file.bin") == 2,
	      QString("the engine's request, then the route's (%1)")
	        .arg(web.count("GET /file.bin")));
	const QByteArray again = web.last("GET /file.bin");
	check(again.contains("who=me"), "the second carries the session's cookie");
	check(again.toLower().contains("referer: " + origin.toUtf8() + "/page"),
	      "and the page as its referer");
	check(QDir(saved_in).entryList(QDir::Files).contains("file.bin"),
	      QString("and the file is saved (%1)")
	        .arg(QDir(saved_in).entryList(QDir::Files).join(' ')));

	section("bound: a blob has no network, and stays with the engine");
	run("const a = document.createElement('a');"
	    "a.href = URL.createObjectURL(new Blob(['made here']));"
	    "a.download = 'made.txt'; document.body.append(a); a.click();");
	for (int i = 0; i < 40 && !QFile::exists(saved_in + "/made.txt"); ++i)
		spin(100);
	check(QFile::exists(saved_in + "/made.txt"), "it is saved all the same");

	section("bound: a form cannot be sent twice");
	run("document.getElementById('form').submit()");
	for (int i = 0; i < 40 && !web.count("POST /form.bin"); ++i)
		spin(100);
	spin(1000);
	check(web.count("POST /form.bin") == 1, "it went once, by the engine");
	check(web.count("GET /form.bin") == 0, "and was not asked for again");
	check(job_for("/form.bin").source_id.isEmpty(), "no job was made of it");
	check(!QDir(saved_in).entryList(QDir::Files).join(' ').contains("form"),
	      "nothing was saved");
	QStatusBar *bar = fx.window.findChild<QStatusBar *>();
	check(bar && bar->currentMessage().contains("from a form"),
	      QString("and the status bar says so (%1)")
	        .arg(bar ? bar->currentMessage() : QString()));

	download_route::shared().set_interface_name(QString());

	// **A link that is not a page goes to the desktop's own handler.** Android's
	// equivalent ended at "Nothing here can open" for months while its record said
	// otherwise, so this one is read off a stand-in xdg-open rather than assumed.
	section("a mailto: link reaches the desktop's handler");
	{
		QTemporaryDir bin;
		const QString log = bin.filePath("opened");
		QFile opener(bin.filePath("xdg-open"));
		opener.open(QIODevice::WriteOnly);
		opener.write(("#!/bin/sh\necho \"$1\" >> '" + log + "'\n").toUtf8());
		opener.setPermissions(opener.permissions() | QFile::ExeOwner);
		opener.close();
		qputenv("PATH", bin.path().toLocal8Bit() + ":" + qgetenv("PATH"));
		// A page of its own: the one above has had a form sent from it.
		fx.address->setText(origin + "/page?again");
		emit fx.address->returnPressed();
		check(wait_for(fx.address, "/page?again"), "a fresh page loads");
		fx.wait_idle();
		spin(800);
		for (QWebEngineView *v : fx.window.findChildren<QWebEngineView *>())
			if (v->isVisible())
				shown = v;
		run("const a = document.createElement('a'); a.href = 'mailto:someone@example.test';"
		    "a.id = 'mail'; a.textContent = 'mail'; a.style = 'display:block;height:200px';"
		    "document.body.prepend(a);");
		spin(500);
		// Posted the way the other drivers click, which the engine takes as a tap.
		QWidget *target = shown->focusProxy() ? shown->focusProxy() : shown;
		const auto click = [target] {
			const QPoint at(40, 40);
			QMouseEvent press(QEvent::MouseButtonPress, at, target->mapToGlobal(at),
			                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QMouseEvent release(QEvent::MouseButtonRelease, at, target->mapToGlobal(at),
			                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(target, &press);
			QApplication::sendEvent(target, &release);
		};
		click();
		for (int i = 0; i < 40 && !QFile::exists(log); ++i)
			spin(100);
		QFile got(log);
		got.open(QIODevice::ReadOnly);
		const QString said = QString::fromUtf8(got.readAll()).trimmed();
		QStatusBar *status = fx.window.findChild<QStatusBar *>();
		check(said == "mailto:someone@example.test",
		      QString("xdg-open is handed the address (%1)").arg(said.isEmpty() ? "nothing" : said));
		check(status && !status->currentMessage().contains("could not be loaded"),
		      QString("and the page is not said to have failed (%1)")
		        .arg(status ? status->currentMessage() : QString()));

		// The control: the same click on a page link must navigate, or the click
		// is what failed and not the handoff.
		run("document.getElementById('mail').href = '/control-page';");
		spin(300);
		click();
		for (int i = 0; i < 40 && !web.count("GET /control-page"); ++i)
			spin(100);
		check(web.count("GET /control-page") == 1, "the same click on a page link navigates");
	}
	return report();
}
