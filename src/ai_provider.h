// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

// One pluggable AI backend (architecture doc §9.1).
//
// Resolution is local-first: if a local model is present and enabled it handles
// requests and nothing leaves the machine. An external provider is used only
// when the user configures and selects one, and whenever an external provider
// is active the review-before-send gate defaults on — URLs and titles are
// themselves sensitive.
//
// The interface is deliberately small: one text request, one text reply. The
// reorganizer, the (later) filter-evolution loop, and the diff/accept pipeline
// are all provider-agnostic, so adding a provider is one adapter class.
class ai_provider : public QObject {
	Q_OBJECT
public:
	explicit ai_provider(QObject *parent = nullptr) : QObject(parent) {}

	virtual QString name() const = 0;

	// True when this backend looks usable — a local server is reachable, or a
	// credential is present. Cheap and non-blocking; not a guarantee.
	virtual bool available() const = 0;

	// Does anything leave the machine? Drives the review-before-send gate.
	virtual bool is_external() const = 0;

	// Asynchronous: exactly one of finished() / failed() follows.
	virtual void send(const QString &system_prompt, const QString &user_prompt) = 0;
	virtual void cancel() {}

signals:
	void finished(const QString &reply);
	void failed(const QString &error);
};
