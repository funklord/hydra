// The Annoyed window's model loop, end to end through the real shell: the
// real button, the real window, the real Ollama provider talking to a
// scripted local server, a real tab, real trial rules, and a real reload.
//
// **Two runs, for the two promises.** One is the promise that matters most:
// a trial nobody keeps does not outlive the window. The model tries a rule
// and gives up, the window is closed, and the ad is back. It runs first,
// because the other keeps a rule and a kept rule stays. The other is the loop
// doing its job: the model looks, tries a rule, asks whether it looks right,
// and finishes; Keep puts the rule in the person's own list and the page
// stays clean after it.
//
// Offline and contained: the page and the model are local servers, and HOME
// and the XDG directories are a temporary directory, so neither the real
// profile, nor the real settings, nor a real model is touched.
#include "ad_probe.h"
#include "annoyed_dialog.h"
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>
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

// A page with one visible ad slot and some content, and a scripted Ollama on
// the same server: `/api/tags` names the model, `/api/generate` answers from
// the script in order.
class local_server : public QTcpServer {
public:
	QStringList script;
	QStringList prompts;
	int pages = 0;
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		auto buf = std::make_shared<QByteArray>();
		connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
			*buf += s->readAll();
			const int end = buf->indexOf("\r\n\r\n");
			if (end < 0)
				return;
			const QByteArray head = buf->left(end);
			const int at = head.toLower().indexOf("content-length:");
			const int want = at < 0 ? 0
			  : head.mid(at + 15, head.indexOf('\r', at) - at - 15).trimmed().toInt();
			if (buf->size() < end + 4 + want)
				return;
			const QByteArray body = buf->mid(end + 4, want);
			reply(s, head, body);
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
private:
	void send(QTcpSocket *s, const QByteArray &type, const QByteArray &body) {
		s->write("HTTP/1.1 200 OK\r\nContent-Type: " + type +
		         "\r\nContent-Length: " + QByteArray::number(body.size()) +
		         "\r\nConnection: close\r\n\r\n" + body);
		s->disconnectFromHost();
	}
	void reply(QTcpSocket *s, const QByteArray &head, const QByteArray &body) {
		if (head.startsWith("GET /api/tags")) {
			send(s, "application/json", "{\"models\":[{\"name\":\"llama3\"}]}");
		} else if (head.startsWith("POST /api/generate")) {
			prompts << QJsonDocument::fromJson(body).object()
			               .value("prompt").toString();
			const QString r = script.isEmpty()
			  ? QStringLiteral("{\"action\":\"done\",\"solved\":false}")
			  : script.takeFirst();
			QJsonObject o;
			o.insert("response", r);
			send(s, "application/json",
			     QJsonDocument(o).toJson(QJsonDocument::Compact));
		} else {
			++pages;
			send(s, "text/html",
			     "<!doctype html><html><body>"
			     "<div id=\"ad-slot\" class=\"ad-banner\" "
			     "style=\"width:300px;height:250px\">AD</div>"
			     "<p style=\"width:400px;height:60px\">The story.</p>"
			     "</body></html>");
		}
	}
};

static QAction *toolbar_action(QWidget *w, const QString &text) {
	for (QToolBar *bar : w->findChildren<QToolBar *>())
		for (QAction *a : bar->actions())
			if (a->text() == text)
				return a;
	return nullptr;
}

static annoyed_dialog *open_window() {
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto *d = qobject_cast<annoyed_dialog *>(w))
			if (d->isVisible())
				return d;
	return nullptr;
}

static QPushButton *button(QWidget *w, const QString &text) {
	for (QPushButton *b : w->findChildren<QPushButton *>())
		if (b->text().remove('&').startsWith(text) && b->isVisible())
			return b;
	return nullptr;
}

static QString log_of(annoyed_dialog *d) {
	QStringList out;
	if (auto *l = d->findChild<QListWidget *>("ai_log"))
		for (int i = 0; i < l->count(); ++i)
			out << l->item(i)->text();
	return out.join('\n');
}

// Waits until `pred` holds or `ms` pass; says which.
template <typename P> static bool until(P pred, int ms) {
	for (int i = 0; i < ms / 100 && !pred(); ++i)
		spin(100);
	return pred();
}

