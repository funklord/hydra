#include "annoyed_dialog.h"

#include "flow_layout.h"

#include "site_extractor.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QList<annoyed_dialog::group> annoyed_dialog::collapse_by_shape(
    const QStringList &urls) {
	QList<group> out;
	QHash<QString, int> at;   // shape -> index into `out`
	for (const QString &u : urls) {
		// **The query goes, and that is the whole correction.**
		// `site_extractor::shape_of` keeps query *keys*, because for the
		// extractor two addresses with different keys are different questions.
		// For reading a suspect list they are the same beacon: measured on
		// kisskh, three calls to one analytics endpoint carried 41, 42 and 44
		// keys, so shape_of saw three shapes and collapsed nothing -- which was
		// the exact case this exists to fix.
		//
		// So the query is dropped first and `shape_of` is still asked about the
		// rest, which is where the hard part lives: it collapses digit runs and
		// long mixed-case path tokens, so a beacon whose *path* carries the
		// payload still folds together.
		QUrl endpoint = QUrl(u);
		endpoint.setQuery(QString());
		const QString shape = site_extractor::shape_of(endpoint);
		const auto it = at.constFind(shape);
		if (it != at.constEnd()) {
			++out[it.value()].count;
			continue;
		}
		at.insert(shape, int(out.size()));
		out.append(group{ u, 1 });
	}
	return out;
}

QString annoyed_dialog::name_of(action a) {
	switch (a) {
	case action::zap:     return QStringLiteral("zapped");
	case action::evolve:  return QStringLiteral("evolved");
	case action::consent: return QStringLiteral("consent");
	case action::recorded: break;
	}
	return QStringLiteral("recorded");
}

