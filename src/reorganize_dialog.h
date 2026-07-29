// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tree_diff.h"

#include <QDialog>
#include <QList>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class ai_provider;
class tab_tree_model;
struct node;

// The non-destructive reorganize pipeline, as a dialog (architecture doc §9.2).
//
//   review payload → send → receive → check invariants → diff → cherry-pick
//
// The live tree is not touched until the user accepts. The proposal is parsed
// into a shadow tree that this dialog owns and deletes; "accept" turns the
// selected changes into model edits, and nothing else in the app has seen the
// proposal by then.
class reorganize_dialog : public QDialog {
	Q_OBJECT
public:
	reorganize_dialog(tab_tree_model *model, ai_provider *provider,
	                   QWidget *parent = nullptr);
	~reorganize_dialog() override;

private slots:
	void on_send();
	void on_reply(const QString &text);
	void on_failed(const QString &error);
	void on_accept();

private:
	void build_ui();
	void show_diff();

	tab_tree_model *m_model    = nullptr;   // not owned
	ai_provider    *m_provider = nullptr;   // not owned
	node           *m_proposal = nullptr;   // the shadow tree; owned

	QStackedWidget *m_pages   = nullptr;
	QPlainTextEdit *m_payload = nullptr;
	QLabel         *m_status  = nullptr;
	QLabel         *m_report  = nullptr;
	QListWidget    *m_changes = nullptr;
	QPushButton    *m_send    = nullptr;
	QPushButton    *m_apply   = nullptr;

	QList<tree_change> m_change_list;
};
