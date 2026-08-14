// SPDX-License-Identifier: GPL-3.0-or-later
#include "reorganize_dialog.h"
#include "ai_provider.h"
#include "node.h"
#include "tab_tree_model.h"
#include "tree_serializer.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

// Kept explicit rather than clever: the model is told the exact grammar back,
// and told the one rule the invariant check will enforce anyway (sec 9.4). Saying
// it up front turns most violations into non-events instead of repairs.
const char *k_system_prompt =
	"You reorganize a browser's tab tree. You are given an outline where every "
	"line is:\n"
	"  - [id] type | title | url | tags=a,b\n"
	"Indentation is two spaces per level. Reply with the SAME outline, "
	"reordered and re-nested as you see fit, and nothing else — no prose, no "
	"code fences.\n\n"
	"Rules:\n"
	"1. Every tab id in the input must appear exactly once in your output. "
	"Never drop, duplicate, or invent a tab.\n"
	"2. You may create new folders (give each a new unique id), rename "
	"folders, and move anything anywhere.\n"
	"3. Keep each line's id, type, url and tags byte-identical. Only folder "
	"titles may be changed.\n";

}  // namespace

reorganize_dialog::reorganize_dialog(tab_tree_model *model, ai_provider *provider,
                                      QWidget *parent)
  : QDialog(parent), m_model(model), m_provider(provider) {
	setWindowTitle("Reorganize tree");
	resize(760, 560);
	build_ui();

	connect(m_provider, &ai_provider::finished, this, &reorganize_dialog::on_reply);
	connect(m_provider, &ai_provider::failed,   this, &reorganize_dialog::on_failed);

	m_payload->setPlainText(tree_serializer::to_payload(m_model->root()));
}

reorganize_dialog::~reorganize_dialog() {
	if (m_provider)
		m_provider->cancel();
	delete m_proposal;   // the shadow tree never outlives the dialog
}

void reorganize_dialog::build_ui() {
	auto *outer = new QVBoxLayout(this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setText(
	  m_provider->is_external()
	    ? QString("<b>%1</b> — this is an external provider. Review exactly "
	              "what will be sent below; only ids, titles, URLs, types and "
	              "tags travel. Nothing is sent until you press Send.")
	              .arg(m_provider->name())
	    : QString("<b>%1</b> — local provider; nothing leaves this machine.")
	              .arg(m_provider->name()));
	outer->addWidget(m_status);

	m_pages = new QStackedWidget(this);

	// Page 0: the review-before-send gate.
	m_payload = new QPlainTextEdit(this);
	m_payload->setReadOnly(true);
	m_payload->setLineWrapMode(QPlainTextEdit::NoWrap);
	m_pages->addWidget(m_payload);

	// Page 1: the proposal, as individually toggleable changes.
	auto *diff_page = new QWidget(this);
	auto *diff_box  = new QVBoxLayout(diff_page);
	m_report = new QLabel(diff_page);
	m_report->setWordWrap(true);
	m_changes = new QListWidget(diff_page);
	diff_box->addWidget(m_report);
	diff_box->addWidget(m_changes, 1);
	m_pages->addWidget(diff_page);

	outer->addWidget(m_pages, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_send  = buttons->addButton("&Send", QDialogButtonBox::ActionRole);
	m_apply = buttons->addButton("&Apply Selected", QDialogButtonBox::AcceptRole);
	m_apply->setEnabled(false);
	gate_send(m_send, m_provider);
	connect(m_send,  &QPushButton::clicked, this, &reorganize_dialog::on_send);
	connect(m_apply, &QPushButton::clicked, this, &reorganize_dialog::on_accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void reorganize_dialog::on_send() {
	m_send->setEnabled(false);
	m_status->setText(QString("Asking %1…").arg(m_provider->name()));
	m_provider->send(QString::fromLatin1(k_system_prompt), m_payload->toPlainText());
}

void reorganize_dialog::on_failed(const QString &error) {
	m_send->setEnabled(true);
	m_status->setText("<b>Failed:</b> " + error.toHtmlEscaped());
}

void reorganize_dialog::on_reply(const QString &text) {
	m_send->setEnabled(true);

	delete m_proposal;
	m_proposal = tree_serializer::parse_proposal(text);
	if (!m_proposal) {
		m_status->setText("<b>Failed:</b> the reply contained no outline.");
		return;
	}

	// Nothing is shown until the invariant check has run (sec 9.4).
	const proposal_report rep = tree_diff::check_and_repair(m_model->root(), m_proposal);
	if (!rep.usable) {
		delete m_proposal;
		m_proposal = nullptr;
		m_status->setText("<b>Rejected:</b> " + rep.message.toHtmlEscaped());
		return;
	}

	m_change_list = tree_diff::compute(m_model->root(), m_proposal);
	m_report->setText(QString("<b>%1</b> %2 change(s) proposed. Untick anything "
	                          "you don't want.")
	                      .arg(rep.message.toHtmlEscaped())
	                      .arg(m_change_list.size()));
	show_diff();
}

void reorganize_dialog::show_diff() {
	m_changes->clear();
	for (const tree_change &c : m_change_list) {
		auto *item = new QListWidgetItem(c.summary, m_changes);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(c.accepted ? Qt::Checked : Qt::Unchecked);
	}
	m_pages->setCurrentIndex(1);
	m_apply->setEnabled(!m_change_list.isEmpty());
	m_status->setText("Reviewing proposal. The tree is unchanged until you apply.");
}

void reorganize_dialog::on_accept() {
	for (int i = 0; i < m_change_list.size() && i < m_changes->count(); ++i)
		m_change_list[i].accepted = (m_changes->item(i)->checkState() == Qt::Checked);

	const int applied = m_model->apply_reorganization(m_change_list);
	m_status->setText(QString("Applied %1 change%2.").arg(applied).arg(applied == 1 ? "" : "s"));
	accept();
}
