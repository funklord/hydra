#include "filter_dialog.h"
#include "ai_provider.h"
#include "filter_signals.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

const char *k_system_prompt =
  "You write ad-blocking filter rules in EasyList / uBlock Origin syntax.\n"
  "You are given the page's URL and a list of requests that were NOT blocked "
  "but look like advertising or tracking.\n\n"
  "Reply with one rule per line and nothing else — no prose, no code fences. "
  "Prefix each line with a short reason and a pipe, like:\n"
  "  blocks the ad server | ||ads.example.com^\n\n"
  "Rules:\n"
  "1. Prefer narrow network rules (||host^) over broad substring matches.\n"
  "2. Never write a rule matching a bare TLD, and never one matching the "
  "page's own origin — it would break the page.\n"
  "3. Cosmetic rules must be domain-scoped (example.com##.ad) and must not "
  "hide a generic tag such as div or img.\n";

}  // namespace

filter_dialog::filter_dialog(filter_signals *signals_source, filter_list *list,
                              ai_provider *provider, const QString &site_host,
                              const picked_element &picked, QWidget *parent)
  : QDialog(parent), m_signals(signals_source), m_list(list),
    m_provider(provider), m_site(site_host), m_picked(picked) {
	setWindowTitle("Evolve ad filters");
	resize(820, 560);
	build_ui();

	connect(m_provider, &ai_provider::finished, this, &filter_dialog::on_reply);
	connect(m_provider, &ai_provider::failed,   this, &filter_dialog::on_failed);

	// The payload is the sec 12.2 context: page URL plus the requests that slipped
	// through. Personal data is not in it because only URLs of third-party
	// ad-shaped requests are collected in the first place.
	const QStringList suspects = m_signals->suspects_for(m_site);
	QString payload = "Page: " + m_site + "\n";
	if (m_picked.is_valid()) {
		// The user pointed at the ad, so the model gets the element itself --
		// this is what makes a *cosmetic* rule proposable at all (sec 12.1/sec 12.2).
		payload += "\nThe user marked this element as an ad:\n";
		payload += "  selector: " + m_picked.selector + "\n";
		payload += "  tag: " + m_picked.tag + "\n";
		if (!m_picked.id.isEmpty())
			payload += "  id: " + m_picked.id + "\n";
		if (!m_picked.classes.isEmpty())
			payload += "  classes: " + m_picked.classes.join(' ') + "\n";
		payload += "  shape: " + m_picked.snippet + "\n";
	}
	payload += "\nRequests that were not blocked:\n";
	for (const QString &s : suspects)
		payload += "  " + s + "\n";
	m_payload->setPlainText(payload);

	if (suspects.isEmpty() && !m_picked.is_valid())
		m_status->setText("Nothing ad-shaped slipped through on this page, and "
		                  "no element was picked — there is nothing to propose "
		                  "against.");
}