annoyed_dialog::annoyed_dialog(const annoyance_report &report, QWidget *parent)
    : QDialog(parent) {
	setWindowTitle("Something got through");
	setObjectName("annoyed_dialog");
	auto *box = new QVBoxLayout(this);

	auto *what = new QLabel(this);
	what->setObjectName("summary");
	what->setWordWrap(true);
	// Counts first, because they say whether there is anything to act on. A
	// page with forty requests and no ad-shaped ones is a different problem
	// from one with six suspects, and the tools below differ accordingly.
	what->setText(QString("<b>%1</b><br>%2 request%3 seen on this page, "
	                       "%4 of them ad-shaped.")
	                  .arg(report.host.toHtmlEscaped())
	                  .arg(report.observed)
	                  .arg(report.observed == 1 ? "" : "s")
	                  .arg(report.suspects.size()));
	box->addWidget(what);

	// **What the page showed when the button was pressed**, first, because it
	// is the one answer to "is there an ad" rather than to "what did the
	// browser do". A player saying it is in an ad is not a guess; the rest is
	// evidence a rule can be written from.
	if (!report.detected.isEmpty()) {
		auto *seen_head = new QLabel(
		  QString("Seen on the page when you pressed this (%1):")
		      .arg(report.detected.size()), this);
		seen_head->setObjectName("detected_head");
		seen_head->setWordWrap(true);
		box->addWidget(seen_head);

		auto *seen = new QListWidget(this);
		seen->setObjectName("detected");
		for (const QString &d : report.detected)
			seen->addItem(d);
		seen->setSelectionMode(QAbstractItemView::NoSelection);
		seen->setFocusPolicy(Qt::NoFocus);
		seen->setWordWrap(true);
		box->addWidget(seen, 1);
	}

	m_suspects = new QListWidget(this);
	m_suspects->setObjectName("suspects");
	for (const group &g : collapse_by_shape(report.suspects)) {
		// **The count goes first.** Appended after the address it was never
		// seen: these are ninety-character analytics urls in a list that
		// scrolls sideways, so `x3` sat off the right edge and the row looked
		// like a single request. The one piece of information the collapsing
		// adds was the one piece placed where nobody would find it.
		auto *row = new QListWidgetItem(
		    g.count > 1 ? QString("\u00d7%1  %2").arg(g.count).arg(g.url)
		                : QString("    %1").arg(g.url),
		    m_suspects);
		// The collapsing is for reading; the addresses are still the evidence,
		// so the row says how many it stands for and does not pretend the rest
		// are gone.
		row->setToolTip(g.count > 1
		                    ? QString("%1\n\nand %2 more address%3 of the same "
		                               "shape")
		                          .arg(g.url).arg(g.count - 1)
		                          .arg(g.count == 2 ? "" : "es")
		                    : g.url);
	}
	if (report.suspects.isEmpty()) {
		// Said plainly rather than left blank. An empty list reads as "nothing
		// was captured"; this is the more useful statement that the network
		// half found nothing, so whatever is wrong is probably cosmetic or is
		// the site itself.
		// And when the page patches ran, the likelier reading is an ad from
		// the site's own host, which is what a video pre-roll looks like from
		// the network: nothing at all.
		const bool same_host = !report.patches.isEmpty() ||
		                       !report.detected.isEmpty();
		m_suspects->addItem(!same_host
		  ? QStringLiteral("Nothing on the network looked ad-shaped — so this "
		                    "is likely cosmetic, a consent banner, or the site "
		                    "itself.")
		  : QStringLiteral("Nothing on the network looked ad-shaped. An ad "
		                    "served from the site's own host, such as a video "
		                    "pre-roll, looks like this — what the page showed "
		                    "and the page patches are what reach it."));
		m_suspects->setEnabled(false);
	}
	box->addWidget(m_suspects, 1);

	// **What the page asked this browser for.** Shown only when there is
	// something, because most pages ask for nothing and an empty box under
	// every report would teach people to stop reading this one.
	//
	// It is here because the network half cannot answer a whole class of
	// complaint. "It says I have no camera" produces no ad-shaped request and
	// no cosmetic leak; the evidence is that the page asked, and what it was
	// told, and that was previously visible only through a debug switch on the
	// desktop and a logcat line on the phone -- neither reachable by the person
	// actually looking at the broken page.
	if (!report.capabilities.isEmpty()) {
		auto *cap_head = new QLabel(
		  QString("It also asked for %1 capabilit%2, most recent first:")
		    .arg(report.capabilities.size())
		    .arg(report.capabilities.size() == 1 ? "y" : "ies"), this);
		cap_head->setObjectName("capabilities_head");
		cap_head->setWordWrap(true);
		box->addWidget(cap_head);

		auto *caps = new QListWidget(this);
		caps->setObjectName("capabilities");
		for (const QString &c : report.capabilities)
			caps->addItem(c);
		// **Not disabled, which is what this was and what it looked like.**
		// `setEnabled(false)` greys the text, and grey means "unavailable" --
		// exactly the wrong thing to say about the one part of this dialog
		// somebody is meant to read. Found by looking at the phone-geometry
		// capture rather than by any assertion: every check passed while the
		// evidence was the least legible thing on screen.
		//
		// Not selectable and not in the tab chain instead, which says "this is
		// not a control" without saying "this does not apply to you".
		caps->setSelectionMode(QAbstractItemView::NoSelection);
		caps->setFocusPolicy(Qt::NoFocus);
		box->addWidget(caps, 1);
	}

	// **What the page patches did.** An ad from the site's own host -- a
	// pre-roll is the common one -- leaves the list above empty, and until
	// this the dialog then said the problem was probably cosmetic, which for
	// a video ad is wrong. What answers it is whether the patches for this
	// site ran, failed, or never applied, so that is shown, read-only like
	// the capabilities and for the same reason.
	if (!report.patches.isEmpty()) {
		auto *patch_head = new QLabel(
		  "Page patches that ran here, most recent first:", this);
		patch_head->setObjectName("patches_head");
		patch_head->setWordWrap(true);
		box->addWidget(patch_head);

		auto *patches = new QListWidget(this);
		patches->setObjectName("patches");
		for (const QString &p : report.patches)
			patches->addItem(p);
		patches->setSelectionMode(QAbstractItemView::NoSelection);
		patches->setFocusPolicy(Qt::NoFocus);
		patches->setWordWrap(true);
		box->addWidget(patches, 1);
	}

	auto *hint = new QLabel(
	    "The report is kept either way. Pick a tool if one fits, or let the AI "
	    "work on it.", this);
	hint->setWordWrap(true);
	box->addWidget(hint);

	build_ai(box);

	// **Three tools that wrap, and the plain answer below them.** All four were
	// one QDialogButtonBox, whose minimum is the sum of its buttons -- 457
	// pixels, so this window would not go below 479 and its labels squeezed
	// past reading on anything narrower. The same shape, and the same fix, as
	// the downloads dialog.
	//
	// Ordered by how specific each is: a picked element is the most precise
	// thing anyone can give. The least specific answer keeps its place last
	// and still carries the default, which is now the bottom row rather than
	// the right of one -- the reading is the same and it survives a narrow
	// screen, which the row did not.
	auto *tools = new flow_layout;
	auto *zap = new QPushButton("&Zap an Element…", this);
	auto *evo = new QPushButton("Propose &Filter Rules…", this);
	auto *con = new QPushButton("&Cookie Banner Rule…", this);
	for (QPushButton *b : { zap, evo, con })
		tools->addWidget(b);
	box->addLayout(tools);

	auto *bb = new QDialogButtonBox(this);
	QPushButton *rec = bb->addButton("Just &Record It", QDialogButtonBox::AcceptRole);
	rec->setDefault(true);

	// **Offered when there is evidence of either kind.** It used to need a
	// suspect request, which a same-host ad never produces -- so the one tool
	// that can propose a page patch was disabled on exactly the page that
	// needed one. The filter dialog sends the patch report to the model too.
	const bool evidence = !report.suspects.isEmpty() ||
	                      !report.patches.isEmpty() ||
	                      !report.detected.isEmpty();
	evo->setEnabled(evidence);
	evo->setToolTip(!evidence
	                    ? QStringLiteral("Nothing ad-shaped was seen on this "
	                                      "page, none was showing and no page "
	                                      "patch ran, so there is nothing to "
	                                      "propose a rule against.")
	                    : QStringLiteral("Ask for filter rules from what this "
	                                      "page showed, what it requested and "
	                                      "what its patches did."));

	// **Non-modal now, so the choice is a signal.** The window stays open
	// beside the page while an investigation runs, which `exec()` could not
	// allow; a tool or the plain answer still closes it, and closing it any
	// other way is "recorded", as it always was.
	const auto choose = [this](action a) {
		m_chosen = a;
		accept();
	};
	connect(zap, &QPushButton::clicked, this, [choose] { choose(action::zap); });
	connect(evo, &QPushButton::clicked, this,
	        [choose] { choose(action::evolve); });
	connect(con, &QPushButton::clicked, this,
	        [choose] { choose(action::consent); });
	connect(rec, &QPushButton::clicked, this,
	        [choose] { choose(action::recorded); });
	connect(this, &QDialog::finished, this, [this](int) {
		emit ai_stop_requested();
		emit chose(m_chosen);
	});
	box->addWidget(bb);

	resize(680, 560);
}

