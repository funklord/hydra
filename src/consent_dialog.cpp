#include "consent_dialog.h"

#include "consent_blocker.h"
#include "empty_state.h"
#include "flow_layout.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

consent_dialog::consent_dialog(consent_blocker *blocker, const QString &rules_path,
                                QWidget *parent)
  : QDialog(parent), m_blocker(blocker), m_path(rules_path) {
	setWindowTitle("Cookie banners we could not answer");
	setObjectName("consent_dialog");
	resize(720, 420);

	auto *outer = new QVBoxLayout(this);

	m_intro = new QLabel(
	  "These pages showed a cookie banner that none of the current rules "
	  "matched, so it was left alone. Pick the button that <b>refuses</b> or "
	  "<b>accepts</b>, and that becomes a rule.<br><br>"
	  "A button label describes a shape banners take rather than one site, so "
	  "the rule applies everywhere and is flagged to be shipped as a built-in "
	  "— you are not fixing one page, you are proposing something everyone "
	  "would carry.");
	m_intro->setWordWrap(true);
	m_intro->setObjectName("intro");
	outer->addWidget(m_intro);

	m_list = new QTreeWidget;
	m_list->setObjectName("banners");
	// One column, because the second was never filled. It carried an empty
	// header and took most of the dialog's width, so every row of real content
	// was squeezed into the left third of a window that looked half broken.
	// Nothing wrote to it: only column 0 is ever set, for both the site rows
	// and the button rows beneath them.
	m_list->setColumnCount(1);
	m_list->setHeaderLabels({ "Site / button" });
	m_list->header()->setStretchLastSection(true);
	outer->addWidget(m_list, 1);

	m_status = new QLabel;
	m_status->setObjectName("status");
	m_status->setWordWrap(true);
	outer->addWidget(m_status);

	// **The two teaching buttons on their own row, in a `flow_layout`.**
	//
	// They were `ActionRole` buttons in the same `QDialogButtonBox` as Close,
	// which lays three buttons in one line and, when the space runs short,
	// squeezes every child equally. Their labels are sentences rather than
	// words -- they have to be, because the button *is* the instruction -- so
	// on a 360-pixel screen the box would not go below 389 and both of them
	// came out 123 pixels wide against labels needing 137 and 138. A button
	// reading "This button &refus" is not a button anybody can use, and the
	// dialog could not be made narrow enough to fit the phone at all.
	//
	// `flow_layout` wraps instead of squeezing, which is the same repair
	// `downloads_dialog` and `settings_dialog` already made for the same
	// reason. Close stays in its own box below: it is leaving rather than
	// teaching, and separating them says so. On a desktop the two still sit on
	// one line exactly as before.
	auto *row = new flow_layout;
	m_reject = new QPushButton("This button &refuses", this);
	m_accept = new QPushButton("This button &accepts", this);
	row->addWidget(m_reject);
	row->addWidget(m_accept);
	outer->addLayout(row);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_reject, &QPushButton::clicked, this,
	         [this] { setProperty("as", "reject"); on_accept_selected(); });
	connect(m_accept, &QPushButton::clicked, this,
	         [this] { setProperty("as", "accept"); on_accept_selected(); });
	outer->addWidget(buttons);

	connect(m_list, &QTreeWidget::itemSelectionChanged, this,
	         &consent_dialog::refresh_buttons);

	m_empty = new empty_state(m_list, this);
	m_empty->set_text("Nothing recorded.\n\nA banner is only listed here when "
	                   "it was found, looked like consent, and offered nothing "
	                   "any rule matched.");

	rebuild();
}

void consent_dialog::rebuild() {
	m_list->clear();
	// One row per banner, one child per button it offered. The children are what
	// can be turned into rules; the parent is only there to say where it came
	// from, since the same labels on two sites are still one rule.
	for (const QString &row : m_blocker->unhandled()) {
		const QStringList parts = row.split('\t');
		if (parts.size() < 2)
			continue;
		auto *top = new QTreeWidgetItem(m_list);
		top->setText(0, parts.first());
		top->setFirstColumnSpanned(true);
		top->setFlags(top->flags() & ~Qt::ItemIsSelectable);
		for (int i = 1; i < parts.size(); ++i) {
			auto *kid = new QTreeWidgetItem(top);
			kid->setText(0, parts[i]);
		}
		top->setExpanded(true);
	}
	// **The count under the table, the explanation inside it.** These were one
	// label under the list, so on the common case -- nothing recorded -- the
	// sentence explaining the blank table sat below it in small text, reading
	// as a footnote about the window rather than as the answer to the question
	// the blank table asks. The downloads and media dialogs both put theirs in
	// the viewport; this one did not, and looked it.
	if (m_list->topLevelItemCount() == 0)
		m_status->clear();
	else
		m_status->setText(QString("%1 banner(s) recorded.")
		                      .arg(m_list->topLevelItemCount()));
	refresh_buttons();
}

void consent_dialog::refresh_buttons() {
	// Only a button can become a rule, never the site row.
	const auto sel = m_list->selectedItems();
	const bool ok = sel.size() == 1 && sel.first()->parent() != nullptr;
	m_reject->setEnabled(ok);
	m_accept->setEnabled(ok);
}

void consent_dialog::on_accept_selected() {
	const auto sel = m_list->selectedItems();
	if (sel.size() != 1 || !sel.first()->parent())
		return;
	const QString label = sel.first()->text(0);
	const QString as = property("as").toString();

	// The rule is built from the label the page really offered, escaped on the
	// way in -- nothing here is typed.
	const site_rule r = m_blocker->rule_from_label(label, as);

	// And it is judged before it is kept, by the same check an imported rule
	// faces. A learned rule carries no host, so it applies to *every* site: a
	// banner whose button says "Yes" would teach a rule that presses "Yes"
	// everywhere, confirmation dialogs included. Being offered by the page is
	// not the same as being safe to generalise.
	const QString unsafe = site_rules::why_unsafe(r);
	if (!unsafe.isEmpty()) {
		m_status->setText(
		  QString("<b>Not learned:</b> a rule for \"%1\" %2. It would apply to "
		           "every site, not just this one.")
		      .arg(label.toHtmlEscaped(), unsafe.toHtmlEscaped()));
		return;
	}
	site_rules rules = m_blocker->rules();
	rules.add(r);
	m_blocker->set_rules(rules);

	const bool saved = m_path.isEmpty() ? false : rules.save(m_path);
	m_status->setText(
	  QString("Learned: <b>%1</b> means %2. It applies from the next page load, "
	           "and it is flagged to be folded into the built-in rules.%3")
	      .arg(label.toHtmlEscaped(), as,
	           saved ? QString() : QString(" <b>It could not be saved</b>, so "
	                                        "it will be gone when Hydra closes.")));
}