void filter_dialog::build_ui() {
	auto *outer = new QVBoxLayout(this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setText(
	  m_provider->is_external()
	    ? QString("<b>%1</b> — external provider. Review exactly what will "
	              "be sent; nothing leaves until you press Send.")
	              .arg(m_provider->name())
	    : QString("<b>%1</b> — local provider; nothing leaves this machine.")
	              .arg(m_provider->name()));
	outer->addWidget(m_status);

	m_pages = new QStackedWidget(this);

	m_payload = new QPlainTextEdit(this);
	m_payload->setReadOnly(true);
	m_payload->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_pages->addWidget(m_payload);

	m_rules = new QTreeWidget(this);
	m_rules->setColumnCount(3);
	m_rules->setHeaderLabels({"Rule", "Dry run", "Why"});
	m_rules->setRootIsDecorated(false);
	m_rules->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_pages->addWidget(m_rules);

	outer->addWidget(m_pages, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_send  = buttons->addButton("&Send", QDialogButtonBox::ActionRole);
	m_apply = buttons->addButton("&Accept Selected", QDialogButtonBox::AcceptRole);
	m_apply->setEnabled(false);
	m_send->setEnabled(!m_signals->suspects_for(m_site).isEmpty() ||
	                    m_picked.is_valid());
	// And not at all when the provider cannot answer -- the header says so
	// already, and a button beside an explanation of why it will fail is the
	// pattern this window spent a pass removing.
	if (m_send->isEnabled())
		gate_send(m_send, m_provider);
	connect(m_send,  &QPushButton::clicked, this, &filter_dialog::on_send);
	connect(m_apply, &QPushButton::clicked, this, &filter_dialog::on_accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void filter_dialog::on_send() {
	m_send->setEnabled(false);
	m_status->setText(QString("Asking %1…").arg(m_provider->name()));
	m_provider->send(QString::fromLatin1(k_system_prompt), m_payload->toPlainText());
}

void filter_dialog::on_failed(const QString &error) {
	m_send->setEnabled(true);
	m_status->setText("<b>Failed:</b> " + error.toHtmlEscaped());
}

void filter_dialog::on_reply(const QString &text) {
	m_send->setEnabled(true);

	QList<filter_rule> proposed;
	for (const QString &raw : text.split('\n')) {
		QString line = raw.trimmed();
		if (line.isEmpty())
			continue;
		QString note;
		const int pipe = line.indexOf('|');
		// "reason | rule" -- but `||host^` also starts with a pipe, so only
		// treat it as a reason when there is text before it.
		if (pipe > 0 && !line.startsWith("||")) {
			note = line.left(pipe).trimmed();
			line = line.mid(pipe + 1).trimmed();
		}
		filter_rule r;
		if (!filter_list::parse_rule(line, &r))
			continue;
		r.note = note;
		if (m_list->contains(r.text))
			continue;   // already have it; de-dup on import (sec 12.5)
		proposed << r;
	}

	if (proposed.isEmpty()) {
		m_status->setText("<b>Nothing usable:</b> the reply contained no rules.");
		return;
	}
	show_proposals(proposed);
}

void filter_dialog::show_proposals(const QList<filter_rule> &rules) {
	m_rules->clear();
	m_accepted.clear();

	const QStringList observed = m_signals->observed_for(m_site);
	int rejected = 0;

	for (const filter_rule &r : rules) {
		const dry_run sim = filter_list::evaluate(r, observed, m_site, m_picked);

		auto *row = new QTreeWidgetItem(m_rules);
		row->setText(0, r.text);
		row->setText(2, r.note);
		row->setFlags(row->flags() | Qt::ItemIsUserCheckable);

		if (sim.rejected) {
			// Statically unsafe: shown so the user can see what was proposed
			// and why it was refused, but not selectable (sec 12.4).
			++rejected;
			row->setText(1, "rejected — " + sim.reason);
			row->setCheckState(0, Qt::Unchecked);
			row->setFlags(row->flags() & ~Qt::ItemIsUserCheckable & ~Qt::ItemIsEnabled);
			continue;
		}

		if (r.cosmetic) {
			row->setText(1, sim.cosmetic_checked
			                    ? (sim.cosmetic_hits
			                           ? QString("hides the element you picked")
			                           : QString("does NOT hide the element you picked"))
			                    : QString("cosmetic — nothing picked to check against"));
		} else {
			row->setText(1, QString("would block %1 of %2 observed request(s)")
			                    .arg(sim.would_block.size()).arg(observed.size()));
		}
		// A rule that matches nothing observed is not obviously wrong, but it
		// is unproven -- leave it unticked so accepting it is a deliberate act.
		// Pre-tick only what is demonstrably doing something: a rule that
		// matched nothing, or a cosmetic rule that misses the picked element,
		// is unproven and stays for the user to decide.
		const bool proven = r.cosmetic ? (sim.cosmetic_checked && sim.cosmetic_hits)
		                               : !sim.would_block.isEmpty();
		row->setCheckState(0, proven ? Qt::Checked : Qt::Unchecked);
		for (const QString &u : sim.would_block)
			new QTreeWidgetItem(row, QStringList{u});
		m_accepted << r;
	}

	m_rules->resizeColumnToContents(1);
	m_pages->setCurrentIndex(1);
	m_apply->setEnabled(!m_accepted.isEmpty());
	m_status->setText(QString("%1 rule(s) proposed, %2 rejected as unsafe. "
	                          "Expand a row to see what it would block.")
	                      .arg(rules.size()).arg(rejected));
}

void filter_dialog::on_accept() {
	int added = 0;
	for (int i = 0; i < m_rules->topLevelItemCount(); ++i) {
		QTreeWidgetItem *row = m_rules->topLevelItem(i);
		if (row->checkState(0) != Qt::Checked)
			continue;
		for (const filter_rule &r : m_accepted) {
			if (r.text == row->text(0)) {
				m_list->add(r);
				++added;
				break;
			}
		}
	}
	m_status->setText(QString("Accepted %1 rule%2.").arg(added).arg(added == 1 ? "" : "s"));
	accept();
}
