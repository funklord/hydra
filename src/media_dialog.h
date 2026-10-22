#pragma once

#include "media_detector.h"
#include "local_proxy.h"

#include <QDialog>

class QLabel;
class QPushButton;
class empty_state;
class QEvent;
class QTreeWidget;
class download_manager;
class player_launcher;
class local_proxy;
class mse_tap;
class stream_assembly;

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
	              mse_tap *tap, stream_assembly *assembly,
	              QWidget *parent = nullptr);

	void set_site(const QString &site_host, const QString &node_id,
	               const stream_context &ctx);

	// What the lookup is doing or found, in a line of its own so the list's
	// own status is not overwritten by it.
	void set_lookup_note(const QString &text);

	// Below this width the Host column is hidden; `main_window`'s drawer
	// threshold, so "narrow" means one thing in this program.
	static constexpr int k_narrow_width = 620;

protected:
	void resizeEvent(QResizeEvent *e) override;

signals:
	// A page whose video only the tap can see has nothing to Watch or
	// Download -- the only way to get those bytes is to record them, and the
	// shell owns that.
	void capture_requested();

	// **Look again.** The page is looked up when this opens and the answer
	// cached (`media_lookups`); this drops it and asks once more. Named for
	// what it does rather than for the tool that does it today, since what
	// it runs may grow -- the holder, 2026-10-22. The dialog stays open and
	// the rows arrive in it.
	void refresh_requested();

private:
	void repopulate();
	void fit_host_column();
	void watch(const media_item &item);
	// `separate`: a second job even when the address is already being
	// fetched (`download_hints::separate`).
	void save(const media_item &item, bool separate = false);
	// What the Save button's menu chose. Asked each time, by the holder's
	// choice: whether a person wants the sound, the picture or both is not
	// something a kind decides.
	enum class save_choice { sound, picture, both };
	// The tallest picture one Save takes, by the holder's choice 2026-10-12:
	// 2160p of an hour-long set is 4.2 GB, 1080p about one. The rows still
	// offer everything.
	static constexpr int k_save_max_height = 1080;
	void save_as(save_choice choice);

	media_detector   *m_detector  = nullptr;
	player_launcher  *m_players   = nullptr;
	download_manager *m_downloads = nullptr;
	local_proxy      *m_proxy     = nullptr;
	mse_tap          *m_tap       = nullptr;
	// **Injected, not owned, and that is the whole point of it.** An assembly
	// outlives this dialog -- see `stream_assembly.h` for what owning it cost.
	stream_assembly  *m_assembly  = nullptr;
	stream_context    m_ctx;
	QString m_site;
	QString m_node_id;

	QTreeWidget *m_list   = nullptr;
	QLabel      *m_status = nullptr;
	QLabel      *m_lookup = nullptr;
	QPushButton *m_save   = nullptr;   // shown when yt-dlp's rows are listed
	// The "nothing here" message, over the empty rows area rather than under
	// it. `m_status` cannot carry this: it is the dialog's status line and
	// also reports assembling, progress and errors.
	empty_state *m_empty   = nullptr;
};
