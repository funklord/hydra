// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "site_extractor.h"

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class ai_provider;
class extractor_signals;
class extractor_store;
class stream_probe;

// Ask for a parser for this site, then judge the answer (architecture doc
// §11.5).
//
// The third use of the same diff/accept shape as the tree reorganizer (§9) and
// the filter loop (§12): show exactly what will be sent, send nothing until
// asked, validate what comes back, and let the user accept or refuse. What
// differs is the output — a script rather than a rearrangement or a rule — and
// the gate, which here is that the script must pick a URL the page really
// requested.
class extractor_dialog : public QDialog {
	Q_OBJECT
public:
	extractor_dialog(extractor_signals *signals_source, extractor_store *store,
	                  ai_provider *provider, const QString &site_host,
	                  const QUrl &page_url, QWidget *parent = nullptr);

	// The evidence, folded down to something a person can read and a model can
	// use. A player requests hundreds of segments that differ only by a number,
	// and sending all of them buries the handful of requests that matter.
	static QString summarise(const QList<evidence_request> &evidence,
	                          int *kept = nullptr);

	// Models fence code even when asked not to. Strip it rather than fail.
	static QString strip_fences(const QString &reply);

private:
	void build_ui();
	void on_send();
	void on_reply(const QString &text);
	void on_failed(const QString &error);
	void on_accept();

	// The §10 content-type tier: fetch what was picked, with the page's own
	// context, and say what it really serves. Advisory unless it contradicts.
	void confirm_by_fetching();

	extractor_signals *m_signals  = nullptr;
	extractor_store   *m_store    = nullptr;
	ai_provider       *m_provider = nullptr;
	stream_probe      *m_probe    = nullptr;
	QString            m_site;
	QUrl               m_page;
	QList<evidence_request> m_evidence;
	QString            m_proposal;
	extractor_verdict  m_verdict;

	QStackedWidget *m_pages   = nullptr;
	QPlainTextEdit *m_payload = nullptr;
	QPlainTextEdit *m_script  = nullptr;
	QLabel         *m_status  = nullptr;
	QLabel         *m_result  = nullptr;
	QPushButton    *m_send    = nullptr;
	QPushButton    *m_apply   = nullptr;
};
