// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// One thing the consent blocker knows about banners.
//
// `host` empty means the rule is **generic** — it describes a shape banners
// take rather than a site. That distinction is the whole reason this carries
// provenance at all: a generic rule that was learned here is a candidate to be
// shipped as a built-in next release, and it is flagged as such rather than
// left to be noticed. A host-specific one never is; it belongs to that site and
// travels with the rule file, not with the binary.
struct consent_rule {
	QString kind;             // "container" | "reject" | "accept"
	QString value;            // a CSS selector, or a button-label regex
	QString host;             // empty = generic
	QString note;             // where it came from, in words
	bool    builtin = false;  // shipped in the binary
	bool    promote = false;  // generic + learned: fold into the C++ defaults

	bool generic() const { return host.isEmpty(); }
};

// What the consent blocker knows, kept as data rather than baked into the
// script.
//
// The reason is the one §11.5 gives for extractors and §12 for filters, and it
// bites hardest here: a CMP's markup changes on its own schedule, so anything
// pinned to `#onetrust-banner-sdk` is a release-and-rebuild away from being
// useless. Rules that live in a file can be edited, diffed, and — the direction
// this is heading — **exchanged between users**. Nothing is shared yet. What
// matters now is that the unit of sharing already exists and has a provenance
// field, because retrofitting one onto a corpus people have already traded is
// how you end up unable to tell a rule you shipped from a rule a stranger sent.
//
// The built-in set is deliberately thin. A long vendor list looks like
// thoroughness and is really maintenance debt; the generic detector is what has
// to carry unknown banners, and a vendor entry earns its place only where the
// generic pass demonstrably fails.
class consent_rules {
public:
	// The built-in set: what ships in the binary, marked as such.
	static consent_rules defaults();

	void add(const consent_rule &r);
	const QList<consent_rule> &all() const { return m_rules; }

	// Everything that applies to a host: the generic rules plus that host's own.
	consent_rules for_host(const QString &host) const;

	// **Generic rules that were learned rather than shipped.** These are the
	// ones to fold into `defaults()` for the next release — a rule that works
	// on any site does not belong in one user's file, and once it is a built-in
	// it costs nothing to carry and everyone gets it. Listing them is the whole
	// mechanism: the flag is set when the rule is added, and this is how a
	// maintainer finds them without reading the file.
	QList<consent_rule> promotable() const;

	QJsonObject to_json() const;
	static consent_rules from_json(const QJsonObject &o);

	bool load(const QString &path);
	bool save(const QString &path) const;

	// What the page-side script is handed: one JSON literal, so the script
	// carries no rules of its own that could drift from these.
	QString to_script_literal() const;

private:
	QList<consent_rule> m_rules;
};
