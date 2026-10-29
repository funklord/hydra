#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

// **What the address bar offers while somebody types**, from the pages this
// browser already holds -- the tree: open tabs, saved links, anything a
// person has kept -- and from nothing else.
//
// Listed as unbuilt in project.md's "What is next" since 2026-09-18, and
// decided here from the aims rather than parked (2026-10-29): the tree is
// the person's record of where they go, so it is what the bar completes
// from. **No remote suggestions**: sending each keystroke to a search
// engine is the privacy cost every other browser pays for this feature,
// and this one's shield exists to not pay it.
//
// Pure, so the ranking can be tested without a window: a page is offered
// when what was typed starts its address (scheme and `www.` aside), when
// every typed word starts a word of its title, or failing both when the
// address contains it. That order is the ranking; within a rank, the page
// seen most recently comes first. One address is offered once.
namespace address_suggest {

struct page {
	QString   title;
	QString   url;
	QDateTime seen;
};

struct suggestion {
	QString title;
	QString url;
	int     rank = 0;   // 3 address prefix, 2 title words, 1 address contains
};

QList<suggestion> suggest(const QString &typed, const QList<page> &pages,
                          int limit = 8);

// The address without what nobody types: the scheme, and a leading `www.`.
QString bare(const QString &url);

}  // namespace address_suggest
