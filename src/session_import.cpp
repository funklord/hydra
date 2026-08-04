// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_import.h"

#ifdef HYDRA_HAVE_LZ4
#include <lz4.h>
#endif

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

namespace session_import {

QByteArray lz4_block_builtin(const QByteArray &in, int expected_size,
                              QString *error) {
	auto fail = [&](const char *why) {
		if (error)
			*error = QString::fromLatin1(why);
		return QByteArray();
	};
	if (expected_size < 0 || expected_size > (1 << 30))
		return fail("implausible decompressed size");

	QByteArray out;
	out.resize(expected_size);
	const quint8 *ip  = reinterpret_cast<const quint8 *>(in.constData());
	const quint8 *end = ip + in.size();
	quint8 *op        = reinterpret_cast<quint8 *>(out.data());
	quint8 *oend      = op + expected_size;

	// The format is a run of sequences, each: a token byte whose high nibble is
	// a literal length and whose low nibble is a match length, then any length
	// extension bytes, then the literals, then a two-byte little-endian offset
	// backwards into what has already been written, then the match.
	//
	// Every read is checked against `end` and every write against `oend`
	// *before* it happens. That is the whole safety argument: this parses a
	// file written by another program, and a truncated or corrupt one must come
	// back as an error rather than as a walk off the end of a buffer.
	while (ip < end) {
		const quint32 token = *ip++;
		quint32 lit = token >> 4;
		if (lit == 15) {
			quint32 more;
			do {
				if (ip >= end)
					return fail("truncated literal length");
				more = *ip++;
				lit += more;
				if (lit > quint32(1 << 30))
					return fail("implausible literal length");
			} while (more == 255);
		}
		if (quint32(end - ip) < lit)
			return fail("literal run past end of input");
		if (quint32(oend - op) < lit)
			return fail("literal run past end of output");
		memcpy(op, ip, lit);
		ip += lit;
		op += lit;

		// The final sequence is literals only: no offset follows it.
		if (ip == end)
			break;
		if (end - ip < 2)
			return fail("truncated match offset");
		const quint32 offset = quint32(ip[0]) | (quint32(ip[1]) << 8);
		ip += 2;
		if (offset == 0)
			return fail("zero match offset");
		if (quint32(op - reinterpret_cast<quint8 *>(out.data())) < offset)
			return fail("match offset points before the output");

		quint32 match = token & 0x0F;
		if (match == 15) {
			quint32 more;
			do {
				if (ip >= end)
					return fail("truncated match length");
				more = *ip++;
				match += more;
				if (match > quint32(1 << 30))
					return fail("implausible match length");
			} while (more == 255);
		}
		match += 4;   // the format's minimum match
		if (quint32(oend - op) < match)
			return fail("match run past end of output");

		// Byte at a time, because the ranges legitimately overlap: an offset of
		// one with a length of ten is how the format writes a run of the same
		// byte, and memcpy would be undefined there.
		const quint8 *m = op - offset;
		for (quint32 i = 0; i < match; ++i)
			*op++ = *m++;
	}

	if (op != oend)
		return fail("decompressed size does not match the header");
	return out;
}

bool using_system_lz4() {
#ifdef HYDRA_HAVE_LZ4
	return true;
#else
	return false;
#endif
}

QByteArray lz4_block_decompress(const QByteArray &in, int expected_size,
                                 QString *error) {
#ifdef HYDRA_HAVE_LZ4
	// The audited one where it exists. Same contract as the built-in: the
	// expected size bounds the output, and anything that does not fill it
	// exactly is a corrupt block rather than a short answer.
	if (expected_size < 0 || expected_size > (1 << 30)) {
		if (error)
			*error = "implausible decompressed size";
		return QByteArray();
	}
	QByteArray out;
	out.resize(expected_size);
	const int n = LZ4_decompress_safe(in.constData(), out.data(), in.size(),
	                                   expected_size);
	if (n < 0 || n != expected_size) {
		if (error)
			*error = "corrupt LZ4 block";
		return QByteArray();
	}
	return out;
#else
	return lz4_block_builtin(in, expected_size, error);
#endif
}

QByteArray mozlz4_decompress(const QByteArray &file, QString *error) {
	static const char magic[] = "mozLz40";
	if (file.size() < 12 || memcmp(file.constData(), magic, 8) != 0) {
		if (error)
			*error = "not a mozlz4 file (bad magic)";
		return QByteArray();
	}
	const quint8 *h = reinterpret_cast<const quint8 *>(file.constData()) + 8;
	const quint32 size = quint32(h[0]) | (quint32(h[1]) << 8) |
	                      (quint32(h[2]) << 16) | (quint32(h[3]) << 24);
	return lz4_block_decompress(file.mid(12), int(size), error);
}

QString firefox_profile(const QString &root_in) {
	const QString root = root_in.isEmpty()
		? QDir::homePath() + "/.mozilla/firefox"
		: root_in;
	const QString ini = root + "/profiles.ini";
	if (!QFile::exists(ini))
		return QString();

	QSettings s(ini, QSettings::IniFormat);
	// An [Install...] section with Locked=1 names the profile the running
	// Firefox actually uses, and it wins over Default=1 in a [Profile...]
	// section. Measured: on this machine Default=1 names a stub holding four
	// certificate databases and nothing else -- no session, no history -- while
	// the install-locked entry names the profile with 81 open tabs in it. An
	// importer that trusts Default=1 imports nothing and reports success.
	QString path;
	for (const QString &group : s.childGroups()) {
		if (!group.startsWith("Install"))
			continue;
		s.beginGroup(group);
		const QString def = s.value("Default").toString();
		s.endGroup();
		if (!def.isEmpty()) {
			path = def;
			break;
		}
	}
	if (path.isEmpty()) {
		for (const QString &group : s.childGroups()) {
			if (!group.startsWith("Profile"))
				continue;
			s.beginGroup(group);
			const bool is_default = s.value("Default").toString() == "1";
			const QString p = s.value("Path").toString();
			s.endGroup();
			if (is_default && !p.isEmpty()) {
				path = p;
				break;
			}
		}
	}
	if (path.isEmpty())
		return QString();
	// Paths in profiles.ini are relative unless IsRelative=0 says otherwise;
	// an absolute one starts with a separator either way.
	return path.startsWith('/') ? path : root + "/" + path;
}

QString firefox_session_path(const QString &profile) {
	if (profile.isEmpty())
		return QString();
	// `recovery.jsonlz4` is the live one Firefox rewrites as you browse.
	// `sessionstore.jsonlz4` only exists after a clean shutdown, so preferring
	// it would mean importing a stale set from a browser that is running right
	// now -- which is the common case for someone reaching for this feature.
	const QString recovery = profile + "/sessionstore-backups/recovery.jsonlz4";
	if (QFile::exists(recovery))
		return recovery;
	const QString clean = profile + "/sessionstore.jsonlz4";
	if (QFile::exists(clean))
		return clean;
	return QString();
}

QList<imported_tab> parse_firefox_session(const QByteArray &json, QString *error) {
	QList<imported_tab> out;
	QJsonParseError perr{};
	const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
	if (doc.isNull() || !doc.isObject()) {
		if (error)
			*error = "session file is not JSON: " + perr.errorString();
		return out;
	}
	const QJsonArray windows = doc.object().value("windows").toArray();
	int window_index = 0;
	for (const QJsonValue &wv : windows) {
		const QJsonObject w = wv.toObject();
		for (const QJsonValue &tv : w.value("tabs").toArray()) {
			const QJsonObject t = tv.toObject();
			const QJsonArray entries = t.value("entries").toArray();
			if (entries.isEmpty())
				continue;
			// `index` is 1-based and points at where in its own history the tab
			// currently is -- not at the end. A tab the user pressed Back on
			// twice would otherwise import as the page they navigated away
			// from, which is not the page they are looking at.
			int idx = t.value("index").toInt(entries.size());
			if (idx < 1 || idx > entries.size())
				idx = entries.size();
			const QJsonObject e = entries.at(idx - 1).toObject();

			imported_tab tab;
			tab.url    = e.value("url").toString();
			tab.title  = e.value("title").toString();
			tab.window = window_index;
			tab.pinned = t.value("pinned").toBool(false);
			if (tab.url.isEmpty())
				continue;
			// A tab that has never been loaded has no title of its own; its
			// address is a better label than an empty row.
			if (tab.title.isEmpty())
				tab.title = tab.url;
			out << tab;
		}
		++window_index;
	}
	// Deliberately not `_closedTabs`: those are what the user closed, and a
	// feature that resurrects them has answered a question nobody asked.
	if (out.isEmpty() && error)
		*error = "no open tabs found in the session file";
	return out;
}

QList<imported_tab> firefox_tabs(const QString &session_file, QString *error) {
	QFile f(session_file);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = "cannot read " + session_file;
		return {};
	}
	const QByteArray raw = f.readAll();
	f.close();
	const QByteArray json = mozlz4_decompress(raw, error);
	if (json.isEmpty())
		return {};
	return parse_firefox_session(json, error);
}

}  // namespace session_import
