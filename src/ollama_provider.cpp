// SPDX-License-Identifier: GPL-3.0-or-later
#include "ollama_provider.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

ollama_provider::ollama_provider(QObject *parent) : ai_provider(parent) {
	m_net = new QNetworkAccessManager(this);
}

QString ollama_provider::name() const {
	return QString("Local model (Ollama, %1)").arg(m_model);
}

bool ollama_provider::available() const {
	return m_reachable;
}

void ollama_provider::probe() {
	QNetworkRequest req(m_endpoint.resolved(QUrl("/api/tags")));
	QNetworkReply *reply = m_net->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_reachable = (reply->error() == QNetworkReply::NoError);
		reply->deleteLater();
	});
	// Don't let a dead localhost hang the probe.
	QTimer::singleShot(2000, reply, [reply] {
		if (reply->isRunning())
			reply->abort();
	});
}

void ollama_provider::send(const QString &system_prompt, const QString &user_prompt) {
	cancel();

	QJsonObject body;
	body.insert("model",  m_model);
	body.insert("system", system_prompt);
	body.insert("prompt", user_prompt);
	body.insert("stream", false);

	QNetworkRequest req(m_endpoint.resolved(QUrl("/api/generate")));
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			emit failed(QString("Ollama request failed: %1").arg(reply->errorString()));
			return;
		}
		const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
		const QString text = o.value("response").toString();
		if (text.isEmpty())
			emit failed("Ollama returned an empty response.");
		else
			emit finished(text);
	});
}

void ollama_provider::cancel() {
	if (m_reply && m_reply->isRunning())
		m_reply->abort();
}
