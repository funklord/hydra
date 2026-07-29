// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// One filter rule in EasyList / uBO syntax (architecture doc §12.3).
struct filter_rule {
	QString text;        // "||ads.example.com^" or "example.com##.ad-banner"
	bool    cosmetic = false;
	QString scope;       // domain for a site-specific rule, empty = generic
	QString note;        // why it was proposed, if it came from the AI
};

// What a rule would do, computed before it is accepted (architecture doc §12.4).
struct dry_run {
	QStringList would_block;   // observed URLs this rule matches
	bool        rejected = false;
	QString     reason;        // set when statically rejected as too broad
};

// The AI/user-authored filter list, kept deliberately apart from any imported
// EasyList so a scheduled upstream update never clobbers custom rules (§12.5).
//
// The validation half of §12.4 is the safety core here, and it is the same
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

	const QList<filter_rule> &rules() const { return m_rules; }
	bool contains(const QString &text) const;
	void add(const filter_rule &r);

	// Does this URL match any accepted network rule?
	bool blocks(const QString &url, const QString &site_host) const;

	// Static breadth check plus simulation against `observed` (§12.4).
	static dry_run evaluate(const filter_rule &r, const QStringList &observed,
	                         const QString &site_host);

	// Parses one line of EasyList-ish syntax. Returns false on a comment or
	// something unparseable.
	static bool parse_rule(const QString &line, filter_rule *out);

	// Does a single network rule pattern match a URL?
	static bool matches(const QString &pattern, const QString &url);

private:
	QList<filter_rule> m_rules;
};