// What the probe sees on the tab now.
static QStringList seen(web_view_backend *view) {
	QStringList out;
	bool got = false;
	view->run_probe(ad_probe::source(), [&](const QString &json) {
		ad_probe::findings f;
		ad_probe::parse(json, &f);
		out = ad_probe::describe(f);
		got = true;
	});
	until([&] { return got; }, 5000);
	return out;
}

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

	local_server srv;
	if (!srv.listen(QHostAddress::LocalHost, 0)) {
		std::printf("no local server\n");
		return 1;
	}
	const QString origin = QString("http://127.0.0.1:%1").arg(srv.serverPort());
	{
		QSettings s(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
		s.setValue("ai/mode", "local_only");
		s.setValue("ai/ollama_endpoint", origin);
		s.setValue("ai/ollama_model", "llama3");
		s.sync();
	}

	const QString out = home.path() + "/profile";
	QDir().mkpath(out);
	const QString tree = out + "/tree.txt";
	{
		QFile tf(tree);
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n"
		                 "  - [a1] unopened | A page | %1/page.html | "
		                 "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n")
		             .arg(origin).toUtf8());
	}

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1200, 900);
	w.show();
	spin(800);
	auto *tree_view = w.findChild<QTreeView *>();
	emit tree_view->activated(
	  tree_view->model()->index(0, 0, tree_view->model()->index(0, 0)));
	spin(3000);
	web_view_backend *view = w.findChild<web_view_backend *>();
	QAction *annoyed = toolbar_action(&w, "Annoyed");
	if (!view || !annoyed) {
		std::printf("no tab or no button\n");
		return 1;
	}
	check(!seen(view).isEmpty(), QString("the ad slot is visible to begin with "
	                                     "(%1)").arg(seen(view).join("; ")));

	section("a trial nobody keeps does not outlive the window");
	{
		// First, so the page starts from the ad showing: the second run
		// keeps a rule, and a kept rule stays.
		srv.script = {
			"{\"action\":\"try\",\"rules\":[\"127.0.0.1###ad-slot\"]}",
			"{\"action\":\"done\",\"solved\":false,\"summary\":\"gave up\"}",
		};
		annoyed->trigger();
		annoyed_dialog *d = nullptr;
		until([&] { return (d = open_window()) != nullptr; }, 8000);
		if (!d)
			return 1;
		if (QPushButton *start = button(d, "Work On It"))
			start->click();
		check(until([&] { return button(d, "Discard") != nullptr; }, 40000),
		       "the run ends with the rule still in trial");
		check(seen(view).isEmpty(), "and while it is, the slot is hidden");
		d->close();
		spin(4000);
		check(!seen(view).isEmpty(),
		       "closing the window without keeping it brings the ad back");
	}

	section("the loop: look, try, ask, done, and Keep");
	{
		srv.prompts.clear();
		srv.script = {
			"{\"action\":\"look\",\"why\":\"see it\"}",
			"{\"action\":\"try\",\"why\":\"hide the slot\","
			  "\"rules\":[\"127.0.0.1###ad-slot\",\"##div\"]}",
			"{\"action\":\"ask\",\"kind\":\"confirm\","
			  "\"question\":\"Does it look right now?\"}",
			"{\"action\":\"done\",\"solved\":true,\"summary\":\"slot hidden\"}",
		};
		annoyed->trigger();
		annoyed_dialog *d = nullptr;
		check(until([&] { return (d = open_window()) != nullptr; }, 8000),
		       "pressing the button opens the window, and it is not modal");
		if (!d)
			return 1;
		QPushButton *start = button(d, "Work On It");
		check(start && start->isEnabled(),
		       "Work On It is offered, the local model being reachable");
		if (start)
			start->click();
		check(until([&] { return button(d, "Yes") != nullptr; }, 40000),
		       "the model's question is put to the person");
		const QStringList during = seen(view);
		check(during.isEmpty(),
		       QString("while in trial the slot is hidden on the tab (%1)")
		           .arg(during.join("; ")));
		if (QPushButton *yes = button(d, "Yes"))
			yes->click();
		check(until([&] { return button(d, "Keep") != nullptr; }, 20000),
		       "it finishes and offers to keep what is in trial");
		const QString log = log_of(d);
		check(log.contains("Turn 1") && log.contains("In trial: 127.0.0.1###ad-slot")
		          && log.contains("Refused: ##div") &&
		          log.contains("nothing ad-like is visible") &&
		          log.contains("Answered: yes") && log.contains("Solved"),
		       "the log says what was asked, tried, refused, seen and answered");
		auto *rules = d->findChild<QListWidget *>("result_rules");
		check(rules && rules->count() == 1 &&
		          rules->item(0)->text() == "127.0.0.1###ad-slot",
		       "only the rule that passed the gates is offered");
		check(srv.prompts.size() == 4 &&
		          srv.prompts.at(2).contains("refused: ##div"),
		       QString("the model was told what was refused (%1 prompts)")
		           .arg(srv.prompts.size()));
		if (QPushButton *keep = button(d, "Keep"))
			keep->click();
		spin(4000);
		QFile own(out + "/filters-ai.txt");
		own.open(QIODevice::ReadOnly);
		check(QString::fromUtf8(own.readAll()).contains("127.0.0.1###ad-slot"),
		       "Keep puts the rule in the person's own list, on disk");
		check(seen(view).isEmpty(),
		       "and the page stays clean under the kept rule, trial gone");
		d->close();
		spin(1500);
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
