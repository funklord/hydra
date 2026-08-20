// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media_detector.h"
#include "local_proxy.h"

#include <QTemporaryDir>

#include <QDialog>

class QLabel;
class empty_state;
class QEvent;
class QTreeWidget;
class download_manager;
class player_launcher;
class local_proxy;
class hls_assembler;
class mse_tap;

// The compact list behind the media badge (architecture doc sec 11.2/sec 11.3).
// One row per detected stream, each offering both Watch and Download -- both
// buttons carry a leading glyph, and the labels are built in
// `media_dialog.cpp` rather than here -- with the primary stream first. Watch
// is the default action, because the whole point is that the site's own player
// is broken.
class media_dialog : public QDialog {
	Q_OBJECT
public:
	media_dialog(media_detector *detector, player_launcher *players,
	              download_manager *downloads, local_proxy *proxy,
	              mse_tap *tap, QWidget *parent = nullptr);

	void set_site(const QString &site_host, const QString &node_id,
	               const stream_context &ctx);

signals:
	// A page whose video only the tap can see has nothing to Watch or
	// Download -- the only way to get those bytes is to record them, and the
	// shell owns that.
	void capture_requested();

private:
	void repopulate();
	void watch(const media_item &item);
	void save(const media_item &item);
	// Assemble an HLS stream into one progressive file, then act on it.
	void assemble_then(const media_item &item, bool play_it);

	media_detector   *m_detector  = nullptr;
	player_launcher  *m_players   = nullptr;
	download_manager *m_downloads = nullptr;
	local_proxy      *m_proxy     = nullptr;
	mse_tap          *m_tap       = nullptr;
	stream_context    m_ctx;
	hls_assembler    *m_assembler = nullptr;
	QTemporaryDir     m_scratch;
	// One name per assembly. A fixed one meant watching a second stream
	// truncated the file the first player was still reading.
	int               m_stream_seq = 0;
	QString m_site;
	QString m_node_id;

	QTreeWidget *m_list   = nullptr;
	QLabel      *m_status = nullptr;
	// The "nothing here" message, over the empty rows area rather than under
	// it. `m_status` cannot carry this: it is the dialog's status line and
	// also reports assembling, progress and errors.
	empty_state *m_empty   = nullptr;
};
