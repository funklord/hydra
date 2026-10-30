#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;

// **What a page sends to somebody else.** A site that shows a person a
// teaser may show a search crawler the whole article -- that is what it
// wants indexed -- or a visitor arriving from a search result a free read;
// or it may send everybody the teaser and keep the rest on the server. Which
// of those a page does decides whether anything in the browser can help,
// and it was being found out by hand with curl: a paywalled Dagens ETC
// story on 2026-10-29 gave Googlebot, bingbot and Facebook's agent a bot
// challenge and Twitterbot the same teaser as a person, so nothing in reach
// held the rest. Asked for by the holder the same day as "a base tool for
// the AI that needs to fix pages, as it will be used often".
//
// The page's address is fetched once per client in a fixed catalog, with
// no cookies, and each answer read for a handful of facts: the status, how
// much paragraph text it holds, whether it is a bot challenge, and what
// the page says about its own paywall in its JSON-LD -- `isAccessibleForFree`
// and the selector of the part that is not, which Google asks publishers to
// declare -- and whether that part is in the answer at all.
//
// **The catalog is closed**: a caller names a client, it does not supply
// headers. The investigation's model picks from it the way it picks a
// scriptlet from that catalog, so what a model can cause stays a list
// somebody can read.
namespace agent_fetch {

struct agent {
	QString    name;         // what a caller names it by
	QString    what;         // one line for a person or a model
	QByteArray user_agent;
	QByteArray referer;      // empty: none sent
};

// Every client, the plain browser first.
QList<agent> catalog();

struct reading {
	QString agent;
	int     status = 0;        // 0: no answer
	QString error;             // why there was no answer
	qint64  bytes = 0;
	int     paragraphs = 0;    // <p> blocks of 40 characters or more
	int     text_chars = 0;    // their text, whitespace collapsed
	bool    challenge = false; // a bot check rather than the page
	bool    declared_paywall = false;   // JSON-LD: isAccessibleForFree false
	QString paywalled_selector;         // JSON-LD hasPart cssSelector
	bool    paywalled_present = false;  // that selector's class is in the body
	QString opening;           // the first paragraph's start
	// What was sent, and from where after redirects: kept so an answer that
	// holds more than the tab can be shown (`helps`, `servable`). Empty in a
	// reading made by `read` alone.
	QByteArray body;
	QUrl       url;
};

// Reads one answer. Pure, so it is tested on fixtures.
reading read(const QString &agent, int status, const QByteArray &body);

// One line per reading, for the log and the model's next turn.
QString describe(const reading &r);

// **Which answer, if any, holds more of the article than the tab does** --
// the decision behind showing a page as a crawler is sent it, which the
// holder settled on 2026-10-30: "we load pages as crawlers if it helps".
// Helps means an answer that is the page (2xx, not a bot check) and either
// carries the part the page declares not free where the tab does not, or
// has clearly more paragraph text: half as much again, and at least 1,000
// characters more, so a different sidebar is not taken for the article.
// Of several, the one with the most text. -1 when none does.
int helps(const reading &tab, const QList<reading> &answers);

// The answer as a document to show in place of the page: without its
// scripts, which are what put a paywall back over text the server sent --
// the JSON-LD stays, being data -- and with the part the page declared
// paywalled forced visible where it names it by class, since a stylesheet
// can hide it as well as a script. Empty when the result is past what a
// view will take (`show_html` goes through a data url of 2 MB).
QString servable(const reading &r);

}  // namespace agent_fetch

// Fetches one address as every client in the catalog, at once, and answers
// with the readings in catalog order. A fresh network manager per run, so no
// cookie a person or an earlier run collected is sent. Each fetch is bounded:
// 15 s, ten redirects, 4 MB.
class agent_fetcher : public QObject {
	Q_OBJECT
public:
	explicit agent_fetcher(QObject *parent = nullptr) : QObject(parent) {}

	// `done` is called once, on this object's thread, unless it is destroyed
	// first. A second call while one is running is answered with an error
	// reading at once rather than queued.
	void fetch_all(const QUrl &url,
	               std::function<void(const QList<agent_fetch::reading> &)> done);

	static constexpr int    k_timeout_ms = 15000;
	static constexpr qint64 k_max_bytes  = 4 * 1024 * 1024;

private:
	QNetworkAccessManager *m_net = nullptr;
	bool m_busy = false;
};
