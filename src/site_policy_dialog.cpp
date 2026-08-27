// SPDX-License-Identifier: GPL-3.0-or-later
#include "site_policy_dialog.h"
#include "policy_engine.h"
#include "policy.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QStandardItemModel>
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
	m_scope->setObjectName("scope");
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
		// Text set by `refresh_default_labels()`, because what this entry
		// means depends on the global default and that changes under us.
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

	// **With no page there is no site to have a rule about**, and the two site
	// scopes then resolve to an empty pattern -- which `set_setting` will
	// happily store, leaving a rule keyed on nothing that matches nothing and
	// can only be found by reading policy.ini. So they are disabled and the
	// scope falls back to the global defaults, which are still meaningful and
	// still worth being able to edit from here.
	const bool have_site = !host.isEmpty();
	m_host_label->setText(have_site
	                          ? host
	                          : QStringLiteral("No page open \u2014 global defaults"));
	for (int i = 0; i < 2; ++i) {
		// Disabled through the model, which is how a QComboBox greys one entry
		// rather than removing it: the choice stays visible, so it is clear
		// that per-site rules exist and simply have nothing to apply to.
		if (auto *m = qobject_cast<QStandardItemModel *>(m_scope->model()))
			if (QStandardItem *it = m->item(i))
				it->setEnabled(have_site);
	}
	if (!have_site)
		m_scope->setCurrentIndex(2);

	m_scope->setItemText(1, have_site
	                            ? "This domain (*." +
	                                  policy_engine::etld_plus_one(host) + ")"
	                            : QStringLiteral("This domain"));
	repopulate();

	// **A popup does not hand focus to a child, and a dialog does.** With
	// `Qt::Popup` set, nothing in here had it -- so this panel of fifteen
	// controls opened with no focus ring anywhere and no entry point for
	// anybody not using a pointer. The scope selector is the right place to
	// land: it decides what everything below it applies to, so reading it
	// first is the order the panel is meant to be used in.
	m_scope->setFocus(Qt::PopupFocusReason);
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
	refresh_default_labels();
	m_populating = false;
}

// **"Default" on its own does not tell anybody anything.** Every other entry
// in these boxes states an outcome -- Allow, Block -- and the one that is
// selected for most sites most of the time stated only that a choice had not
// been made. Whether that meant the scripts ran was a fact held one dialog
// away, on the settings page, per feature, and the shield is exactly where
// somebody is standing when they want to know.
//
// It cannot be written once at construction: it is the *global default* that
// decides, that is editable from this same dialog's third scope and from the
// settings page, and a label baked in at build time would go stale the moment
// either changed.
void site_policy_dialog::refresh_default_labels() {
	const bool global = (m_scope->currentIndex() == 2);
	for (int i = 0; i < policy::feature_count() && i < m_combos.size(); ++i) {
		const feature f = static_cast<feature>(i);
		const setting d = m_engine->global_default(f);
		// "allowed"/"blocked" rather than "enabled"/"disabled": the entries
		// below it say Allow and Block, and one concept wants one word.
		m_combos[i]->setItemText(0, d == setting::block
		                                 ? QStringLiteral("Default (blocked)")
		                                 : QStringLiteral("Default (allowed)"));

		// In the global scope this entry is what a *site* falls back to, so
		// offering it here is a setting pointing at itself -- the settings
		// page declines to offer it at all for that reason, and this dialog
		// used to accept it and quietly turn it into Allow. Greyed rather than
		// removed, the way the scope entries above are, so the row does not
		// change shape as the scope changes.
		if (auto *m = qobject_cast<QStandardItemModel *>(m_combos[i]->model()))
			if (QStandardItem *it = m->item(0))
				it->setEnabled(!global);
	}
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
		// Global default: unset is not meaningful, treat it as allow. The
		// entry is greyed in this scope so it cannot normally be chosen; this
		// stays as the belt to that braces.
		m_engine->set_global_default(f, s == setting::unset ? setting::allow : s);
		// What every site's "Default" resolves to has just moved.
		refresh_default_labels();
	} else {
		// Belt and braces against the case above: a site rule with no site is
		// not a rule, and storing one is worse than refusing it because it
		// persists and matches nothing.
		const QString pattern = current_pattern();
		if (pattern.isEmpty())
			return;
		m_engine->set_setting(pattern, f, s);
	}
	emit policy_changed();
}
