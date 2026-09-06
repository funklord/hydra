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

// The non-destructive reorganize pipeline, as a dialog (architecture doc sec 9.2).
//
//   review payload -> send -> receive -> check invariants -> diff -> cherry-pick
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
	// **Where the payload is going, in a label nothing else writes to.**
	//
	// This used to be `m_status`'s first text, and `m_status` is also the
	// working line -- "Asking...", a probe result, a failure. The extractor
	// probes its candidates the moment it opens, so by the time anybody read
	// the screen the sentence naming the provider had been replaced by a
	// count of addresses, on the one dialog whose payload is every address
	// the page requested. A sentence that has to be read before pressing Send
	// cannot share a widget with progress.
	QLabel         *m_provider_note = nullptr;
	// **The status line, which takes no room when it has nothing to say.**
	//
	// Splitting the provider sentence into its own label left this one empty
	// on a freshly opened dialog, and an empty `QLabel` still reserves a line:
	// a blank strip appeared above the payload in all three. Going through one
	// method rather than calling `setText` in a dozen places is what makes the
	// rule -- empty means hidden -- a property of the widget instead of
	// something every caller has to remember.
	void say(const QString &text);
	QLabel         *m_status  = nullptr;
	QLabel         *m_report  = nullptr;
	QListWidget    *m_changes = nullptr;
	QPushButton    *m_send    = nullptr;
	QPushButton    *m_apply   = nullptr;

	QList<tree_change> m_change_list;
};
