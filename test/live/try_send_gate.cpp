// Photograph Send greyed out when the model is not installed.
//
// `test_send_gate` proves the predicate and proves `gate_send()` moves a bare
// button. Neither reaches the thing that was actually wrong, which was visible
// rather than logical: the filter window's header read "Local model (Ollama,
// llama3 -- not installed)" and the Send button sat next to it looking
// perfectly usable. So this one builds the real dialog and takes its picture.
//
// **The state is reached honestly**, through the same door the shell uses. A
// fake Ollama answers `/api/tags` with a model list that does not contain the
// configured model -- which is exactly what `choose_ai()` hands the dialog in
// `local_only` mode when the server is up and nobody has pulled anything: it
// probes, sees a reachable server, and returns the provider.
//
// **`QWidget::grab()`, never `import`** -- it renders in-process and does not
// touch the X server. `import` grabs the X pointer and keeps the grab when it
// cannot do what it was asked, which froze this desktop once already.
#include "ai_provider.h"
#include "filter_dialog.h"
#include "filter_list.h"
#include "filter_signals.h"
#include "request_filter.h"
#include "ollama_provider.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QPushButton>
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

// The dialog owns its buttons; find Send by the text it was given.
static QPushButton *send_button(QDialog *dlg) {
	for (QPushButton *b : dlg->findChildren<QPushButton *>())
		if (b->text().contains("Send"))
			return b;
	return nullptr;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	// The only one of the thirty fixed `/tmp/hydra-*` paths in these drivers
	// that could not be redirected. Twenty-nine honour HYDRA_TEST_OUT, either
	// directly or through `shell::fixture`, and one uses HYDRA_TEST_CONFIG;
	// this one took the shared path unconditionally, so a second account on
	// the machine wrote into the first account's directory or failed to.
	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"))
	                        : QString("/tmp/hydra-send-gate");
	QDir().mkpath(out);
	int shots = 0;
	auto save = [&](QWidget *w, const char *name) {
		const QString path = QString("%1/%2-%3.png")
		                         .arg(out).arg(shots, 2, 10, QChar('0')).arg(name);
		if (w->grab().save(path)) {
			std::printf("  shot  %s\n", qPrintable(path));
			++shots;
		} else {
			std::printf("  %-24s could not be grabbed\n", name);
		}
	};

	fake_ollama server;
	if (!server.listen(QHostAddress::LocalHost)) {
		std::printf("could not listen: %s\n", qPrintable(server.errorString()));
		return 1;
	}

	filter_signals signals_source;
	filter_list    list;

	// **Give the dialog something to send, or this proves nothing.** The first
	// run of this driver showed Send greyed in both cases and reported a pass:
	// with no requests recorded, `suspects_for()` is empty and the button was
	// already disabled for having nothing to ask about. A green tick over an
	// empty evidence list is the vacuous pass this project keeps guarding
	// against, and it caught itself here.
	//
	// Third-party and ad-shaped, because only a request that *got through*
	// counts as evidence of a gap -- a blocked one is the system working.
	static const char *const k_evidence[] = {
		"http://ads.invalid/pagead/banner.js",
		"http://tracker.invalid/track?id=1",
	};
	for (const char *u : k_evidence) {
		request_context ctx;
		ctx.site_host    = "example.invalid";
		ctx.url          = QUrl(u);
		ctx.request_host = ctx.url.host();
		signals_source.on_request(ctx, request_decision{});
	}
	if (signals_source.suspects_for("example.invalid").isEmpty()) {
		std::printf("the fixture recorded no suspects; the gate below would "
		             "pass for the wrong reason\n");
		return 1;
	}
	std::printf("%d suspect(s) recorded, so Send has something to ask about\n",
	             int(signals_source.suspects_for("example.invalid").size()));

	auto shoot = [&](const char *what, const char *label) {
		ollama_provider ai;
		ai.set_endpoint(QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())));
		ai.set_model("llama3");
		ai.probe_now();

		filter_dialog dlg(&signals_source, &list, &ai, "example.invalid");
		dlg.resize(760, 560);
		dlg.show();
		QApplication::processEvents();

		QPushButton *send = send_button(&dlg);
		std::printf("  header reads: %s\n", qPrintable(ai.name()));
		if (!send) {
			check(false, "the dialog has a Send button");
			return;
		}
		check(send->isEnabled() == QString(what).startsWith("enabled"),
		      QString("Send is %1").arg(what));
		if (!send->isEnabled())
			check(!send->toolTip().isEmpty(),
			      QString("and explains itself: \"%1\"").arg(send->toolTip()));
		save(&dlg, label);
	};

	section("the model is not installed");
	server.names = { "mistral", "phi3" };
	shoot("disabled", "not-installed");

	section("and when it is");
	server.names = { "llama3" };
	shoot("enabled", "installed");

	std::printf("\n%d image(s) in %s\n", shots, qPrintable(out));
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
