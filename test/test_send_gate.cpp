// Send is offered only when the model behind it exists.
//
// The dialogs used to show "Local model (Ollama, llama3 -- not installed)" in
// the header with the Send button enabled beside it. That label was itself the
// fix for an earlier round of the same problem -- the reorganizer announced a
// model that was not there and the first anyone knew was a failed request
// after pressing Send -- so the warning got written and the button that
// produces the failure was left alone.
//
// **A fake Ollama rather than a real one**, because the interesting case is a
// server that answers with a model list not containing the configured model,
// and that is not a state a running install can be asked to be in on demand.
// It also keeps this in `make test` and in CI, where `test_probe` (which wants
// a real server) cannot go.
#include "ai_provider.h"
#include "ollama_provider.h"

#include <QApplication>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

// Answers /api/tags with whatever model list it was given.
class fake_ollama : public QTcpServer {
public:
	QByteArrayList names;

	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, this, [this, s] {
			s->readAll();
			QByteArrayList entries;
			for (const QByteArray &n : names)
				entries << "{\"name\":\"" + n + "\"}";
			const QByteArray body = "{\"models\":[" + entries.join(',') + "]}";
			QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
			head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
			head += "Connection: close\r\n\r\n";
			s->write(head + body);
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	fake_ollama server;
	if (!server.listen(QHostAddress::LocalHost)) {
		std::printf("could not listen: %s\n", qPrintable(server.errorString()));
		return 1;
	}
	const QUrl endpoint(QString("http://127.0.0.1:%1").arg(server.serverPort()));

	section("a server that has the model");
	{
		server.names = { "llama3", "mistral" };
		ollama_provider p;
		p.set_endpoint(endpoint);
		p.set_model("llama3");
		check(p.probe_now(), "reachable");
		QString why;
		check(p.ready(&why), "ready(), so Send is offered");
		check(why.isEmpty(), "with nothing to explain");
	}

	section("a server that does not have it");
	{
		server.names = { "mistral", "phi3" };
		ollama_provider p;
		p.set_endpoint(endpoint);
		p.set_model("llama3");
		check(p.probe_now(), "still reachable -- this is the case that fooled us");
		check(p.available(), "available() is true: the backend is fine");
		QString why;
		check(!p.ready(&why),
		      "ready() is false: the backend being fine is not the question");
		check(why.contains("llama3"),
		      "and says which model, since the fix is to pull that one");
		check(p.name().contains("not installed"),
		      "the header said so all along; now the button agrees");
	}

	section("nothing listening");
	{
		ollama_provider p;
		p.set_endpoint(QUrl("http://127.0.0.1:9"));
		p.set_model("llama3");
		check(!p.probe_now(), "unreachable");
		QString why;
		check(!p.ready(&why), "not ready");
		check(!why.isEmpty(), "with a reason that is about the server, not the model");
	}

	section("an unprobed provider is not a missing model");
	{
		// The guard `name()` already keeps: an empty list means nobody
		// has asked yet. Reading it as "not installed" would grey out Send on a
		// working setup, which is the same guess-dressed-as-fact in reverse.
		ollama_provider p;
		p.set_endpoint(endpoint);
		p.set_model("llama3");
		check(!p.ready(), "before a probe it is not ready, because it is not reachable");
		server.names = {};                      // answers, but lists nothing
		check(p.probe_now(), "now reachable, with an empty list");
		QString why;
		check(p.ready(&why),
		      "and ready: an empty list is an unanswered question, not a no");
	}

	section("gate_send does both halves");
	{
		server.names = { "mistral" };
		ollama_provider p;
		p.set_endpoint(endpoint);
		p.set_model("llama3");
		p.probe_now();
		QPushButton send("&Send");
		gate_send(&send, &p);
		check(!send.isEnabled(), "the button is disabled");
		check(send.toolTip().contains("llama3"),
		      "and carries the reason, where the question gets asked of it");

		server.names = { "llama3" };
		p.probe_now();
		gate_send(&send, &p);
		check(send.isEnabled(), "and comes back when the model does");
		check(send.toolTip().isEmpty(), "with the stale reason cleared");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
