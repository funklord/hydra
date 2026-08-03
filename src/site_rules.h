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
struct site_rule {
	// "container" | "reject" | "accept" — the consent banner's parts — and
	// "detector", a script whose only job is to notice we block ads. One store
	// rather than one per feature: they are the same kind of thing (a small,
	// perishable fact about how sites behave), they go stale for the same
	// reason, and they are meant to travel together. A second rule file would
	// be a second provenance model to keep honest and a second thing to send.
	QString kind;
	QString value;            // a CSS selector, or a button-label regex
	QString host;             // empty = generic
	QString note;             // where it came from, in words
	bool    builtin = false;  // shipped in the binary
	bool    promote = false;  // generic + learned: fold into the C++ defaults
	// Came from somebody else's file. Kept distinct from both of the above
	// because the three have different standing: a built-in was reviewed by
	// whoever ships the program, a learned rule was accepted by the person
	// sitting here, and an imported one has been vouched for by neither.
	bool    imported = false;
	QString origin;           // where an imported rule came from, in words

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
class site_rules {
public:
	// The built-in set: what ships in the binary, marked as such.
	static site_rules defaults();

	void add(const site_rule &r);
	const QList<site_rule> &all() const { return m_rules; }

	// Everything that applies to a host: the generic rules plus that host's own.
	site_rules for_host(const QString &host) const;

	// **Generic rules that were learned rather than shipped.** These are the
	// ones to fold into `defaults()` for the next release — a rule that works
	// on any site does not belong in one user's file, and once it is a built-in
	// it costs nothing to carry and everyone gets it. Listing them is the whole
	// mechanism: the flag is set when the rule is added, and this is how a
	// maintainer finds them without reading the file.
	QList<site_rule> promotable() const;

	// The detector names, for the anti-adblock notice. Same corpus, same
	// provenance, same file — a learned one is generic for exactly the reason a
	// button label is: it describes a script, not a site.
	QStringList detectors() const;

	// --- Exchanging rule sets -------------------------------------------------
	//
	// Local for now: a file someone sends, not a protocol. That is the whole
	// transport decision deferred, and deliberately — what a received rule must
	// *prove* is the part that has to be right first, and it does not change
	// when the bytes eventually arrive over something else.
	//
	// **The threat is specific and worth naming.** A consent rule is a licence
	// to click buttons on pages the user is logged into. An `accept` pattern of
	// `^.*$` would press the first button on every banner-shaped thing on every
	// site — "Delete account" included. So an imported rule is not trusted for
	// being well-formed; it has to survive the same kind of breadth check §12.4
	// runs on a filter rule before that one is allowed to block anything.
	struct import_result {
		QList<site_rule> accepted;
		QStringList      refused;    // human-readable, one per rejected rule
	};

	// Everything learned here, as a document to hand to someone else. Built-ins
	// are left out: the recipient's program already has its own, and a copy
	// would go stale the day either side changes.
	QJsonObject export_learned() const;

	// Forget everything that came from elsewhere, keeping built-ins and what was
	// learned here. The safety valve the threat model asks for: if a rule set
	// turns out to be careless or hostile, "undo that import" has to be one
	// action rather than a hunt through a list, and it must not take the user's
	// own rules with it. Returns how many went.
	int forget_imported();

	// Judge a document from elsewhere. Nothing is added to this set — the caller
	// shows the result and adds what the user accepts, so importing can never be
	// something that happened while a dialog was opening.
	// `origin` is recorded on each accepted rule so the list can say where it
	// came from. It is the caller's label -- a file name, later perhaps a peer --
	// and never anything the document claims about itself.
	static import_result judge_import(const QJsonObject &doc,
	                                   const QString &origin = QString());

	// The breadth check, exposed because it is the whole safety story and
	// deserves to be testable on its own. Empty return means the rule is safe to
	// offer; otherwise it is the reason to refuse.
	static QString why_unsafe(const site_rule &r);

	// Kept for the exchange document, which is a different thing from storage:
	// `judge_import` reads what somebody else sent, and that is reviewed before
	// it is added rather than loaded.
	QJsonObject to_json() const;
	static site_rules from_json(const QJsonObject &o);

	// INI, with the old JSON read once so a format change costs nobody their
	// rules. See policy_engine, which moved the same way for the same reason.
	bool load(const QString &path);
	bool save(const QString &path) const;

	// The format this file used to be in, kept only to read what is on disk.
	bool load_json(const QString &path);

	// What the page-side script is handed: one JSON literal, so the script
	// carries no rules of its own that could drift from these.
	QString to_script_literal() const;

private:
	QList<site_rule> m_rules;
};
