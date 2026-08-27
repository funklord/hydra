// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_import.h"

/// @pkg_optional liblz4 defines HYDRA_HAVE_LZ4
#ifdef HYDRA_HAVE_LZ4
#include <lz4.h>
#endif

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QPair>
#include <algorithm>
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
			// The rest of `entries` is where this tab had been. Kept in the
			// order Firefox wrote it, oldest first, which is the order `index`
			// counts in -- so the current entry keeps its place rather than
			// being hoisted to the front and losing what came before it.
			//
			// The position is counted as the list is built, not searched for
			// afterwards: entries with no url are skipped, so a position in
			// `entries` is not a position in `history`, and matching on the
			// url instead would find the *first* occurrence -- a tab that
			// went A, B, A and is on the second A would report itself as
			// being on the first, two steps of history further back than it
			// really is.
			for (int i = 0; i < entries.size(); ++i) {
				const QJsonObject h = entries.at(i).toObject();
				const QString hu = h.value("url").toString();
				if (hu.isEmpty())
					continue;
				if (i == idx - 1)
					tab.history.index = tab.history.entries.size();
				history_entry he;
				he.url   = hu;
				he.title = h.value("title").toString();
				if (he.title.isEmpty())
					he.title = hu;
				tab.history.entries << he;
			}
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


// --- Chromium -------------------------------------------------------------

namespace {

// From components/sessions/core/command_storage_backend.cc.
constexpr qint32 k_snss_signature = 0x53534E53;   // 'SNSS' little-endian
constexpr qint32 k_version_plain  = 1;
constexpr qint32 k_version_marker = 3;            // what Chromium writes today

// From components/sessions/core/session_service_commands.cc. Only the ones this
// needs: a tab's identity, where it lives, which entry of its history it is on,
// and whether it has gone away.
constexpr quint8 k_cmd_set_tab_window            = 0;
constexpr quint8 k_cmd_set_tab_index_in_window   = 2;
constexpr quint8 k_cmd_update_tab_navigation     = 6;
constexpr quint8 k_cmd_set_selected_nav_index    = 7;
constexpr quint8 k_cmd_tab_closed                = 16;
constexpr quint8 k_cmd_window_closed             = 17;

// A reader over a command payload that cannot walk off the end. Chromium's
// `base::Pickle` writes an int as four bytes and a string as a length followed
// by its bytes padded up to four, and every accessor here refuses rather than
// reads past `m_end`.
class payload_reader {
public:
	payload_reader(const char *data, int size) : m_p(data), m_end(data + size) {}
	bool ok() const { return m_ok; }
	qint32 read_int() {
		if (!m_ok || m_end - m_p < 4) { m_ok = false; return 0; }
		qint32 v;
		memcpy(&v, m_p, 4);
		m_p += 4;
		return v;
	}
	QString read_string() {
		const qint32 n = read_int();
		if (!m_ok || n < 0 || m_end - m_p < n) { m_ok = false; return {}; }
		const QString s = QString::fromUtf8(m_p, n);
		m_p += (n + 3) & ~3;   // padded to four
		if (m_p > m_end) m_ok = false;
		return s;
	}
	QString read_string16() {
		const qint32 n = read_int();   // a count of characters, not bytes
		if (!m_ok || n < 0 || (m_end - m_p) / 2 < n) { m_ok = false; return {}; }
		const QString s = QString::fromUtf16(
		  reinterpret_cast<const char16_t *>(m_p), n);
		m_p += (2 * n + 3) & ~3;
		if (m_p > m_end) m_ok = false;
		return s;
	}
private:
	const char *m_p;
	const char *m_end;
	bool m_ok = true;
};

struct replay_tab {
	qint32  window = 0;
	int     index_in_window = -1;
	qint32  selected = -1;
	QHash<qint32, QPair<QString, QString>> navigations;   // index -> (url, title)
	int     first_seen = 0;   // to keep a stable order when nothing else says
};

}  // namespace

