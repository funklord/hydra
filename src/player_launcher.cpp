// SPDX-License-Identifier: GPL-3.0-or-later
#include "player_launcher.h"

#include <QProcess>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

struct known_player {
	const char *id;
	const char *label;
	bool native_streams;   // handles HLS/DASH manifests itself
};

// The full menu of what is supported, installed or not — the settings UI shows
// the missing ones greyed out so the user can see what to install (§11.3).
const known_player k_known[] = {
	{ "mpv",      "mpv",       true  },
	{ "vlc",      "VLC",       true  },
	{ "smplayer", "SMPlayer",  true  },
	{ "ffplay",   "ffplay",    true  },
	{ "mplayer2", "mplayer2",  false },
	{ "mplayer",  "mplayer",   false },
};

bool is_manifest(const media_item &item) {
	return item.kind == media_kind::hls || item.kind == media_kind::dash;
}

}  // namespace

player_launcher::player_launcher() {
	refresh();
}

void player_launcher::refresh() {
	m_players.clear();
	for (const known_player &k : k_known) {
		player_entry e;
		e.id             = QString::fromLatin1(k.id);
		e.label          = QString::fromLatin1(k.label);
		e.native_streams = k.native_streams;
		e.path           = QStandardPaths::findExecutable(e.id);
		e.installed      = !e.path.isEmpty();
		m_players.push_back(e);
	}

	// The Custom… entry is always offered; whether it works is up to whatever
	// the user typed, which is checked when it is set rather than probed here.
	{
		player_entry e;
		e.id             = QString::fromLatin1(custom_id());
		e.label          = "Custom…";
		e.native_streams = true;    // unknown, so do not warn about manifests
		e.installed      = false;
		m_players.push_back(e);
		set_custom_command(m_custom);   // re-resolves its path
	}

	// Resolve the default from what is present — never assume mpv exists.
	if (!entry(m_selected) || !entry(m_selected)->installed) {
		m_selected.clear();
		for (const player_entry &e : m_players) {
			if (e.installed) { m_selected = e.id; break; }
		}
	}
}

QList<player_entry> player_launcher::installed() const {
	QList<player_entry> out;
	for (const player_entry &e : m_players)
		if (e.installed)
			out.push_back(e);
	return out;
}

const player_entry *player_launcher::entry(const QString &id) const {
	for (const player_entry &e : m_players)
		if (e.id == id)
			return &e;
	return nullptr;
}

bool player_launcher::selected_handles_streams() const {
	const player_entry *e = entry(m_selected);
	return e && e->installed && e->native_streams;
}

QString player_launcher::warning_for(const media_item &item) const {
	const player_entry *e = entry(m_selected);
	if (!e || !e->installed)
		return "No external player found. Install mpv, VLC, or mplayer.";
	if (is_manifest(item) && !e->native_streams) {
		// HLS gets assembled into a progressive file before it reaches a player
		// like this (§11.3), so only DASH is still a genuine problem.
		if (item.kind == media_kind::dash)
			return QString("%1 cannot play DASH, and DASH assembly is not "
			               "implemented.").arg(e->label);
	}
	return QString();
}

void player_launcher::set_custom_command(const QString &cmd) {
	m_custom = cmd.trimmed();
	for (player_entry &e : m_players) {
		if (e.id != QLatin1String(custom_id()))
			continue;
		const QStringList parts = m_custom.split(QRegularExpression("\\s+"),
		                                          Qt::SkipEmptyParts);
		e.path      = parts.isEmpty() ? QString()
		                              : QStandardPaths::findExecutable(parts.first());
		// An absolute path that exists is equally valid as a PATH lookup.
		if (e.path.isEmpty() && !parts.isEmpty() && QFileInfo(parts.first()).isExecutable())
			e.path = parts.first();
		e.installed = !e.path.isEmpty();
		e.label     = m_custom.isEmpty() ? "Custom…"
		                                 : QString("Custom… (%1)").arg(parts.value(0));
		return;
	}
}

bool player_launcher::play(const media_item &item, QString *error,
                            const QUrl &via) const {
	const player_entry *e = entry(m_selected);
	if (!e || !e->installed) {
		if (error)
			*error = "No external player selected.";
		return false;
	}

	// Always a URL, never stdin: a pipe cannot seek (§11.3).
	const QString target = via.isValid() ? via.toString() : item.url.toString();
	QStringList args;
	if (e->id == QLatin1String(custom_id())) {
		QStringList parts = m_custom.split(QRegularExpression("\\s+"),
		                                    Qt::SkipEmptyParts);
		if (!parts.isEmpty())
			parts.removeFirst();          // the program itself
		bool substituted = false;
		for (QString &a : parts) {
			if (a.contains("%U")) {
				a.replace("%U", target);
				substituted = true;
			}
		}
		if (!substituted)
			parts << target;              // no placeholder: append, as meant
		args = parts;
	} else if (e->id == "mpv") {
		args << "--force-window=yes" << target;
	} else if (e->id == "vlc") {
		args << target;
	} else if (e->id == "ffplay") {
		args << "-autoexit" << target;
	} else if (e->id == "smplayer") {
		args << target;
	} else {   // mplayer / mplayer2
		args << "-cache" << "8192" << target;
	}

	// Detached: a player outliving the browser is normal.
	const bool ok = QProcess::startDetached(e->path, args);
	if (!ok && error)
		*error = QString("Could not start %1.").arg(e->label);
	return ok;
}
