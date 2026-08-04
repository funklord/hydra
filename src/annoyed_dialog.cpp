// SPDX-License-Identifier: GPL-3.0-or-later
#include "annoyed_dialog.h"

#include "site_extractor.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QHash>
#include <QListWidget>
#include <QPushButton>
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

	m_suspects = new QListWidget(this);
	m_suspects->setObjectName("suspects");
	for (const group &g : collapse_by_shape(report.suspects)) {
		auto *row = new QListWidgetItem(
		    g.count > 1 ? QString("%1        \u00d7%2").arg(g.url).arg(g.count)
		                : g.url,
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
		m_suspects->addItem("Nothing on the network looked ad-shaped — so this "
		                     "is likely cosmetic, a consent banner, or the site "
		                     "itself.");
		m_suspects->setEnabled(false);
	}
	box->addWidget(m_suspects, 1);

	auto *hint = new QLabel(
	    "The report is kept either way. Pick a tool if one fits.", this);
	hint->setWordWrap(true);
	box->addWidget(hint);

	auto *bb = new QDialogButtonBox(this);
	// Ordered by how specific each is: a picked element is the most precise
	// thing anyone can give, and the least specific answer is on the right
	// where a default button sits.
	QPushButton *zap = bb->addButton("&Zap an Element…", QDialogButtonBox::ActionRole);
	QPushButton *evo = bb->addButton("Propose &Filter Rules…",
	                                  QDialogButtonBox::ActionRole);
	QPushButton *con = bb->addButton("&Cookie Banner Rule…",
	                                  QDialogButtonBox::ActionRole);
	QPushButton *rec = bb->addButton("Just &Record It", QDialogButtonBox::AcceptRole);
	rec->setDefault(true);

	// Nothing to propose a network rule from, so do not offer to.
	evo->setEnabled(!report.suspects.isEmpty());
	evo->setToolTip(report.suspects.isEmpty()
	                    ? QStringLiteral("Nothing ad-shaped was seen on this "
	                                      "page, so there is nothing to propose "
	                                      "a network rule against.")
	                    : QStringLiteral("Ask for filter rules against the "
	                                      "addresses above."));

	connect(zap, &QPushButton::clicked, this,
	        [this] { m_chosen = action::zap; accept(); });
	connect(evo, &QPushButton::clicked, this,
	        [this] { m_chosen = action::evolve; accept(); });
	connect(con, &QPushButton::clicked, this,
	        [this] { m_chosen = action::consent; accept(); });
	connect(rec, &QPushButton::clicked, this,
	        [this] { m_chosen = action::recorded; accept(); });
	box->addWidget(bb);

	resize(620, 380);
}
