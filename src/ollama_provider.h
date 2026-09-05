#pragma once

#include "ai_provider.h"

#include <QPointer>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Local-model backend: Ollama (architecture doc sec 9.1). On loopback nothing
// leaves the machine, which is why this is the default when it is reachable --
// but the endpoint is a free-text setting, so that is a question to ask rather
// than a property to assume. See `endpoint_is_local`.
class ollama_provider : public ai_provider {
	Q_OBJECT
public:
	explicit ollama_provider(QObject *parent = nullptr);

	QString name() const override;
	bool available() const override;
	// **Asks the endpoint, which is the thing the base class actually means.**
	// This returned a flat `false`, so it was answering "is this the Ollama
	// backend" while `ai_provider` asks "does anything leave the machine" --
	// and the endpoint above is a free-text field in Settings. Point it at a
	// box on the LAN and three dialogs went on saying, in bold and directly
	// over the Send button, *"local provider; nothing leaves this machine"*
	// before sending a tab tree over the network.
	bool is_external() const override { return !endpoint_is_local(m_endpoint); }

	// Whether `url` names this machine.
	//
	// Deliberately narrow: `localhost` and anything under `.localhost` (which
	// RFC 6761 reserves for loopback), or a literal address that is loopback.
	// A name resolving to 127.0.0.1 through `/etc/hosts` reads as remote, and
	// that is the direction to be wrong in -- calling a remote host local
	// leaks a payload, while calling a local host remote costs a review step
	// somebody was about to take anyway. It is also the only answer available
	// without a DNS lookup, and a lookup's answer can change between asking
	// and sending.
	static bool endpoint_is_local(const QUrl &url);
	void send(const QString &system_prompt, const QString &user_prompt) override;
	void cancel() override;

	void set_endpoint(const QUrl &url) { m_endpoint = url; }
	void set_model(const QString &model) { m_model = model; }
	QUrl    endpoint() const { return m_endpoint; }
	QString model() const { return m_model; }

	// What the server said it has, from the same `/api/tags` reply the probe
	// already makes. It was being fetched and thrown away: the probe kept one
	// boolean out of a document listing every installed model, so a dialog
	// could announce "Ollama, llama3" while the machine had only qwen -- and
	// the first anyone knew of it was a failed request after pressing Send.
	//
	// Empty until a probe has answered. `has_model` is false in that case too,
	// so a caller must ask `available()` first to tell "not installed" from
	// "not asked yet".
	QStringList models() const { return m_models; }
	bool has_model(const QString &name) const { return m_models.contains(name); }

	// Reachable, and the configured model is one the server actually has.
	//
	// **An unprobed list is not an absent model.** `m_models` is empty before
	// the server has answered, and treating that as "not installed" would
	// disable Send on a perfectly good setup -- the same guess-dressed-as-fact
	// that `name()` is careful to avoid.
	bool ready(QString *reason = nullptr) const override;

	// Probes the local server and remembers the answer for available().
	// Asynchronous: probe_finished() follows, and available() is stale until
	// it does.
	void probe();

	// Probe and wait for the answer, up to the probe timeout.
	//
	// Deliberately blocking, which is the right trade here. The backend choice
	// cannot be deferred -- something has to be asked *now* -- and getting it
	// wrong is not symmetric: treating a running local model as absent sends
	// the payload to an external service when it never had to leave the
	// machine. A user-initiated action may cost a moment to avoid that.
	bool probe_now();

	// How long to wait for the local server before giving up on it.
	//
	// Configurable because the right value depends entirely on where the
	// endpoint points. On loopback the answer arrives in about a millisecond
	// and a short timeout costs nothing. A remote host that *drops* packets
	// rather than refusing gives no answer at all, and then the timeout is paid
	// in full every time a dialog that needs a backend is opened -- so the
	// person who moved the endpoint off localhost is the person who needs to be
	// able to change this.
	void set_probe_timeout(int ms) { m_probe_timeout = qMax(100, ms); }
	int  probe_timeout() const { return m_probe_timeout; }

signals:
	void probe_finished(bool reachable);

public:

private:
	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QUrl    m_endpoint = QUrl("http://localhost:11434");
	QString m_model    = "llama3";
	QStringList m_models;
	bool    m_reachable = false;
	int     m_probe_timeout = 2500;   // milliseconds
};
