// SPDX-License-Identifier: GPL-3.0-or-later
#include "ollama_provider.h"
#include <QJsonArray>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>

ollama_provider::ollama_provider(QObject *parent) : ai_provider(parent) {
	m_net = new QNetworkAccessManager(this);
}

QString ollama_provider::name() const {
	// **Says when the configured model is not there.** Every use of this is a
	// label a person reads -- three dialog banners and an "Asking %1..."
	// status -- so the state belongs in it rather than in three copies of a
	// check. Without this the reorganizer announced "Local model (Ollama,
	// llama3)" on a machine holding only qwen, and the first anyone knew was a
	// failed request after pressing Send.
	//
	// Only when the server has actually answered: `m_models` is empty before a
	// probe, and calling that "not installed" would be a guess dressed as a
	// fact.
	if (m_reachable && !m_models.isEmpty() && !m_models.contains(m_model))
		return QString("Local model (Ollama, %1 \u2014 not installed)").arg(m_model);
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
		m_models.clear();
		if (m_reachable) {
			// The same reply, read rather than discarded.
			const QJsonObject root =
			    QJsonDocument::fromJson(reply->readAll()).object();
			for (const QJsonValue &v : root.value("models").toArray())
				m_models << v.toObject().value("name").toString();
		}
		reply->deleteLater();
		emit probe_finished(m_reachable);
	});
	// Don't let a host that never answers hang the probe.
	QTimer::singleShot(m_probe_timeout, reply, [reply] {
		if (reply->isRunning())
			reply->abort();
	});
}

bool ollama_provider::probe_now() {
	QEventLoop loop;
	connect(this, &ollama_provider::probe_finished, &loop, &QEventLoop::quit);
	// The abort above is what normally ends this, and it produces a real
	// answer rather than a guess. The grace period is only a backstop for the
	// case where the abort itself does not deliver finished() promptly — so
	// the wait cannot outlive the timeout by anything a user would notice.
	QTimer::singleShot(m_probe_timeout + 500, &loop, &QEventLoop::quit);
	probe();
	loop.exec();
	return m_reachable;
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
