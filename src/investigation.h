#pragma once

#include "filter_list.h"
#include "scriptlets.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class ai_provider;

// **What the Annoyed button gathered**, handed to an investigation as the
// starting evidence. Everything here is already collected elsewhere; this is
// the snapshot at the moment somebody complained.
struct investigation_evidence {
	QString     host;
	QString     page;
	QStringList detected;      // `ad_probe::describe`: what the page showed
	QStringList patches;       // `scriptlets::describe`: what the patches did
	QStringList suspects;      // ad-shaped requests that got through
	QStringList observed;      // every request the page made, for dry runs
};

// **What an investigation may ask of the browser**, and nothing else. Three
// operations, so the loop can be driven against a fake in a test and a real
// tab in the shell, and so what a model can cause is a list somebody can read.
//
// Each answers through its callback exactly once, unless the window and the
// tab are gone first, in which case the investigation has been stopped too.
class investigation_host {
public:
	virtual ~investigation_host() = default;

	// Look at the page again. Answers with `ad_probe::describe` lines; empty
	// means nothing ad-shaped is on screen.
	virtual void look(std::function<void(const QStringList &)> done) = 0;

	// Replace the trial rules for this tab with these, reload it, and look.
	// An empty pair takes every trial rule back out. Trial rules apply to
	// this tab's site only and only while the investigation is open; nothing
	// is saved by this.
	virtual void trial(const QList<filter_rule> &rules,
	                   const QList<scriptlet_call> &calls,
	                   std::function<void(const QStringList &)> done) = 0;

	// Put a question to the person. `kind` is "confirm" -- answered "yes",
	// "no" -- or "point", answered with what they picked on the page, or
	// "none" when they would not or could not.
	virtual void ask(const QString &kind, const QString &question,
	                 std::function<void(const QString &)> done) = 0;
};

// **The Annoyed button's model loop**: the iterative process the window is a
// control panel for. Asked for by the copyright holder on 2026-10-09 -- not
// just recording, a process that works on the problem, says what it is doing
// and how long it expects to take, and asks the person to look or to point
// "not often, but when things get gnarly".
//
// Each turn the model is sent the evidence and everything that happened so
// far, and answers with exactly one action as JSON:
//
//     {"action": "look", "why": "..."}
//     {"action": "try", "rules": ["site##.ad", "site##+js(set, x, false)"],
//      "why": "..."}
//     {"action": "ask", "kind": "confirm" | "point", "question": "..."}
//     {"action": "done", "solved": true | false, "summary": "..."}
//
// The browser carries it out through the host and the result goes into the
// next turn. **Every proposed rule passes the gates a subscribed or accepted
// rule passes**: classified by `filter_subscription::classify`, a network or
// cosmetic rule dry-run by `filter_list::evaluate` and refused when too broad,
// a scriptlet only from the untrusted catalog, and anything scoped to a site
// other than this one refused. A refusal goes back to the model with its
// reason, so it can do better rather than repeat itself.
//
// Bounded at `k_max_turns`, and stopped early after two replies in a row
// with no action in them, because a model that has stopped following the
// protocol will not start again by being asked once more.
class investigation : public QObject {
	Q_OBJECT
public:
	static constexpr int k_max_turns = 8;

	investigation(ai_provider *provider, investigation_host *host,
	              const investigation_evidence &evidence,
	              QObject *parent = nullptr);
	~investigation() override;

	void start();
	// Stops after whatever is in flight; nothing further is sent or tried.
	void stop();
	bool running() const { return m_running; }
	int  turn() const { return m_turn; }

	// The rules in trial now, as text. What Keep saves.
	QStringList trial_rules() const;

	// For the tests and the window: the action a reply carries, or an empty
	// object when it carries none. The first `{...}` that parses as an object
	// with an "action" key -- a model that wraps it in prose or a code fence
	// is still understood.
	static QJsonObject action_in(const QString &reply);

signals:
	// One line of the log. `kind` is "step", "model", "page", "rule", "ask",
	// "error" or "done".
	void logged(const QString &kind, const QString &text);
	// How long the step just started is expected to take, in seconds.
	void expecting(const QString &what, int seconds);
	void finished(bool solved, const QString &summary);

private:
	void next_turn();
	void on_reply(const QString &reply);
	void on_failed(const QString &error);
	void act(const QJsonObject &a);
	void record(const QString &what_happened);
	void finish(bool solved, const QString &summary);
	QString prompt() const;
	int model_estimate() const;

	QPointer<ai_provider>  m_provider;
	investigation_host    *m_host = nullptr;
	investigation_evidence m_evidence;
	QStringList            m_history;      // what happened, turn by turn
	QList<filter_rule>     m_trial_rules;
	QList<scriptlet_call>  m_trial_calls;
	QStringList            m_trial_text;
	QStringList            m_last_seen;
	QElapsedTimer          m_clock;
	QList<qint64>          m_model_ms;
	int  m_turn = 0;
	int  m_bad_replies = 0;
	bool m_running = false;
	bool m_waiting_model = false;
};
