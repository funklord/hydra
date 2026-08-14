// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "site_extractor.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <functional>

// The helper tier (architecture doc sec 11.5.1).
//
// The pure tier hands a script the request log and takes an address back. It
// cannot help where the address is computed in page JS and never appears in the
// log, and it cannot follow a master playlist to the variant a player would
// pick. Both need the script to *look* at something, which means opening a
// sandbox that is otherwise empty on purpose.
//
// **Follow, not fabricate.** The rule that keeps this safe is the pure tier's
// rule generalised from the address a script returns to every address it
// touches: it may read or return an address only if that address was observed
// in the request log, or appeared inside a document already fetched under this
// same rule. The allowlist grows from bytes the app fetched; it never grows
// from a string the script composed. A script therefore still cannot name a
// destination of its own, which is what leaves it with no channel to send
// anything anywhere.

// What one helper call did. Shown to the user at review, and stored beside the
// script so a later run that behaves differently is a diff rather than a
// mystery.
struct helper_call {
	QString verb;       // "head" | "text" | "log"
	QString target;
	bool    allowed = false;
	int     status  = 0;
	qint64  bytes   = 0;
	QString outcome;    // a content type, a byte count, or why it was refused
};

// Enforced in C++ and not negotiable from script. A breach is not a partial
// answer: the run yields nothing and names the budget it hit.
struct helper_budget {
	int    max_calls   = 8;
	qint64 max_bytes   = 512 * 1024;
	int    deadline_ms = 10000;
};

// What a fetch produced. Deliberately not a QNetworkReply: the fetcher is
// injected, so the whole tier is exercised offline with a fake one and the only
// component that touches a network -- or a thread -- is the real implementation.
struct fetch_result {
	bool       reached = false;
	int        status  = 0;
	QString    content_type;
	QByteArray body;
	QString    error;
};

// url, byte cap, timeout. Blocking: the script that calls it is synchronous,
// because QJSEngine is, and because a promise-returning surface would force
// generated code into a shape models get wrong far more often than a
// straight-line loop (project.md records four prompt iterations spent getting a
// synchronous two-argument function to land reliably).
using helper_fetcher = std::function<fetch_result(const QUrl &, qint64, int)>;

// Which addresses a script is allowed to touch.
class helper_allowlist {
public:
	// Everything the page requested. The starting set, and the only one that
	// exists before the script does anything.
	void observe(const QList<evidence_request> &evidence);

	bool allows(const QUrl &url) const;
	int  size() const { return m_allowed.size(); }

	// Grow the set from a document the app fetched, resolving relative
	// references against the address it came from -- which is how a master
	// playlist legitimately leads to its variants. Returns how many new
	// addresses it learned.
	//
	// Only called with bodies fetched under `allows()`, so the set can only
	// ever grow along links that genuinely exist in fetched documents.
	int learn_from(const QUrl &base, const QByteArray &body);

	// Addresses are compared with the fragment and any trailing slash removed,
	// matching the gate's own normalisation: a fragment never reaches a server,
	// and a trailing slash is the same resource.
	static QString normalise(const QUrl &url);

private:
	QSet<QString> m_allowed;
};

// The object a script sees as `hydra`. Every call is checked against the
// allowlist and the budget before anything happens, and recorded either way.
class helper_host : public QObject {
	Q_OBJECT
public:
	helper_host(helper_allowlist *allow, helper_fetcher fetch,
	             helper_budget budget, QObject *parent = nullptr);

	// The addresses worth asking about, best first, as ranked by the caller.
	//
	// Free: it is data already computed, with no I/O behind it. It exists
	// because a budget is spent by *order*, and a script scanning the request
	// log left to right spends it on stylesheets and beacons before reaching
	// the video -- measured against a real capture, where eight calls went to
	// page furniture and the manifest was never reached. A script that starts
	// here spends its budget where the answer is.
	Q_INVOKABLE QStringList candidates() const { return m_candidates; }
	void set_candidates(const QStringList &ranked) { m_candidates = ranked; }

	// Note for the transcript. Costs a call slot, because an unbounded log is
	// its own denial of service.
	Q_INVOKABLE void log(const QString &message);

	// What the server says an address is, without pulling its body: status,
	// content type, and the sec 10 classification. Empty map if refused.
	Q_INVOKABLE QVariantMap head(const QString &url);

	// The body, capped, as text. Empty string if refused. Anything it returns
	// has already extended the allowlist with the addresses inside it.
	Q_INVOKABLE QString text(const QString &url);

	const QList<helper_call> &transcript() const { return m_calls; }

	// The gate needs this: once a script may follow a manifest, the address it
	// returns can legitimately be one that was never requested by the page, and
	// "was it observed" stops being the right question. "May it be followed" is.
	const helper_allowlist *allowlist() const { return m_allow; }

	// Set when a budget was exceeded or a disallowed address was reached. The
	// gate turns this into a named rejection rather than letting a script that
	// misbehaved return an answer anyway.
	bool    breached() const { return !m_breach.isEmpty(); }
	QString breach() const { return m_breach; }

	qint64 bytes_used() const { return m_bytes; }
	int    calls_used() const { return m_calls.size(); }

	// Notes are bounded on their own count, not on the fetch budget: they cost
	// nothing to serve, and charging them the same made a script that explained
	// itself run out of room to work.
	static constexpr int k_max_notes = 20;

	// Starts the deadline. Called once, immediately before the script runs.
	void begin();

private:
	// Shared preamble: budget, then allowlist. Returns false and records the
	// refusal if the call may not proceed.
	bool permit(const QString &verb, const QUrl &url, helper_call *out);
	void finish(helper_call call);

	helper_allowlist *m_allow = nullptr;
	helper_fetcher    m_fetch;
	helper_budget     m_budget;
	QList<helper_call> m_calls;
	QString           m_breach;
	QStringList       m_candidates;
	qint64            m_bytes = 0;
	int               m_notes = 0;
	QElapsedTimer     m_clock;
};
