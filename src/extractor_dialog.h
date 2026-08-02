// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "site_extractor.h"

#include <QDialog>
#include <QPointer>
#include <QHash>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class ai_provider;
class extractor_signals;
class extractor_store;
class stream_probe;
class helper_host;
struct helper_call;

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
	// `served` annotates a line with what that address actually returned, keyed
	// by url. Optional, because the tier is.
	static QString summarise(const QList<evidence_request> &evidence,
	                          int *kept = nullptr,
	                          const QHash<QString, QString> *served = nullptr);

	// Which addresses are worth asking the server about, most promising first.
	//
	// Not all of them: probing costs a request each, and most of a page's
	// traffic is furniture the gate would refuse anyway. Skips the page itself
	// and anything fetched as a script or an image, keeps one per repeated
	// shape, and puts the addresses fetched *once* first — a manifest is
	// fetched once and its segments are not, so that ordering spends the budget
	// where the answer usually is.
	static QList<evidence_request> candidates(const QList<evidence_request> &evidence,
	                                           const QUrl &page, int max);

	// Enough to cover a manifest and a representative segment or two without
	// turning a review into a crawl.
	static constexpr int k_max_probes = 10;

	// Models fence code even when asked not to. Strip it rather than fail.
	static QString strip_fences(const QString &reply);

	// What the script did, rendered for a person (§11.5.1). Pure, so the
	// wording is tested without standing up a dialog — and the wording is the
	// point, since this is what consent is given against.
	static QString transcript_text(const QList<helper_call> &calls);

	// Run proposals with the helper tier. Null, the default, is the pure tier:
	// the script gets no `hydra` and there is nothing to show.
	void use_helpers(helper_host *helpers);

private:
	void build_ui();
	void on_send();
	void on_reply(const QString &text);
	void on_judged(const extractor_verdict &verdict);
	void on_failed(const QString &error);
	void on_accept();

	// The §10 content-type tier: fetch what was picked, with the page's own
	// context, and say what it really serves. Advisory unless it contradicts.
	void confirm_by_fetching();
	void show_transcript();

	// Ask the server about the likely candidates before the model is asked
	// anything, and fold the answers into the payload.
	void probe_candidates();
	void rebuild_payload();

	extractor_signals *m_signals  = nullptr;
	extractor_store   *m_store    = nullptr;
	ai_provider       *m_provider = nullptr;
	stream_probe      *m_probe    = nullptr;
	helper_host       *m_helpers  = nullptr;
	QString            m_site;
	QUrl               m_page;
	QList<evidence_request> m_evidence;
	QString            m_proposal;
	QHash<QString, QString> m_served;   // url -> what it actually returned
	// The same answers, kept as the gate needs them rather than as the payload
	// prints them: the addresses the tier positively identified as a playlist.
	// Parsing them back out of the human sentence above would be a second
	// spelling of one fact, and the two would drift.
	QSet<QString>           m_manifests;
	int                m_pending = 0;   // probes still out
	extractor_verdict  m_verdict;

	QStackedWidget *m_pages   = nullptr;
	QPlainTextEdit *m_payload = nullptr;
	QPlainTextEdit *m_script  = nullptr;
	QLabel         *m_status  = nullptr;
	QLabel         *m_result  = nullptr;
	QPlainTextEdit *m_transcript = nullptr;
	QLabel         *m_transcript_label = nullptr;
	QPushButton    *m_send    = nullptr;
	QPushButton    *m_apply   = nullptr;
};
