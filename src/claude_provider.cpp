// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude_provider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>

namespace {

// The API version header is a dated contract, distinct from the model id.
const char *k_api_version = "2023-06-01";
const char *k_endpoint    = "https://api.anthropic.com/v1/messages";

}  // namespace

claude_provider::claude_provider(QObject *parent) : ai_provider(parent) {
	m_net = new QNetworkAccessManager(this);
	m_api_key = QProcessEnvironment::systemEnvironment().value("ANTHROPIC_API_KEY");
}

QString claude_provider::name() const {
	return QString("Claude (%1)").arg(m_model);
}

bool claude_provider::available() const {
	return !m_api_key.isEmpty();
}

void claude_provider::send(const QString &system_prompt, const QString &user_prompt) {
	cancel();

	if (m_api_key.isEmpty()) {
		emit failed("No Anthropic API key. Set ANTHROPIC_API_KEY, or pick the "
		            "local provider.");
		return;
	}

	QJsonObject message;
	message.insert("role", "user");
	message.insert("content", user_prompt);

	QJsonObject body;
	body.insert("model", m_model);
	// Thinking is on by default on this model and counts against max_tokens, so
	// leave real headroom above the size of the outline coming back.
	body.insert("max_tokens", 16000);
	body.insert("system", system_prompt);
	body.insert("messages", QJsonArray{message});
	// No temperature / top_p / top_k: this model rejects them with a 400.

	QNetworkRequest req{QUrl(QString::fromLatin1(k_endpoint))};
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	req.setRawHeader("x-api-key", m_api_key.toUtf8());
	req.setRawHeader("anthropic-version", k_api_version);

	m_reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		const QByteArray raw = reply->readAll();
		const QJsonObject o = QJsonDocument::fromJson(raw).object();

		if (reply->error() != QNetworkReply::NoError) {
			// The API reports failures as a JSON error object; prefer its
			// message over Qt's generic transport string when present.
			const QString detail = o.value("error").toObject().value("message").toString();
			emit failed(detail.isEmpty()
			                ? QString("Claude request failed: %1").arg(reply->errorString())
			                : QString("Claude request failed: %1").arg(detail));
			return;
		}

		// A refusal arrives as a normal 200 with an empty or partial body, so
		// this has to be checked before reading content.
		if (o.value("stop_reason").toString() == "refusal") {
			emit failed("Claude declined this request.");
			return;
		}

		QString text;
		const QJsonArray content = o.value("content").toArray();
		for (const QJsonValue &v : content) {
			const QJsonObject block = v.toObject();
			if (block.value("type").toString() == "text")
				text += block.value("text").toString();
		}

		if (text.isEmpty())
			emit failed("Claude returned no text content.");
		else
			emit finished(text);
	});
}

void claude_provider::cancel() {
	if (m_reply && m_reply->isRunning())
		m_reply->abort();
}
