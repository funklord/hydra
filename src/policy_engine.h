// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "policy.h"

#include <QObject>
#include <QString>
#include <QVector>

// Holds all per-site rules and resolves the effective decision for a feature on
// a host (architecture doc §7.1/§7.2). Consulted by the interceptor, the cookie
// filter, per-page settings, and permission handling. Thread note: reads happen
// from the interceptor which Qt may call off the UI thread; the rule set is
// only mutated from the UI thread and reads tolerate a stale snapshot.
class policy_engine : public QObject {
	Q_OBJECT
public:
	struct rule {
		QString pattern;   // exact host, "*.domain.tld", or "*"
		quint64 bits = 0;  // packed per-feature settings
	};

	explicit policy_engine(QObject *parent = nullptr);

	// Effective decision. true = feature permitted, false = blocked.
	bool is_allowed(policy::feature f, const QString &host) const;
	policy::setting effective_setting(policy::feature f, const QString &host) const;

	// Global default per feature (always allow or block, never unset).
	policy::setting global_default(policy::feature f) const;
	void            set_global_default(policy::feature f, policy::setting s);

	// Per-pattern rule editing.
	policy::setting setting_for(const QString &pattern, policy::feature f) const;
	void            set_setting(const QString &pattern, policy::feature f, policy::setting s);

	const QVector<rule> &rules() const { return m_rules; }

	// INI. A JSON file at the old path is read once and rewritten as INI on the
	// next save, so nobody has to run a migration or lose their rules to one.
	bool load(const QString &path);
	bool save(const QString &path) const;

	// Best-effort registrable domain (last two labels; no public-suffix list).
	static QString etld_plus_one(const QString &host);

signals:
	void changed();

private:
	// The format this file used to be in, kept only to read what is already on
	// disk. Nothing writes it.
	bool load_json(const QString &path);

	static bool match_pattern(const QString &pattern, const QString &host, int &specificity);
	rule       *find_rule(const QString &pattern);
	const rule *find_rule(const QString &pattern) const;

	QVector<rule> m_rules;
	quint64       m_global_defaults = 0;
};
