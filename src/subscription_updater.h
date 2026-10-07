#pragma once

#include "filter_subscription.h"

#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// Fetching the subscribed lists (architecture doc sec 12.5).
//
// **The gate runs before anything is written, and a refusal leaves the cached
// copy exactly as it was.** That is the whole shape of this class: the failure
// it is built against is not a network error -- those are easy and loud -- but
// a 200 carrying a login page, an error page or a half-migrated mirror, which
// would otherwise overwrite a working list with one that blocks nothing.
// `filter_subscription::read` decides, and only its verdict can promote a
// body.
//
// One request at a time, deliberately. A browser that opens six connections to
// six list servers on startup is doing it for its own convenience; the lists
// are refreshed once a day and nothing waits on them.
//
// **This owns the index.** `main_window` reads the subscriptions from here
// rather than keeping its own copy, because two owners of one list is how the
// settings UI and the fetcher end up disagreeing about what somebody
// subscribed to.
class subscription_updater : public QObject {
	Q_OBJECT
public:
	subscription_updater(const QString &index_path, const QString &dir,
	                      QObject *parent = nullptr);

	const QList<subscription> &subscriptions() const { return m_subs; }
	// Replace the set, from the settings UI. Saves the index immediately: a
	// subscription somebody added and a crash before the next fetch should
	// leave the subscription, not lose it.
	bool set_subscriptions(const QList<subscription> &subs);
	QString dir() const { return m_dir; }

	// Fetch the enabled subscriptions that are due. `force` takes them all.
	// Returns how many will be fetched, so a caller can say "nothing was due"
	// rather than implying a pass happened.
	int update(bool force = false, int max_age_hours = k_default_age_hours);
	bool busy() const { return m_at >= 0; }

	static constexpr int k_default_age_hours = 24;
	// **A ceiling, because a body is read into memory to be parsed.** EasyList
	// is a couple of megabytes; anything approaching this is not a filter
	// list, and a reply that keeps coming is aborted rather than trusted to
	// stop.
	static constexpr qint64 k_max_bytes = 32 * 1024 * 1024;

signals:
	// One line, for the status bar: what is being fetched, and what each
	// fetch decided.
	void note(const QString &text);
	// A pass has finished. `promoted` bodies were written and are in force
	// once the caller reloads; `refused` were fetched and declined, or could
	// not be fetched at all.
	void updated(int promoted, int refused);

private:
	void start_next();
	void finish(QNetworkReply *reply, int which);

	QNetworkAccessManager *m_net = nullptr;
	QString m_index;
	QString m_dir;
	QList<subscription> m_subs;
	QList<int> m_queue;      // indices into m_subs, still to fetch
	int m_at = -1;           // the one in flight, or -1
	int m_promoted = 0;
	int m_refused  = 0;
};
