#pragma once

#include "ai_provider.h"

#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;

// The default external backend (architecture doc sec 9.1): the Anthropic Messages
// API over plain HTTP. There is no official Anthropic SDK for C++, so this
// speaks the REST endpoint directly rather than pulling in a shim.
//
// Because this is external, everything it sends is gated behind the
// review-before-send dialog, and only the sec 9.3 metadata is ever in the payload.
class claude_provider : public ai_provider {
	Q_OBJECT
public:
	explicit claude_provider(QObject *parent = nullptr);

	QString name() const override;
	bool available() const override;
	bool is_external() const override { return true; }
	void send(const QString &system_prompt, const QString &user_prompt) override;
	void cancel() override;

	// Held in memory only; never written to disk with the tree or policy -- and
	// that includes the settings file, which is plain INI.
	void set_api_key(const QString &key) { m_api_key = key; }
	bool has_api_key() const { return !m_api_key.isEmpty(); }
	void set_model(const QString &model) { m_model = model; }
	QString model() const { return m_model; }

private:
	QNetworkAccessManager *m_net = nullptr;
	QPointer<QNetworkReply> m_reply;
	QString m_api_key;
	// Current Anthropic model id. Deliberately not date-suffixed.
	QString m_model = "claude-opus-5";
};
