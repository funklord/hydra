#pragma once

#include "annoyance_log.h"

#include <QDialog>
#include <QStringList>

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QVBoxLayout;
class QWidget;

// What happens after the one click: show what was captured, and offer the
// tools that already exist.
//
// **A funnel, not a fourth tool.** The element picker, filter evolution and the
// consent-rule editor are all built and all require knowing what went wrong
// before you can reach them. This is the step in front of them -- it says what
// the page was doing at the moment somebody said it was wrong, and then hands
// them to whichever of the three fits.
//
// **It shows the evidence rather than thanking you for the feedback.** A button
// whose click produces nothing visible teaches people it is theatre, which is
// the same defect as a permission for a capability that does not exist. So the
// suspects are on screen, the report is written whether or not any tool is
// chosen, and "just record it" is an honest answer rather than a cancel.
class annoyed_dialog : public QDialog {
	Q_OBJECT
public:
	// What the person chose. `recorded` is the default and the floor: the
	// report is filed either way, so dismissing this is not the same as not
	// having complained.
	enum class action { recorded, zap, evolve, consent };

	annoyed_dialog(const annoyance_report &report, QWidget *parent = nullptr);

	action chosen() const { return m_chosen; }

	// --- The control panel for the model loop -----------------------------
	//
	// **Asked for by the copyright holder on 2026-10-09**: this window is not
	// only a recording, it is where an iterative process works on the
	// problem -- saying what it is doing and how long it expects, asking the
	// person to look or to point when it is stuck, and ending with rules the
	// person keeps or discards. The window shows and asks; `investigation`
	// decides; the shell wires the two and owns the tab.
	//
	// Whether the AI can be asked, and if not why, and where what is sent
	// goes -- the same sentence the other AI dialogs show above Send, since
	// starting is consent to sending.
	void set_ai(bool ready, const QString &note);
	// One line of the log, with the time.
	void log_line(const QString &kind, const QString &text);
	// What is being waited for and how long it should take; the status line
	// counts against it.
	void expecting(const QString &what, int seconds);
	// A question for the person. `kind` "confirm" offers yes and no; "point"
	// offers to pick the thing on the page, which `point_requested` asks the
	// shell to start, and `answer_point` completes.
	void ask(const QString &kind, const QString &question,
	         std::function<void(const QString &)> answer);
	void answer_point(const QString &picked);
	// The loop ended. `rules` are those in trial, offered to keep.
	void ai_finished(bool solved, const QString &summary,
	                 const QStringList &rules);
	// The load knob's position, 10 to 100, without emitting.
	void set_load(int percent);

signals:
	// A tool was chosen; the window is closing. `recorded` when closed
	// without one.
	void chose(annoyed_dialog::action a);
	void ai_start_requested();
	void ai_stop_requested();
	void point_requested();
	void keep_requested(const QStringList &rules);
	void discard_requested();
	// The knob moved; applied from the next request, and remembered.
	void load_changed(int percent);

public:
	// The machine-readable form, for `annoyance_log::set_outcome`.
	static QString name_of(action a);

	// One row per *shape*, in the order the shapes were first seen, with how
	// many addresses fell into each.
	//
	// **For display only. The report keeps every address**, because that is the
	// corpus a proposed rule gets simulated against and collapsing it would
	// throw away the evidence. This is about what a person reads: measured on a
	// real site, three of five suspects were one analytics endpoint with
	// different query strings, which is three lines saying the same thing to
	// somebody deciding what to do about it.
	//
	// "Same" here means *same endpoint*: the query dropped entirely, and
	// `site_extractor::shape_of` asked about the rest so that digit runs and
	// long mixed-case path tokens still fold. That is deliberately coarser than
	// shape_of alone, which keeps query keys -- right for the extractor, where
	// two addresses with different keys are different questions, and wrong
	// here, where they are the same beacon reported twice.
	struct group {
		QString url;     // the first address seen with this shape
		int     count;   // how many addresses share it
	};
	static QList<group> collapse_by_shape(const QStringList &urls);

private:
	void build_ai(QVBoxLayout *box);
	void tick();

	action       m_chosen = action::recorded;
	QListWidget *m_suspects = nullptr;

	QLabel      *m_ai_note = nullptr;
	QPushButton *m_ai_start = nullptr;
	QPushButton *m_ai_stop = nullptr;
	QLabel      *m_ai_status = nullptr;
	QListWidget *m_ai_log = nullptr;
	QWidget     *m_question = nullptr;
	QLabel      *m_question_text = nullptr;
	QPushButton *m_yes = nullptr;
	QPushButton *m_no = nullptr;
	QPushButton *m_show = nullptr;
	QPushButton *m_cannot = nullptr;
	QWidget     *m_result = nullptr;
	QLabel      *m_result_text = nullptr;
	QListWidget *m_result_rules = nullptr;
	QPushButton *m_keep = nullptr;
	QPushButton *m_discard = nullptr;
	QTimer      *m_ticker = nullptr;
	QSlider     *m_load = nullptr;
	QLabel      *m_load_text = nullptr;
	QString      m_waiting_for;
	int          m_waiting_eta = 0;
	qint64       m_waiting_since = 0;
	std::function<void(const QString &)> m_answer;
	QStringList  m_rules;
};