QList<imported_tab> replay_snss(const QByteArray &file, QString *error) {
	auto fail = [&](const QString &why) {
		if (error)
			*error = why;
		return QList<imported_tab>();
	};
	if (file.size() < 8)
		return fail("session file is too short to have a header");
	qint32 sig = 0, version = 0;
	memcpy(&sig, file.constData(), 4);
	memcpy(&version, file.constData() + 4, 4);
	if (sig != k_snss_signature)
		return fail("not a Chromium session file (bad signature)");
	// Versions 2 and 4 are the encrypted ones, and there is no key here to read
	// them with. Saying which version was found beats "could not read it": this
	// is internal API and the number is the first thing worth knowing when it
	// stops working.
	if (version != k_version_plain && version != k_version_marker)
		return fail(QString("unsupported Chromium session version %1 "
		                     "(encrypted, or newer than this reader)").arg(version));

	QHash<qint32, replay_tab> tabs;
	QSet<qint32> closed_windows;
	int seen = 0;
	int pos = 8;
	while (pos + 2 <= file.size()) {
		quint16 size = 0;
		memcpy(&size, file.constData() + pos, 2);
		pos += 2;
		// A truncated tail is normal, not corruption: this file is being
		// written by a running browser and the last record may be half there.
		if (size == 0 || pos + size > file.size())
			break;
		const quint8 id = quint8(file.at(pos));
		const char *body = file.constData() + pos + 1;
		const int body_size = size - 1;
		pos += size;

		payload_reader r(body, body_size);
		switch (id) {
		case k_cmd_update_tab_navigation: {
			// A pickle: its own uint32 size, then the tab id, then the entry.
			payload_reader p(body, body_size);
			p.read_int();                       // pickle payload size
			const qint32 tab = p.read_int();
			const qint32 index = p.read_int();
			const QString url = p.read_string();
			const QString title = p.read_string16();
			if (!p.ok() || url.isEmpty())
				break;
			replay_tab &t = tabs[tab];
			if (t.first_seen == 0)
				t.first_seen = ++seen;
			// Last writer wins: a tab that navigated twice at the same index
			// has been rewritten, and the later record is where it is now.
			t.navigations.insert(index, { url, title });
			break;
		}
		case k_cmd_set_selected_nav_index: {
			const qint32 tab = r.read_int();
			const qint32 index = r.read_int();
			if (!r.ok())
				break;
			replay_tab &t = tabs[tab];
			if (t.first_seen == 0)
				t.first_seen = ++seen;
			t.selected = index;
			break;
		}
		case k_cmd_set_tab_window: {
			const qint32 window = r.read_int();
			const qint32 tab = r.read_int();
			if (!r.ok())
				break;
			replay_tab &t = tabs[tab];
			if (t.first_seen == 0)
				t.first_seen = ++seen;
			t.window = window;
			break;
		}
		case k_cmd_set_tab_index_in_window: {
			const qint32 tab = r.read_int();
			const qint32 index = r.read_int();
			if (!r.ok())
				break;
			tabs[tab].index_in_window = index;
			break;
		}
		case k_cmd_tab_closed: {
			// The struct's first field is the id; whatever padding follows it
			// is not read, so this does not depend on the compiler's layout.
			const qint32 tab = r.read_int();
			if (r.ok())
				tabs.remove(tab);
			break;
		}
		case k_cmd_window_closed: {
			const qint32 window = r.read_int();
			if (r.ok())
				closed_windows.insert(window);
			break;
		}
		default:
			break;   // everything else is state this does not need
		}
	}

	// What survived, in the order the browser would show it.
	QList<QPair<QPair<qint32, int>, imported_tab>> ordered;
	for (auto it = tabs.constBegin(); it != tabs.constEnd(); ++it) {
		const replay_tab &t = it.value();
		if (closed_windows.contains(t.window))
			continue;
		if (t.navigations.isEmpty())
			continue;
		// The entry the tab is actually on. Falling back to the highest index
		// rather than the first: a tab whose selected index was never recorded
		// is one that has not navigated since the log began, and its latest
		// entry is the better guess at what is on screen.
		qint32 idx = t.selected;
		if (!t.navigations.contains(idx)) {
			idx = -1;
			for (auto n = t.navigations.constBegin(); n != t.navigations.constEnd(); ++n)
				idx = qMax(idx, n.key());
		}
		const auto nav = t.navigations.value(idx);
		imported_tab tab;
		tab.url    = nav.first;
		tab.title  = nav.second.isEmpty() ? nav.first : nav.second;
		tab.window = int(t.window);
		// The map is already the tab's whole history; it is keyed by Chromium's
		// navigation index, which a QHash does not keep in order, so the keys
		// are sorted before the list is built. Out of order this would read as
		// a tab that had visited its own past at random.
		QList<qint32> keys = t.navigations.keys();
		std::sort(keys.begin(), keys.end());
		for (qint32 k : keys) {
			const auto n = t.navigations.value(k);
			if (n.first.isEmpty())
				continue;
			history_entry he;
			he.url   = n.first;
			he.title = n.second.isEmpty() ? n.first : n.second;
			tab.history.entries << he;
			if (k == idx)
				tab.history.index = int(tab.history.entries.size()) - 1;
		}
		if (tab.url.isEmpty())
			continue;
		ordered << qMakePair(qMakePair(t.window,
		  t.index_in_window >= 0 ? t.index_in_window : t.first_seen), tab);
	}
	std::sort(ordered.begin(), ordered.end(),
	          [](const auto &a, const auto &b) { return a.first < b.first; });

	QList<imported_tab> out;
	for (const auto &o : ordered)
		out << o.second;
	if (out.isEmpty() && error)
		*error = "no open tabs found in the Chromium session";
	return out;
}

QString chromium_profile(const QString &root_in) {
	// Chromium and Chrome keep the same layout in different directories, and a
	// machine may have either, both, or neither.
	const QStringList roots = root_in.isEmpty()
	  ? QStringList{ QDir::homePath() + "/.config/chromium",
		                QDir::homePath() + "/.config/google-chrome" }
	  : QStringList{ root_in };
	for (const QString &root : roots) {
		const QString def = root + "/Default";
		if (QFile::exists(def + "/Preferences") || QDir(def + "/Sessions").exists())
			return def;
	}
	return QString();
}

QString chromium_session_path(const QString &profile) {
	if (profile.isEmpty())
		return QString();
	QDir dir(profile + "/Sessions");
	if (!dir.exists())
		return QString();
	// The newest `Session_*`. Chromium keeps more than one and the number in
	// the name is a timestamp, but sorting by mtime asks the question directly
	// rather than depending on how that number is formed.
	QFileInfoList files = dir.entryInfoList({ "Session_*" }, QDir::Files, QDir::Time);
	return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}

QList<imported_tab> chromium_tabs(const QString &session_file, QString *error) {
	QFile f(session_file);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error)
			*error = "cannot read " + session_file;
		return {};
	}
	const QByteArray raw = f.readAll();
	f.close();
	return replay_snss(raw, error);
}

}  // namespace session_import
