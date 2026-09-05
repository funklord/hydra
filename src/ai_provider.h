#pragma once

#include <QAbstractButton>
#include <QObject>
#include <QString>

// Which backend the user wants used (architecture doc sec 9.1).
//
// `automatic` is the local-first rule the design describes: a reachable local
// model handles everything and nothing leaves the machine, falling back to an
// external provider only when there is no local one.
//
// `local_only` exists because "nothing leaves this machine" should be
// something a user can *enforce*, not merely a default that quietly stops
// applying the day Ollama is not running. Under it, no local model means no
// AI features rather than a silent switch to a service.
enum class ai_choice {
	automatic,
	local_only,
	external,
};

// One pluggable AI backend (architecture doc sec 9.1).
//
// Resolution is local-first: if a local model is present and enabled it handles
// requests and nothing leaves the machine. An external provider is used only
// when the user configures and selects one, and whenever an external provider
// is active the review-before-send gate defaults on -- URLs and titles are
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

	// True when this backend looks usable -- a local server is reachable, or a
	// credential is present. Cheap and non-blocking; not a guarantee.
	virtual bool available() const = 0;

	// True when a request stands a chance of being answered, which is not the
	// same as `available()`.
	//
	// **`available()` is about the backend; this is about the model.** Ollama
	// answers its API as soon as it is serving, so `available()` is true with
	// no usable model installed at all -- and the dialogs then offered Send,
	// beside a label reading "not installed", which is the shape this project
	// keeps removing: a control that looks usable and cannot work.
	//
	// Default is `available()`, because for a backend with no separate notion
	// of a model the two questions are the same one. Say why in `reason` when
	// answering false, since a disabled button has to explain itself somewhere.
	virtual bool ready(QString *reason = nullptr) const {
		if (available())
			return true;
		if (reason)
			*reason = name() + " is not available.";
		return false;
	}

	// Does anything leave the machine? Drives the review-before-send gate.
	virtual bool is_external() const = 0;

	// Asynchronous: exactly one of finished() / failed() follows.
	virtual void send(const QString &system_prompt, const QString &user_prompt) = 0;
	virtual void cancel() {}

signals:
	void finished(const QString &reply);
	void failed(const QString &error);
};

// The sentence above a Send button, saying where what is about to leave is
// going.
//
// **One copy, because it is a privacy claim and there were three.** Each of
// the three review dialogs built its own, and the local half was identical in
// all of them -- so `ollama_provider::is_external()` returning a flat `false`
// made the same false promise in three places at once, and a fix had to be
// found three times. The external halves had already drifted into three
// wordings.
//
// `what_travels` is the one part that is genuinely the dialog's own: what its
// particular payload contains. Everything around it is the same question.
inline QString provider_note(const ai_provider *provider,
                              const QString &what_travels) {
	if (!provider)
		return QStringLiteral("<b>No AI backend.</b> Nothing can be sent.");
	const QString name = "<b>" + provider->name().toHtmlEscaped() + "</b>";
	// Not "on this machine" as a phrase about the backend: `is_external` is
	// asked of the provider precisely so that a backend which is usually local
	// can say when it is not.
	if (!provider->is_external())
		return name + QStringLiteral(" \u2014 local provider; nothing leaves "
		                              "this machine.");
	QString note = name + QStringLiteral(" \u2014 external provider.");
	if (!what_travels.isEmpty())
		note += " " + what_travels;
	return note + QStringLiteral(" Nothing leaves until you press Send.");
}

// Disable a Send button when the provider cannot answer, and say why on it.
//
// **The label was already honest and the button was not.** `ollama_provider`
// grew a "not installed" name precisely because the reorganizer used to
// announce a model that was not there, and the first anyone knew was a failed
// request after pressing Send -- but the button that produces that failure sat
// beside the warning, enabled. This project's own rule is that a control which
// cannot work should look unavailable rather than explain itself afterwards.
//
// A tooltip rather than a status message: the question "why is this greyed
// out" is asked of the button, so the answer belongs on it.
inline void gate_send(QAbstractButton *send, const ai_provider *provider) {
	if (!send)
		return;
	QString why;
	const bool ok = provider && provider->ready(&why);
	send->setEnabled(ok);
	send->setToolTip(ok ? QString() : why);
}