void annoyed_dialog::build_ai(QVBoxLayout *box) {
	auto *head = new QLabel("<b>Work on it with AI</b>", this);
	box->addWidget(head);

	m_ai_note = new QLabel(this);
	m_ai_note->setObjectName("ai_note");
	m_ai_note->setWordWrap(true);
	box->addWidget(m_ai_note);

	auto *row = new QHBoxLayout;
	m_ai_start = new QPushButton("&Work On It", this);
	m_ai_start->setObjectName("ai_start");
	m_ai_start->setToolTip("The AI looks at the page, tries rules on this tab "
	                       "only, and asks you when it is stuck. Nothing is "
	                       "kept unless you keep it.");
	m_ai_stop = new QPushButton("S&top", this);
	m_ai_stop->setObjectName("ai_stop");
	m_ai_stop->setEnabled(false);
	row->addWidget(m_ai_start);
	row->addWidget(m_ai_stop);
	row->addStretch(1);
	box->addLayout(row);

	m_ai_status = new QLabel(this);
	m_ai_status->setObjectName("ai_status");
	m_ai_status->setWordWrap(true);
	box->addWidget(m_ai_status);

	// **The log is the point of the window**, so it is a list a person can
	// read back rather than a status line that overwrites itself: what the
	// model wanted, what the browser did, what the page then showed.
	m_ai_log = new QListWidget(this);
	m_ai_log->setObjectName("ai_log");
	m_ai_log->setWordWrap(true);
	m_ai_log->setSelectionMode(QAbstractItemView::NoSelection);
	m_ai_log->setFocusPolicy(Qt::NoFocus);
	m_ai_log->hide();
	box->addWidget(m_ai_log, 2);

	// The question, when there is one. Shown only then, and it says so in
	// the status line too, because a question nobody notices is a stalled
	// investigation.
	m_question = new QWidget(this);
	m_question->setObjectName("question");
	auto *q = new QVBoxLayout(m_question);
	q->setContentsMargins(0, 0, 0, 0);
	m_question_text = new QLabel(m_question);
	m_question_text->setWordWrap(true);
	m_question_text->setObjectName("question_text");
	q->addWidget(m_question_text);
	auto *answers = new QHBoxLayout;
	m_yes    = new QPushButton("&Yes", m_question);
	m_no     = new QPushButton("&No", m_question);
	m_show   = new QPushButton("&Show Me…", m_question);
	m_cannot = new QPushButton("I &Can't", m_question);
	for (QPushButton *b : { m_yes, m_no, m_show, m_cannot })
		answers->addWidget(b);
	answers->addStretch(1);
	q->addLayout(answers);
	m_question->hide();
	box->addWidget(m_question);

	m_result = new QWidget(this);
	m_result->setObjectName("result");
	auto *r = new QVBoxLayout(m_result);
	r->setContentsMargins(0, 0, 0, 0);
	m_result_text = new QLabel(m_result);
	m_result_text->setWordWrap(true);
	r->addWidget(m_result_text);
	m_result_rules = new QListWidget(m_result);
	m_result_rules->setObjectName("result_rules");
	m_result_rules->setSelectionMode(QAbstractItemView::NoSelection);
	r->addWidget(m_result_rules);
	auto *decide = new QHBoxLayout;
	m_keep    = new QPushButton("&Keep These Rules", m_result);
	m_discard = new QPushButton("&Discard Them", m_result);
	decide->addWidget(m_keep);
	decide->addWidget(m_discard);
	decide->addStretch(1);
	r->addLayout(decide);
	m_result->hide();
	box->addWidget(m_result);

	m_ticker = new QTimer(this);
	m_ticker->setInterval(1000);
	connect(m_ticker, &QTimer::timeout, this, &annoyed_dialog::tick);

	connect(m_ai_start, &QPushButton::clicked, this, [this] {
		m_ai_start->setEnabled(false);
		m_ai_stop->setEnabled(true);
		m_ai_log->show();
		m_result->hide();
		emit ai_start_requested();
	});
	connect(m_ai_stop, &QPushButton::clicked, this, [this] {
		m_ai_stop->setEnabled(false);
		emit ai_stop_requested();
	});
	const auto answer = [this](const QString &a) {
		m_question->hide();
		auto fn = std::move(m_answer);
		m_answer = nullptr;
		if (fn)
			fn(a);
	};
	connect(m_yes, &QPushButton::clicked, this, [answer] { answer("yes"); });
	connect(m_no, &QPushButton::clicked, this, [answer] { answer("no"); });
	connect(m_cannot, &QPushButton::clicked, this,
	        [answer] { answer("none"); });
	connect(m_show, &QPushButton::clicked, this, [this] {
		m_ai_status->setText("Click the thing on the page — Escape cancels.");
		emit point_requested();
	});
	connect(m_keep, &QPushButton::clicked, this, [this] {
		m_result->hide();
		emit keep_requested(m_rules);
	});
	connect(m_discard, &QPushButton::clicked, this, [this] {
		m_result->hide();
		emit discard_requested();
	});
}

