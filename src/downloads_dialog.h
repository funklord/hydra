// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "download_manager.h"

#include <QDialog>
#include <QHash>

class QLabel;
class empty_state;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class local_proxy;
class player_launcher;

// The downloads window (architecture doc §11.2, §11.4).
//
// One list for every source, which is the whole point: a torrent appears beside
// an HTTP file with the same columns, the same progress bar and the same
// controls, because §11.4 decided torrents are a first-class download rather
// than a side feature.
//
// Two rules this window is built around:
//
//  1. **It never asks what transport a row is.** Everything it varies — whether
//     Pause is offered, whether children are shown, whether the row carries a
//     warning — comes from `source_capabilities` and the job's own fields. The
//     word "torrent" does not appear in the logic, only in a source's
//     display_name.
//
//  2. **Publicly-observable rows are visibly different.** This is the other
//     half of the §11.4 privacy decision. Making a torrent behave exactly like
//     every other download is the goal, and it is also exactly what could
//     mislead someone into thinking it *is* like every other download. The
//     consent dialog says it once before the first one; this says it
//     permanently, on the row, for as long as the transfer exists.
class downloads_dialog : public QDialog {
	Q_OBJECT
public:
	downloads_dialog(download_manager *downloads, player_launcher *players,
	                  local_proxy *proxy, QWidget *parent = nullptr);

private:
	void refresh();              // reconcile rows against the job list
	void schedule_refresh();     // coalesce bursts of changed()
	void update_buttons();
	void act_pause();
	void act_resume();
	void act_cancel();
	void act_open_folder();
	void act_watch();
	// Watch cannot launch the moment it is pressed: the front of the chosen
	// file is usually not there yet, especially when that file is not first in
	// the torrent. This polls until enough has landed, then launches.
	void try_launch_watch();
	int  selected_job() const;
	// Is there something in this job worth playing, and if so which file?
	// `rel` is the job-relative path, left empty for a single-file job — that
	// is a real answer, not a failure, which is why it cannot be signalled by
	// returning an empty string. UI-level media judgement, which is why it
	// lives here and not behind the transport seam.
	bool find_playable(const download_job &j, QString *rel) const;

	download_manager *m_downloads = nullptr;
	player_launcher  *m_players   = nullptr;
	local_proxy      *m_proxy     = nullptr;

	// Keeps the empty-state label over the list as the window changes size.
	// Setting its geometry once, from `refresh`, put it at the top-left and
	// clipped it: `refresh` runs before the first layout, so the viewport it
	// measured was not the one that ended up on screen.
	// **Filtered on the viewport, not on the dialog.** A `resizeEvent` override
	// here fires before the list's viewport has settled, so the geometry it
	// measured was a few pixels tall and the message came out clipped against
	// the header. The viewport tells us when it is actually the size it will
	// be drawn at.

	QTreeWidget *m_list   = nullptr;
	// Shown over the empty list. A window of column headings above four
	// hundred pixels of nothing reads as broken rather than as idle, which is
	// the same complaint the comment beside the action buttons already makes
	// about them.
	empty_state *m_empty   = nullptr;
	QLabel      *m_note   = nullptr;   // standing "public transfer" explanation
	QLabel      *m_action = nullptr;   // transient feedback from a button press
	QPushButton *m_pause  = nullptr;
	QPushButton *m_resume = nullptr;
	QPushButton *m_cancel = nullptr;
	QPushButton *m_folder = nullptr;
	QPushButton *m_watch  = nullptr;
	QTimer      *m_coalesce = nullptr;

	// Pending Watch, waiting for enough of the file to exist.
	QTimer  *m_watch_wait = nullptr;
	int      m_watch_job  = 0;
	QString  m_watch_rel;
	QString  m_watch_path;
	int      m_watch_ticks = 0;

	// Rows are reconciled in place rather than rebuilt. changed() fires on
	// every chunk of every transfer, and clearing the tree that often would
	// throw away the selection and scroll position several times a second.
	QHash<int, QTreeWidgetItem *> m_rows;
};
