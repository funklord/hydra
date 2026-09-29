#pragma once

#include "policy.h"

#include <QObject>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

// Holds all per-site rules and resolves the effective decision for a feature on
// a host (architecture doc sec 7.1/sec 7.2). Consulted by the interceptor, the cookie
// filter, per-page settings, and permission handling.
//
// **Thread note, and the previous one was an argument of the wrong kind.** It
// said reads "tolerate a stale snapshot", which is a claim about *when* a value
// was written offered in defence of a *span of memory*: `effective_setting`
// walks `m_rules` by reference while `set_setting` can `push_back` into it, and
// a QVector that reallocates frees the buffer the reader is walking. Reading a
// rule one edit out of date is harmless; reading a freed one is not, and no
// amount of tolerance for staleness covers it.
//
// The boundary is real, and it is Android's rather than the desktop's.
// Measured on Qt 6.8.2 with `try_adblock_fix`: `interceptRequest` runs on the
// **main** thread (`same=1`, `Qt mainThread`), so the desktop crosses nothing
// today. `HydraWebView.shouldInterceptRequest` says in as many words that it is
// called "on a network thread", and it reaches `request_filter::decide` and so
// this class through `android_view::should_block`, while the UI thread is free
// to toggle a per-site permission.
//
// So `m_lock` guards the rule set, the same way and for the same boundary as
// `filter_list`'s. Reads take it shared, mutations take it exclusively, and
// `changed()` is emitted after it is released -- a signal delivered under a
// lock invites a slot that asks this class a question back.
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

	// **UI thread only.** This hands out a reference into the vector, so it must
	// not be held while another thread could be adding a rule -- exactly the
	// contract `filter_list::rules()` carries, and for the same reason.
	// `effective_setting` is the cross-thread entry point and takes the lock.
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

	// **The `_unlocked` half exists so nothing takes `m_lock` twice.**
	// `QReadWriteLock` is not recursive here, and the callers that would do it
	// are ordinary: `effective_setting` needs the global default, `save` needs
	// both, and `load` reaches `load_json` which sets defaults. Each public
	// entry point acquires once and calls these.
	policy::setting global_default_unlocked(policy::feature f) const;
	void    set_global_default_unlocked(policy::feature f, policy::setting s);
	policy::setting effective_setting_unlocked(policy::feature f,
	                                            const QString &host) const;
	bool    load_unlocked(const QString &path);

	QVector<rule> m_rules;
	quint64       m_global_defaults = 0;
	// Guards m_rules and m_global_defaults across the one thread boundary this
	// class has. Mutable so the const read path can take it.
	mutable QReadWriteLock m_lock;
};