void annoyed_dialog::set_ai(bool ready, const QString &note) {
	m_ai_note->setText(note);
	m_ai_start->setEnabled(ready);
}

void annoyed_dialog::log_line(const QString &kind, const QString &text) {
	m_ai_log->show();
	auto *item = new QListWidgetItem(
	  QString("%1  %2").arg(QTime::currentTime().toString("HH:mm:ss"), text),
	  m_ai_log);
	item->setData(Qt::UserRole, kind);
	if (kind == QLatin1String("error"))
		item->setForeground(palette().color(QPalette::Highlight));
	m_ai_log->scrollToBottom();
	// The step is no longer being waited on once something new arrives.
	if (kind != QLatin1String("step")) {
		m_waiting_for.clear();
		m_ticker->stop();
		m_ai_status->setText(text);
	}
}

void annoyed_dialog::expecting(const QString &what, int seconds) {
	m_waiting_for   = what;
	m_waiting_eta   = seconds;
	m_waiting_since = QDateTime::currentMSecsSinceEpoch();
	tick();
	m_ticker->start();
}

// "Waiting for the model's answer: 7 s of about 30." Past the estimate it says
// so rather than counting into negative numbers, because an estimate that has
// been passed is information too.
void annoyed_dialog::tick() {
	if (m_waiting_for.isEmpty())
		return;
	const int gone =
	  int((QDateTime::currentMSecsSinceEpoch() - m_waiting_since) / 1000);
	m_ai_status->setText(gone <= m_waiting_eta
	    ? QString("Waiting for %1: %2 s of about %3.")
	          .arg(m_waiting_for).arg(gone).arg(m_waiting_eta)
	    : QString("Waiting for %1: %2 s, longer than the %3 expected.")
	          .arg(m_waiting_for).arg(gone).arg(m_waiting_eta));
}

