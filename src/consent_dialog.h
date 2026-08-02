// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "site_rules.h"

#include <QDialog>

class QLabel;
class QTreeWidget;
class QPushButton;
class consent_blocker;

// Where a banner nobody could answer becomes a rule.
//
// The same accept-or-refuse shape as the filter and extractor loops, and the
// simplest of the three, because there is no model in it: the evidence *is* the
// proposal. A banner that could not be answered was recorded with the labels it
// offered, and a label is very nearly a rule already — all that is missing is
// which of them means "reject" and which means "accept", and that is a question
// only a person can answer for a language nobody here reads.
//
// So the dialog asks exactly that and nothing else. No free text: a rule is
// built from a label the page really offered, escaped, by
// `consent_blocker::rule_from_label`. Letting someone type a pattern would let
// them type `.*`, and this is a corpus meant to be shared.
//
// **It says what scope the rule gets, and why.** A label describes a shape
// banners take rather than a site, so an accepted rule is generic and is
// flagged for the shipped defaults. That is worth stating in front of the
// person accepting it: they are not fixing one site, they are proposing
// something everyone would carry.
class consent_dialog : public QDialog {
	Q_OBJECT
public:
	consent_dialog(consent_blocker *blocker, const QString &rules_path,
	                QWidget *parent = nullptr);

private slots:
	void on_accept_selected();

private:
	void rebuild();
	void refresh_buttons();

	consent_blocker *m_blocker = nullptr;
	QString          m_path;
	QLabel          *m_intro   = nullptr;
	QTreeWidget     *m_list    = nullptr;
	QLabel          *m_status  = nullptr;
	QPushButton     *m_reject  = nullptr;
	QPushButton     *m_accept  = nullptr;
};
