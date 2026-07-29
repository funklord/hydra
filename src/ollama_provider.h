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

	// Probes the local server and remembers the answer for available().
	void probe();

private:
	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QUrl    m_endpoint = QUrl("http://localhost:11434");
	QString m_model    = "llama3";
	bool    m_reachable = false;
};
