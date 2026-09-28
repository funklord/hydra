#pragma once

#include "download_manager.h"

#include <QDialog>

#include <functional>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QString>

class QLabel;
class empty_state;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class local_proxy;
class player_launcher;

// **The folder "Open Folder" would show, or empty when it is not there.**
//
// Inline and free, so a suite can ask it without linking the window: the
// decision is the testable part and the button press is not.
//
// A finished download stays in the list across restarts, and the file it
// names can be moved, renamed or deleted at any moment after -- so a row
// whose folder is gone is the ordinary case rather than a strange one. It
// was reached by reading a driver's output: `try_downloads` printed
// thirteen rows from earlier runs, every one naming a scratch directory
// that had since been removed.
inline QString download_folder_to_show(const QString &path) {
	if (path.isEmpty())
		return QString();
	// The containing folder, never the file. Swarms and web servers carry
	// whatever is in them, and the standing rule (sec 11.4) is that a
	// download is written to disk and not opened by us.
	const QFileInfo fi(path);
	const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
	return QFileInfo::exists(dir) ? dir : QString();
}

// **What the status tooltip says about where a download came from.**
//
// Free and inline for the same reason as the folder decision above: the
// wording is the testable part and the hover is not.
//
// A job stores the `node_id` it was started from, and the row showed it raw
// -- "From tab t-5". That is an internal token: the tree shows titles, the id
// appears nowhere a person can see, and there is nothing to map it to.
//
// **`title` empty means the tree has no such node**, which is the ordinary
// case rather than an error: the history outlives the tabs in it, and a tab
// can be closed at any time after its download finished. Saying so is more
// use than naming an id that resolves to nothing.
//
// What this cannot say is that an id has been REUSED. `add_tab` takes the
// first free suffix from `unused_id`, so a deleted tab's id goes to the next
// new one, and a history row restored from disk can name a tab that is not
// the tab it came from. Nothing in the stored row distinguishes the two --
// see `project.md` -- so the wording stays "from a tab called X", which is
// what is known, rather than "from that tab".
inline QString download_tab_tooltip(const QString &node_id,
                                     const QString &title) {
	if (node_id.isEmpty())
		return QStringLiteral("Not associated with a tab");
	if (title.isEmpty())
		return QStringLiteral("The tab this came from is no longer in the "
		                       "tree");
	return QStringLiteral("Started from a tab called \"%1\"").arg(title);
}

// The downloads window (architecture doc sec 11.2, sec 11.4).
//
// One list for every source, which is the whole point: a torrent appears beside
// an HTTP file with the same columns, the same progress bar and the same
// controls, because sec 11.4 decided torrents are a first-class download rather
// than a side feature.
//
// Two rules this window is built around:
//
//  1. **It never asks what transport a row is.** Everything it varies -- whether
//     Pause is offered, whether children are shown, whether the row carries a
//     warning -- comes from `source_capabilities` and the job's own fields. The
//     word "torrent" does not appear in the logic, only in a source's
//     display_name.
//
//  2. **Publicly-observable rows are visibly different.** This is the other
//     half of the sec 11.4 privacy decision. Making a torrent behave exactly like
//     every other download is the goal, and it is also exactly what could
//     mislead someone into thinking it *is* like every other download. The
//     consent dialog says it once before the first one; this says it
//     permanently, on the row, for as long as the transfer exists.
class downloads_dialog : public QDialog {
	Q_OBJECT
public:
	downloads_dialog(download_manager *downloads, player_launcher *players,
	                  local_proxy *proxy, QWidget *parent = nullptr);

	// How a node id becomes something a person recognises. Injected rather
	// than looked up, so this window keeps knowing nothing about the tree:
	// it is handed an answer, the way the seam hands a view its decider.
	// Returns an empty string for an id the tree does not have.
	using tab_namer = std::function<QString(const QString &node_id)>;
	void set_tab_namer(tab_namer fn) { m_tab_namer = std::move(fn); }

protected:
	// The widths come from the font, which is right and is not the whole
	// story: it cannot know how wide the screen is. Re-decide on every
	// resize, which includes the first show.
	void resizeEvent(QResizeEvent *event) override;

private:
	// Drop the two columns a phone has no room for, and put them back when
	// there is room again. See the definition for the measurement.
	void fit_columns();
	void showEvent(QShowEvent *event) override;
	// What each column asks for at the current font, recorded once. Live
	// sizes cannot be read back: `setStretchLastSection` makes the last
	// column elastic, so they always add to about the viewport.
	int  m_natural[5]    = { 0, 0, 0, 0, 0 };
	int  m_natural_total = 0;
	bool m_cramped       = false;

	void refresh();              // reconcile rows against the job list
	void schedule_refresh();     // coalesce bursts of changed()
	void update_buttons();
	void act_pause();
	void act_resume();
	void act_cancel();
	void act_open_folder();
	void act_watch();
	void act_remove();
	void act_clear_finished();
	// Watch cannot launch the moment it is pressed: the front of the chosen
	// file is usually not there yet, especially when that file is not first in
	// the torrent. This polls until enough has landed, then launches.
	void try_launch_watch();
	int  selected_job() const;
	// Is there something in this job worth playing, and if so which file?
	// `rel` is the job-relative path, left empty for a single-file job -- that
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
	tab_namer    m_tab_namer;
	QLabel      *m_note   = nullptr;   // standing "public transfer" explanation
	QLabel      *m_action = nullptr;   // transient feedback from a button press
	QPushButton *m_pause  = nullptr;
	QPushButton *m_resume = nullptr;
	QPushButton *m_cancel = nullptr;
	QPushButton *m_folder = nullptr;
	QPushButton *m_remove = nullptr;
	QPushButton *m_clear  = nullptr;
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
