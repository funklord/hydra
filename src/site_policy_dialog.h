// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QVector>
#include <QString>

class QComboBox;
class QLabel;
class policy_engine;

// The compact per-site editor that drops down from the address-bar shield
// (architecture doc sec 7.4). A scope selector (this host / *.domain / global) and
// one tri-state combo per feature, all writing straight into the policy_engine.
class site_policy_dialog : public QDialog {
	Q_OBJECT
public:
	site_policy_dialog(policy_engine *engine, QWidget *parent = nullptr);

	void set_host(const QString &host);

signals:
	void policy_changed();

private slots:
	void on_scope_changed();
	void on_feature_changed(int feature_index);

private:
	QString current_pattern() const;   // empty string means "global"
	void    repopulate();
	// The first entry says what "no rule here" actually does, which depends on
	// the global default and so cannot be written once at construction.
	void    refresh_default_labels();

	policy_engine      *m_engine;
	QString             m_host;
	QLabel             *m_host_label = nullptr;
	QComboBox          *m_scope      = nullptr;
	QVector<QComboBox *> m_combos;
	bool                m_populating = false;
};
