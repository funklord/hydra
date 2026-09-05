#pragma once

#include "request_filter.h"
#include "site_extractor.h"

#include <QHash>
#include <QMutex>
#include <QObject>

// The evidence a generated extractor is proposed against, and judged against
// (architecture doc sec 11.5): what a page actually requested, in order.
//
// A third rider on the interceptor's observer seam, beside media detection and
// filter signals. It overlaps them in that all three keep URLs per site, and
// stays separate because what each keeps differs: this one needs order and
// resource kind, which the others discard, and it must survive exactly as long
// as the page it describes.
//
// Bounded on purpose. A page left open on a live stream requests forever, and
// evidence is only useful while it is small enough for a person to read.
class extractor_signals : public QObject, public request_observer {
	Q_OBJECT
public:
	explicit extractor_signals(QObject *parent = nullptr);

	void on_request(const request_context &ctx, const request_decision &d) override;

	QList<evidence_request> evidence_for(const QString &site_host) const;
	int count_for(const QString &site_host) const;
	void clear_site(const QString &site_host);
	// Forget every site at once, for "Clear browsing data".
	//
	// **This is a browsing record and it was not being cleared.** The shell's
	// clear dropped cookies, the cache, session permission answers and the
	// proxy's publications, and left this behind for the life of the process
	// -- which is the moment a person has just said they want the browser to
	// forget where they have been.
	//
	// Returns how many sites were dropped, so the caller can say what it did
	// rather than claiming a clear it cannot see the size of.
	int clear_all();


	// How many requests are kept per page.
	static constexpr int k_limit = 400;

private:
	mutable QMutex m_lock;
	QHash<QString, QList<evidence_request>> m_by_site;
	QHash<QString, int> m_next_order;
};
