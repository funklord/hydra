#pragma once

#include "policy.h"
#include "sponsor_segments.h"

#include <QHash>
#include <QSet>
#include <QObject>
#include <QString>
#include <QUrl>

class policy_engine;
class QNetworkAccessManager;

// The bridge the injected script asks, and the one thing here that touches a
// network.
//
// **Nothing is asked until the shield says so, and the shield says no until
// somebody changes it.** `policy::feature::sponsor_skip` defaults to block, so
// on an untouched install this class makes no request of any kind: `ask`
// answers with an empty list and returns before building a url. That is the
// whole of "off by default" -- not a setting that suppresses the skipping while
// the query still goes out.
//
// **The policy is read against the FIRST-PARTY host, not the frame's.** An
// embedded player is an iframe on `youtube.com` sitting inside somebody else's
// page, and the setting belongs to the page a person is on rather than to the
// frame that happens to carry the video. `refresh_media_affordance` records
// the same distinction for the media badge, and for the same reason: a
// frame-supplied hostname is not the site the switch is about.
//
// **The rule for where to jump is the script's**, deliberately: it runs
// against the page's own `<video>`, on its own `timeupdate`, and a copy of it
// here would be a second implementation with no caller.
class sponsor_skip : public QObject {
	Q_OBJECT
public:
	explicit sponsor_skip(policy_engine *policy, QObject *parent = nullptr);

	// The object's name on the channel, and the two scripts.
	//
	// **Two, because Qt installs the channel transport in the main frame
	// alone** -- the same reason `mse_tap` has a relay and a hook. The worker
	// runs in every frame, has the DOM and no channel, and posts up to the
	// top; the relay runs in the main frame, has the channel, and posts the
	// answer back down to the frame that asked. An embedded player is a
	// subframe, which is the case this exists for, so the split is not
	// optional.
	static QString bridge_name();
	static QString relay_source();    // main frame: the channel
	static QString frame_source();    // every frame: the DOM and the rule

	// Where a prefix is asked. Exposed so a test can check what would be
	// requested without anything being requested.
	static QUrl query_url(const QString &prefix);

	// The first-party host of the view this bridge belongs to, which is what
	// the policy is read against. Set by the shell on every navigation, as the
	// cosmetic and consent bridges are.
	void set_page_host(const QString &host);

public slots:
	// Asked by the script with its own frame's url. Answers on `answer` with
	// the token it was given and a JSON array of `{f, t}` pairs in seconds --
	// empty when the feature is off, the url names no video, or the service
	// says nothing about it.
	void ask(const QString &token, const QString &url);

signals:
	void answer(const QString &token, const QString &json);

private:
	void reply_with(const QString &token, const QList<sponsor_segment> &segs);

	policy_engine         *m_policy = nullptr;
	QNetworkAccessManager *m_net    = nullptr;
	QString                m_host;
	// **A bound on what one page may ask about.** The frame supplies the url
	// it claims to be playing, and the policy gate is read against the
	// first-party host rather than that claim -- so the worst a hostile frame
	// can do is have this ask the service about a video nobody is watching.
	// Cheap to do and pointless to allow, so a page gets a few.
	QSet<QString> m_asked;
	static constexpr int k_max_videos_per_page = 8;
	// Keyed by prefix, because that is what was fetched: two videos sharing a
	// prefix are answered from one request, which is the only efficiency the
	// hash-prefix design hands back.
	QHash<QString, QByteArray> m_cache;
};
