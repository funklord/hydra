#pragma once

#include "filter_list.h"

#include <QDialog>
#include <QList>

class QLabel;
class QTreeWidget;
class QPushButton;
class QStackedWidget;
class QPlainTextEdit;
class ai_provider;
class filter_signals;

// The filter-evolution loop's accept UI (architecture doc sec 12.5) -- Spine 3's
// diff/accept machinery pointed at the filter list instead of the tree.
//
// The shape deliberately mirrors reorganize_dialog, because it is the same
// pipeline: review payload -> send -> receive -> validate -> per-item accept.
// What differs is the safety core. There the invariant was "no node left
// behind"; here it is the dry run (sec 12.4), which statically rejects
// dangerously broad rules and simulates the survivors against the requests
// actually observed on the page, so each rule is shown with exactly what it
// would have blocked.
class filter_dialog : public QDialog {
	Q_OBJECT
public:
	filter_dialog(filter_signals *signals_source, filter_list *list,
	               ai_provider *provider, const QString &site_host,
	               const picked_element &picked = picked_element{},
	               QWidget *parent = nullptr);

	// **Cancels whatever it asked for.** The provider outlives this dialog --
	// it belongs to the window -- and `ai_provider::finished` carries no
	// request identity, so a reply still in flight when this closes is
	// broadcast to whichever dialog is connected when it lands. That is a
	// different question's answer arriving as this one's.
	//
	// `reorganize_dialog` has always done this and these two never did.
	~filter_dialog() override;

private slots:
	void on_send();
	void on_reply(const QString &text);
	void on_failed(const QString &error);
	void on_accept();

private:
	void build_ui();
	void show_proposals(const QList<filter_rule> &rules);

	filter_signals *m_signals  = nullptr;
	filter_list    *m_list     = nullptr;
	ai_provider    *m_provider = nullptr;
	QString         m_site;
	picked_element  m_picked;

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
	QLabel         *m_status  = nullptr;
	QTreeWidget    *m_rules   = nullptr;
	QPushButton    *m_send    = nullptr;
	QPushButton    *m_apply   = nullptr;

	QList<filter_rule> m_accepted;
};
