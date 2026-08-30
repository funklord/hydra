#pragma once

#include "request_filter.h"
#include "site_rules.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

// Notices when a page is checking whether we block ads.
//
// This exists because of a measured failure, not a hypothetical one. On a real
// watch page the video player simply never started: the play button stayed put,
// every click was answered by the ad network, and nothing in the request log
// looked wrong. What the log actually contained was
// `cdnjs.cloudflare.com/ajax/libs/fuckadblock/3.2.1/fuckadblock.min.js` -- the
// page was watching for a blocker and refusing to play. Allowing ads for that
// site alone started it.
//
// **The failure mode is the point.** A page broken this way looks like a broken
// site, or like a broken browser, and gives the user nothing to act on. Our own
// blocking is the cause and only we can say so. So this reports; it does not
// decide. The shield already has a per-site tri-state for ads and the user can
// use it -- what was missing was any way to know that is the lever.
//
// A fourth rider on the interceptor's observer seam, beside the media detector,
// filter signals and extractor signals: the request stream is already being
// watched, and a second sensor for the same facts would be a second thing to
// keep true (sec 10).
//
// **Detected by name, from the shared rule store.** The names live in
// `site_rules` beside the consent-banner rules rather than in an array here: a
// detector renames itself, and a list compiled into the binary is a release
// behind for everyone. They are the same kind of fact, they go stale the same
// way, and they are meant to travel together -- one file with one provenance
// model, not two.
class antiadblock_watch : public QObject, public request_observer {
	Q_OBJECT
public:
	explicit antiadblock_watch(QObject *parent = nullptr);

	// Called off the UI thread, like every observer (sec 10). Guarded accordingly.
	void on_request(const request_context &ctx, const request_decision &d) override;

	// Whether this page fetched something whose whole job is to detect us.
	bool checked_for_blocker(const QString &site_host) const;
	// What gave it away, for the message and the tests.
	QStringList evidence_for(const QString &site_host) const;
	void clear_site(const QString &site_host);

	// The rules in force. Set by the shell from the same file the consent
	// blocker reads.
	void set_rules(const site_rules &r);

	// Whether a name is one of the known detectors. Static and taking its
	// patterns explicitly, because it is the whole judgement and deserves to be
	// testable without a browser or a shell.
	static bool looks_like_detector(const QUrl &url, const QStringList &names);
	bool looks_like_detector(const QUrl &url) const;

signals:
	// Emitted once per site, not once per request: this is a fact about the
	// page, and repeating it would make the status bar useless.
	void detected(const QString &site_host, const QString &what);

private:
	mutable QMutex               m_lock;
	QStringList                  m_detectors;
	QHash<QString, QStringList>  m_by_site;
};
