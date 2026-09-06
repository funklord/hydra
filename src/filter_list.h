#pragma once

#include "element_picker.h"

#include <QHash>
#include <QList>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>

// One filter rule in EasyList / uBO syntax (architecture doc sec 12.3).
struct filter_rule {
	QString text;        // "||ads.example.com^" or "example.com##.ad-banner"
	bool    cosmetic = false;
	QString scope;       // domain for a site-specific rule, empty = generic
	QString note;        // why it was proposed, if it came from the AI
};

// What a rule would do, computed before it is accepted (architecture doc sec 12.4).
struct dry_run {
	QStringList would_block;   // observed URLs this rule matches
	bool        rejected = false;
	QString     reason;        // set when statically rejected as too broad
	// For a cosmetic rule: does it actually hit the element the user picked?
	// `checked` is false when there was nothing picked to check against.
	bool        cosmetic_checked = false;
	bool        cosmetic_hits    = false;
};

// The AI/user-authored filter list, kept deliberately apart from any imported
// EasyList so a scheduled upstream update never clobbers custom rules (sec 12.5).
//
// The validation half of sec 12.4 is the safety core here, and it is the same
// shape as the reorganizer's invariant check: nothing reaches the accept UI
// until a static breadth check and a simulation against real observed requests
// have both run. A rule that would hide a generic tag globally, match a whole
// TLD, or block a first-party essential is rejected outright rather than shown
// with a warning, because a filter that breaks a page is indistinguishable from
// a broken browser to the person using it.
class filter_list {
public:
	bool load(const QString &path);
	bool save(const QString &path) const;

	// UI thread only: this hands out a reference into the list, so it must not be
	// held while another thread could be adding a rule. `blocks()` is the
	// cross-thread entry point and takes a read lock of its own.
	const QList<filter_rule> &rules() const { return m_rules; }
	bool contains(const QString &text) const;
	void add(const filter_rule &r);

	// Take one back out, by its exact text. Accepting a rule was reversible only
	// by hand-editing `filters-ai.txt` until now, which is a poor answer for a
	// list built by accepting AI proposals one at a time -- the whole design
	// assumes some of them will turn out wrong. Returns false when there was no
	// such rule, so a caller can tell "removed" from "never there".
	bool remove(const QString &text);

	// Does this URL match any accepted network rule?
	bool blocks(const QString &url, const QString &site_host) const;

	// Static breadth check plus simulation against `observed` (sec 12.4).
	static dry_run evaluate(const filter_rule &r, const QStringList &observed,
	                         const QString &site_host,
	                         const picked_element &picked = picked_element{});

	// Would this cosmetic selector hide the picked element?
	//
	// Approximate on purpose: matching a full selector needs a live DOM, which
	// is not available here. This checks the selector's *rightmost* compound --
	// the part that names the element itself -- against the element's tag, id
	// and classes. That answers the question the user actually has ("will this
	// hit the thing I zapped?") and never claims more than it checked.
	static bool cosmetic_matches(const QString &selector, const picked_element &e);

	// Parses one line of EasyList-ish syntax. Returns false on a comment or
	// something unparseable.
	static bool parse_rule(const QString &line, filter_rule *out);

	// Does a single network rule pattern match a URL?
	static bool matches(const QString &pattern, const QString &url);

	// **Whether a cosmetic selector can be put in a stylesheet without being
	// able to say anything else.**
	//
	// The selectors are delivered to the page and end up in CSS. A selector
	// carrying `}` closes the rule it was placed in, and everything after it
	// is then arbitrary CSS on that site -- `@import` from a remote host, a
	// full-page overlay, `background: url(...)` on an attribute selector to
	// send what somebody typed somewhere else. None of that is code
	// execution, and all of it is more than hiding an element.
	//
	// Nothing can reach this today: the only source of a cosmetic rule is the
	// evolution loop, which a person accepts one rule at a time. It stops
	// being latent the moment rules arrive from anywhere else, which is the
	// point of the sharing work -- so the check lands before the sharing does,
	// not after.
	//
	// Empty means safe; otherwise the reason.
	static QString why_selector_unsafe(const QString &selector);

private:
	bool contains_locked(const QString &text) const;

	// --- The index, which is what makes a real list usable -------------
	//
	// `blocks()` was a linear scan calling `matches()` on every rule, and
	// `matches()` copied the pattern and built a whole `QUrl` **per rule**. On
	// the handful of rules the evolution loop produces that is free. On a
	// subscription list it is not: every request would parse the URL tens of
	// thousands of times.
	//
	// Two structures, chosen from what the syntax actually contains rather
	// than from a general idea about matching:
	//
	//  * **Host-anchored rules** (`||ads.example^`) are the large majority and
	//    they are an exact host test. Keyed by host, and a URL is checked by
	//    walking its own suffixes -- `a.b.example` asks for `a.b.example`,
	//    `b.example`, `example`. Three or four hash lookups whatever the list
	//    holds.
	//  * **Everything else** is a substring or a wildcard, and is bucketed by
	//    one token taken from the pattern: its longest run of letters and
	//    digits. A URL is tokenised the same way and only the buckets its own
	//    tokens name are tested. This is what every real blocker does, and the
	//    reason it works is that a pattern which cannot contribute a token is
	//    rare -- those go in `m_untokenised` and are always tested.
	//
	// Rebuilt whenever the rules change, under the same write lock.
	struct compiled {
		enum class kind { host, substring, wildcard } k = kind::substring;
		QString     host;    // for `host`: the bare domain, lowercased
		QString     needle;  // for `substring`: the pattern as written
		QStringList parts;   // for `wildcard`: the pieces between the stars
	};
	void reindex();
	// **One rule, appended.** `add()` used to call `reindex()`, which rebuilt
	// the whole index per rule -- fine for the handful the evolution loop
	// produces, quadratic for a subscription. Measured: adding 25,000 rules
	// that way took minutes; the benchmark that found it is in `test_bundle`.
	void index_one(int i);
	static QString token_of(const QString &pattern);
	static QStringList tokens_in(const QString &url);

	QList<compiled>            m_compiled;    // parallel to m_rules
	QHash<QString, QList<int>> m_by_host;
	QHash<QString, QList<int>> m_by_token;
	QList<int>                 m_untokenised;

	QList<filter_rule> m_rules;
	// Guards m_rules across the one thread boundary this class has: rules are
	// added and removed on the UI thread while the interceptor asks blocks() on
	// Qt WebEngine's. Mutable so the const read path can take it.
	mutable QReadWriteLock m_lock;
};
