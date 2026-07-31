// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "local_proxy.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

// What a candidate address turned out to be (architecture doc §10, §11.1).
struct probe_result {
	bool    reached = false;   // an HTTP response came back at all
	int     status  = 0;
	QString content_type;      // as the server declared it, for the record
	QString kind;              // "hls" | "dash" | "direct" | empty if unknown
	QString reason;            // what this concluded, in a sentence
	bool    disagreed = false; // the body says one thing, the header another
	QByteArray head;           // the opening bytes the verdict was drawn from
};

// The content-type tier (architecture doc §10).
//
// URL-shaped detection loses on real sites, and this project has the
// measurement rather than the suspicion: on the site that motivated §11.5 the
// manifest arrives as `cf-master.<digits>.txt?k=…`, its segments arrive as
// `.woff2`, and its init segment as `.woff`. Nothing in the address says
// "video". `media_detector::classify()` sees nothing, and four rounds of prompt
// work could not make an extractor find it either, because there was no signal
// in the evidence to find.
//
// So ask the server. Fetch the opening bytes with the page's own context — the
// same `stream_context` the proxy injects, because a naked fetch of these URLs
// returns 403 — and decide from what comes back.
//
// **The body outranks the Content-Type, deliberately.** A server that serves a
// master playlist as `text/plain` under a `.txt` name is not making a mistake;
// it is the disguise, and believing its header would reproduce exactly the
// failure this tier exists to fix. `#EXTM3U` in the first bytes settles the
// question whatever the header claims, and the disagreement is worth reporting
// rather than smoothing over.
//
// This is *not* the browser-through-the-proxy half of §10. Inspecting every
// response would mean terminating TLS with a certificate the browser must
// trust, which the design does not address. This fetches one address the user
// is already being asked about, which needs none of that.
class stream_probe : public QObject {
	Q_OBJECT
public:
	explicit stream_probe(QObject *parent = nullptr);

	// Fetch the opening bytes of `url` with `ctx` applied, and report. The
	// callback runs once, on this thread.
	void probe(const QUrl &url, const stream_context &ctx,
	            std::function<void(const probe_result &)> done);

	// The whole subtlety lives here, so it is separable and tested on its own
	// with no network: given what the server said and what it sent, what is it?
	static probe_result classify(const QString &content_type,
	                              const QByteArray &head);

	// Enough for `#EXTM3U`, an `<MPD` root element with its declaration, or an
	// ISO-BMFF `ftyp` box, and small enough not to pull a segment down.
	static constexpr int k_sniff_bytes = 2048;

	// A slow CDN should not hold the review dialog open indefinitely.
	static constexpr int k_timeout_ms = 8000;

private:
	QNetworkAccessManager *m_net = nullptr;
};
