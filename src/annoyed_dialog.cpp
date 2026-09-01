#include "annoyed_dialog.h"

#include "flow_layout.h"

#include "site_extractor.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QHash>
#include <QAbstractItemView>
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
		m_suspects->addItem("Nothing on the network looked ad-shaped — so this "
		                     "is likely cosmetic, a consent banner, or the site "
		                     "itself.");
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

	auto *hint = new QLabel(
	    "The report is kept either way. Pick a tool if one fits.", this);
	hint->setWordWrap(true);
	box->addWidget(hint);

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
