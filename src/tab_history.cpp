// SPDX-License-Identifier: GPL-3.0-or-later
#include "tab_history.h"

namespace {

// A title is free text and the format is line-based, so a newline in one would
// turn a single entry into two -- the second of which parses as a url. Tabs go
// the same way for the same reason: they are whitespace nobody can see in the
// file they are corrupting.
QString one_line(QString text) {
	text.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ');
	return text;
}

const char k_magic[] = "hydra-history 1";

}  // namespace

namespace tab_history_codec {

QByteArray encode(const tab_history &history) {
	// **An empty record has no representation.** A header with nothing under
	// it is a file, and a file is what `has_history` answers about -- so an
	// empty one would make every caller re-open it to find out that the tab
	// has no past after all. A position with no entries to index is not a
	// weaker record; it is not a record.
	if (history.entries.isEmpty())
		return {};
	QString out = QString::fromLatin1(k_magic);
	// Written even when it is -1, so that reading the file back cannot
	// silently turn "position unknown" into "position at the start".
	out += QString(" | index=%1\n").arg(history.index);
	for (const history_entry &e : history.entries) {
		if (e.url.isEmpty())
			continue;
		out += one_line(e.url) + " | " + one_line(e.title) + "\n";
	}
	return out.toUtf8();
}

tab_history decode(const QByteArray &bytes) {
	tab_history out;
	const QStringList lines = QString::fromUtf8(bytes).split('\n');
	bool first = true;
	for (const QString &raw : lines) {
		const QString line = raw.trimmed();
		if (line.isEmpty())
			continue;
		if (first) {
			first = false;
			// The header is required. A file that does not begin with it is
			// not this format, and guessing at one is how a reader invents a
			// history out of somebody else's file.
			if (!line.startsWith(QString::fromLatin1(k_magic)))
				return {};
			const int at = line.indexOf("index=");
			if (at >= 0) {
				bool ok = false;
				const int v = line.mid(at + 6).trimmed().toInt(&ok);
				if (ok)
					out.index = v;
			}
			continue;
		}
		const int sep = line.indexOf(" | ");
		history_entry e;
		e.url   = sep >= 0 ? line.left(sep) : line;
		e.title = sep >= 0 ? line.mid(sep + 3) : QString();
		if (e.url.isEmpty())
			continue;
		if (e.title.isEmpty())
			e.title = e.url;
		out.entries << e;
	}
	// A position pointing outside the list is a file that has been edited, or
	// truncated in a way that lost entries. Refusing it costs the position and
	// keeps the list; honouring it would index past the end at the first click.
	if (out.index >= out.entries.size())
		out.index = out.entries.isEmpty() ? -1 : int(out.entries.size()) - 1;
	if (out.index < -1)
		out.index = -1;
	return out;
}

}  // namespace tab_history_codec
