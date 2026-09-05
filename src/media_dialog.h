#pragma once

#include "media_detector.h"
#include "local_proxy.h"

#include <QDialog>

class QLabel;
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

signals:
	// A page whose video only the tap can see has nothing to Watch or
	// Download -- the only way to get those bytes is to record them, and the
	// shell owns that.
	void capture_requested();

private:
	void repopulate();
	void watch(const media_item &item);
	void save(const media_item &item);

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
	// The "nothing here" message, over the empty rows area rather than under
	// it. `m_status` cannot carry this: it is the dialog's status line and
	// also reports assembling, progress and errors.
	empty_state *m_empty   = nullptr;
};
