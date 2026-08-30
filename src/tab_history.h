#pragma once

#include <QList>
#include <QString>
#include <QByteArray>

// Where a tab had been: the back/forward list, as a record rather than as
// engine state (architecture doc sec 4.2).
//
// **This is not the engine's history and cannot become it.** Qt WebEngine
// serializes its own back/forward list into an opaque blob, which carries
// scroll positions, form contents and cache keys, and which only the same
// engine version can read back. This carries urls and titles and nothing
// else, which is precisely what makes it portable: it survives being imported
// from another browser, written to a text file, read by a person, edited by
// one, and restored into a tab that has never been opened.
//
// The two coexist. A suspended tab has both -- `state/<id>.blob` for the
// engine and `state/<id>.history` for this -- and the blob is the one that is
// thrown away when the engine version moves on.
struct history_entry {
	QString url;
	QString title;
};

// The list and where in it the tab stands. One type because the two are
// meaningless apart: a position without its list indexes nothing, and a list
// without its position cannot say which entry is the page you are looking at.
struct tab_history {
	QList<history_entry> entries;
	// Index into `entries` of the page the tab is on. -1 when unknown, which
	// is not the same as 0: a tab whose position was never recorded is not a
	// tab sitting at the beginning of its own past.
	int index = -1;

	bool is_empty() const { return entries.isEmpty(); }
	// How many pages are behind and ahead of where the tab stands. Both are 0
	// when the position is unknown, so a caller need not special-case it.
	int back_count() const { return index > 0 ? index : 0; }
	int forward_count() const {
		return index >= 0 ? int(entries.size()) - index - 1 : 0;
	}
};

namespace tab_history_codec {

// The sidecar format, which is the tree file's: line-based, ` | `-separated,
// meant to be read and edited by a person. Split on the *first* separator
// only, because a title may contain one and a url may not -- an unencoded
// space cannot appear in a url, and everything after the first separator is
// therefore the title however many separators it holds.
QByteArray  encode(const tab_history &history);
tab_history decode(const QByteArray &bytes);

}  // namespace tab_history_codec
