// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ai_provider.h"

#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Local-model backend: Ollama on localhost (architecture doc §9.1). Nothing
// leaves the machine, which is why this is the default when it is reachable.
class ollama_provider : public ai_provider {
	Q_OBJECT
public:
	explicit ollama_provider(QObject *parent = nullptr);

	QString name() const override;
	bool available() const override;
	bool is_external() const override { return false; }
	void send(const QString &system_prompt, const QString &user_prompt) override;
	void cancel() override;

	void set_endpoint(const QUrl &url) { m_endpoint = url; }
	void set_model(const QString &model) { m_model = model; }
	QUrl    endpoint() const { return m_endpoint; }
	QString model() const { return m_model; }

	// Probes the local server and remembers the answer for available().
	// Asynchronous: probe_finished() follows, and available() is stale until
	// it does.
	void probe();

	// Probe and wait for the answer, up to the probe timeout.
	//
	// Deliberately blocking, which is the right trade here. The backend choice
	// cannot be deferred — something has to be asked *now* — and getting it
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
	// in full every time a dialog that needs a backend is opened — so the
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
	bool    m_reachable = false;
	int     m_probe_timeout = 2500;   // milliseconds
};