void annoyed_dialog::ask(const QString &kind, const QString &question,
                         std::function<void(const QString &)> answer) {
	m_answer = std::move(answer);
	const bool point = kind == QLatin1String("point");
	m_question_text->setText("<b>The AI asks:</b> " + question.toHtmlEscaped());
	m_yes->setVisible(!point);
	m_no->setVisible(!point);
	m_show->setVisible(point);
	m_cannot->setVisible(point);
	m_question->show();
	m_waiting_for.clear();
	m_ticker->stop();
	m_ai_status->setText("Waiting for your answer below.");
	raise();
	activateWindow();
}

void annoyed_dialog::answer_point(const QString &picked) {
	m_question->hide();
	auto fn = std::move(m_answer);
	m_answer = nullptr;
	if (fn)
		fn(picked);
}

void annoyed_dialog::ai_finished(bool solved, const QString &summary,
                                 const QStringList &rules) {
	m_ticker->stop();
	m_waiting_for.clear();
	m_question->hide();
	m_ai_stop->setEnabled(false);
	m_ai_start->setEnabled(true);
	m_ai_start->setText("&Work On It Again");
	m_rules = rules;
	m_ai_status->setText(QString("%1: %2").arg(solved ? "Solved" : "Not solved",
	                                            summary));
	m_result_rules->clear();
	for (const QString &r : rules)
		m_result_rules->addItem(r);
	if (rules.isEmpty()) {
		m_result->hide();
		return;
	}
	m_result_text->setText(solved
	    ? QString("These %1 rule(s) are in trial on this tab. Keep them, and "
	              "they apply to this site from now on.").arg(rules.size())
	    : QString("These %1 rule(s) are in trial but did not solve it. Keep "
	              "them anyway, or discard them.").arg(rules.size()));
	m_result->show();
}
