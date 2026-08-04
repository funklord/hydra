// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "annoyance_log.h"

#include <QDialog>

class QListWidget;

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

	// The machine-readable form, for `annoyance_log::set_outcome`.
	static QString name_of(action a);

private:
	action       m_chosen = action::recorded;
	QListWidget *m_suspects = nullptr;
};
