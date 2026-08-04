// SPDX-License-Identifier: GPL-3.0-or-later
#include "site_policy_dialog.h"
#include "policy_engine.h"
#include "policy.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include <QFont>

using policy::feature;
using policy::setting;

namespace {

int setting_to_index(setting s) {
	switch (s) {
		case setting::allow: return 1;
		case setting::block: return 2;
		default:             return 0;  // unset
	}
}

setting index_to_setting(int i) {
	switch (i) {
		case 1:  return setting::allow;
		case 2:  return setting::block;
		default: return setting::unset;
	}
}

}  // namespace

site_policy_dialog::site_policy_dialog(policy_engine *engine, QWidget *parent)
  : QDialog(parent), m_engine(engine) {
	setWindowFlags(Qt::Popup);
	setWindowTitle("Site controls");

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(12, 12, 12, 12);
	outer->setSpacing(8);

	m_host_label = new QLabel(this);
	QFont hf = m_host_label->font();
	hf.setBold(true);
	m_host_label->setFont(hf);
	outer->addWidget(m_host_label);

	m_scope = new QComboBox(this);
	m_scope->addItem("This host");
	m_scope->addItem("This domain");
	m_scope->addItem("Global default");
	connect(m_scope, &QComboBox::currentIndexChanged,
	         this, &site_policy_dialog::on_scope_changed);
	outer->addWidget(m_scope);

	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::HLine);
	outer->addWidget(line);

	auto *grid = new QGridLayout;
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(4);
	m_combos.resize(policy::feature_count());
	for (int i = 0; i < policy::feature_count(); ++i) {
		const feature f = static_cast<feature>(i);
		grid->addWidget(new QLabel(policy::feature_label(f), this), i, 0);

		auto *combo = new QComboBox(this);
		combo->addItem("Default");
		combo->addItem("Allow");
		combo->addItem("Block");
		connect(combo, &QComboBox::currentIndexChanged,
		         this, [this, i](int) { on_feature_changed(i); });
		m_combos[i] = combo;
		grid->addWidget(combo, i, 1);
	}
	outer->addLayout(grid);
}

void site_policy_dialog::set_host(const QString &host) {
	m_host = host;
	m_host_label->setText(host.isEmpty() ? QStringLiteral("(no page)") : host);
	m_scope->setItemText(1, "This domain (*." + policy_engine::etld_plus_one(host) + ")");
	repopulate();
}

QString site_policy_dialog::current_pattern() const {
	switch (m_scope->currentIndex()) {
		case 0:  return m_host;
		case 1:  return "*." + policy_engine::etld_plus_one(m_host);
		default: return QString();  // global
	}
}

void site_policy_dialog::repopulate() {
	m_populating = true;
	const bool global = (m_scope->currentIndex() == 2);
	const QString pattern = current_pattern();
	for (int i = 0; i < policy::feature_count(); ++i) {
		const feature f = static_cast<feature>(i);
		setting s = global ? m_engine->global_default(f)
		                   : m_engine->setting_for(pattern, f);
		m_combos[i]->setCurrentIndex(setting_to_index(s));
	}
	m_populating = false;
}

void site_policy_dialog::on_scope_changed() {
	repopulate();
}

void site_policy_dialog::on_feature_changed(int feature_index) {
	if (m_populating || feature_index < 0 || feature_index >= m_combos.size())
		return;
	const feature f = static_cast<feature>(feature_index);
	const setting s = index_to_setting(m_combos[feature_index]->currentIndex());

	if (m_scope->currentIndex() == 2) {
		// Global default: unset is not meaningful, treat it as allow.
		m_engine->set_global_default(f, s == setting::unset ? setting::allow : s);
	} else {
		m_engine->set_setting(current_pattern(), f, s);
	}
	emit policy_changed();
}
