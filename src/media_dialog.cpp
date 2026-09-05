#include "media_dialog.h"

#include "empty_state.h"
#include "download_manager.h"
#include "download_source.h"
#include "mse_tap.h"
#include "player_launcher.h"
#include "local_proxy.h"
#include "stream_assembly.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLocale>
#include <QEvent>
#include <QLabel>
#include <QPushButton>

namespace {

// **A control that cannot work looks unavailable**, which is this project's
// own rule -- see `gate_send` in ai_provider.h, written for the Send button
// after it sat enabled beside an explanation of why pressing it would fail.
// The media rows were the place the rule was not applied: Watch and Download
// were offered on every row and refused after the click, with the reason
// arriving as a message about something the user had already decided to do.
//
// The reason goes on the button, because "why is this greyed out" is a
// question asked of the button.
void gate(QAbstractButton *b, bool ok, const QString &why) {
	if (!b)
		return;
	b->setEnabled(ok);
	if (!ok)
		b->setToolTip(why);
}

// Whether anything here can fetch this item, and why not when it cannot.
//
// **HLS is fetchable even when no download source will take it**: `save()`
// routes it to the segment assembler instead, so asking the sources alone
// would grey a row that works. That is the trap in gating on `accepts()` by
// itself.
bool can_fetch(const media_item &item, download_manager *dm, QString *why) {
	if (item.kind == media_kind::hls)
		return true;
	if (!dm) {
		*why = "Downloads are not available in this window.";
		return false;
	}
	for (download_source *s : dm->sources()) {
		QString w;
		if (s->accepts(item.url, &w))
			return true;
		if (why->isEmpty() && !w.isEmpty())
			*why = w;
	}
	if (why->isEmpty())
		*why = "Nothing here knows how to fetch that address.";
	return false;
}

// Whether a player can be handed this, and why not when it cannot.
//
// A player that does not take manifests is not a refusal: `watch()` assembles
// an HLS stream into a progressive file first. What stops Watch entirely is
// having no player at all.
bool can_watch(const media_item &item, player_launcher *pl, QString *why) {
	if (!pl) {
		*why = "No player is configured in this window.";
		return false;
	}
	if (item.kind == media_kind::hls && !pl->selected_handles_streams())
		return true;
	if (pl->installed().isEmpty() && pl->custom_command().trimmed().isEmpty()) {
		*why = "No media player was found. Install one, or set a command in Settings.";
		return false;
	}
	return true;
}

}  // namespace

#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString kind_label(media_kind k) {
	switch (k) {
		case media_kind::hls:     return "HLS stream";
		case media_kind::dash:    return "DASH stream";
		case media_kind::direct:  return "File";
		case media_kind::segment: return "Segment";
		// Named rather than left to the fallthrough, so that a kind added
		// later is a warning here instead of quietly becoming "Unknown". It
		// should not arrive: nothing builds an item for a request classify
		// could not place.
		case media_kind::unknown:  return "Unknown";
	}
	return "Unknown";
}

}  // namespace

media_dialog::media_dialog(media_detector *detector, player_launcher *players,
                            download_manager *downloads, local_proxy *proxy,
                            mse_tap *tap, stream_assembly *assembly,
                            QWidget *parent)
  : QDialog(parent), m_detector(detector), m_players(players),
    m_downloads(downloads), m_proxy(proxy), m_tap(tap), m_assembly(assembly) {
	setWindowTitle("Media on this page");
	resize(720, 340);

	auto *outer = new QVBoxLayout(this);

	m_list = new QTreeWidget(this);
	m_list->setColumnCount(4);
	m_list->setHeaderLabels({"Type", "Name", "Host", ""});
	m_list->setRootIsDecorated(false);
	m_list->header()->setStretchLastSection(false);
	m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	outer->addWidget(m_list, 1);

	// **The empty state goes over the rows area, not under it.** It used to be
	// one of `m_status`'s texts, which left a dialog-sized black table with the
	// explanation stranded beneath it -- the tab tree and the downloads dialog
	// both centre theirs in the space that is empty, and this was the odd one
	// out. Nobody had seen it because the media dialog needs a loaded page to
	// photograph, so the capture pass had always skipped it -- `try_look` runs
	// against a local fixture now, so it does not.
	//
	// The placement is `empty_state`'s business; see its header for why the
	// overlay is in the viewport and why both its pointers are guarded.
	m_empty = new empty_state(m_list, this);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	outer->addWidget(m_status);

	// While this is open its own line is where somebody is looking, so the
	// assembly reports here too. It goes on reporting to the window after the
	// dialog closes -- the connection is bound to `this` and dies with it,
	// which is the whole reason the assembly is not.
	if (m_assembly)
		connect(m_assembly, &stream_assembly::status, this,
		         [this](const QString &text) { m_status->setText(text); });

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void media_dialog::set_site(const QString &site_host, const QString &node_id,
                            const stream_context &ctx) {
	m_site    = site_host;
	m_node_id = node_id;
	m_ctx     = ctx;
	repopulate();
}

void media_dialog::repopulate() {
	m_list->clear();
	const QList<media_item> items = m_detector->items_for(m_site);

	for (const media_item &item : items) {
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, kind_label(item.kind));
		row->setText(1, item.label);
		row->setText(2, item.url.host());

		// Both actions on every row; the list is already primary-first (sec 11.3).
		auto *cell    = new QWidget(m_list);
		auto *layout  = new QHBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *watch   = new QPushButton("▶ Watch", cell);
		auto *save    = new QPushButton("⬇ Download", cell);
		layout->addWidget(watch);
		layout->addWidget(save);
		QString why_watch, why_save;
		gate(watch, can_watch(item, m_players, &why_watch), why_watch);
		gate(save,  can_fetch(item, m_downloads, &why_save), why_save);
		// A player that will take it but warn about it stays enabled: a warning
		// is advice, not a refusal, and greying on one would hide a stream that
		// plays perfectly well.
		if (watch->isEnabled() && m_players) {
			const QString warn = m_players->warning_for(item);
			if (!warn.isEmpty())
				watch->setToolTip(warn);
		}
		connect(watch, &QPushButton::clicked, this, [this, item] { this->watch(item); });
		connect(save,  &QPushButton::clicked, this, [this, item] { this->save(item); });
		m_list->setItemWidget(row, 3, cell);
	}
	// What the tap can see but no URL can name (sec 11.6). These rows are not
	// streams anyone can fetch -- the bytes exist only inside the player -- so
	// the one thing offered is to record them.
	const QList<mse_stream> playing = m_tap ? m_tap->streams_for(m_site)
	                                        : QList<mse_stream>{};
	for (const mse_stream &s : playing) {
		if (s.bytes <= 0)
			continue;
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, "Playing");
		row->setText(1, QString("%1 · %2 buffered")
		                    .arg(s.mime, QLocale().formattedDataSize(s.bytes)));
		row->setText(2, m_site);

		auto *cell   = new QWidget(m_list);
		auto *layout = new QHBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *rec = new QPushButton("⏺ Capture", cell);
		rec->setToolTip("Record what the player is fed. The page reloads first, "
		                 "so the recording starts from the beginning.");
		layout->addWidget(rec);
		connect(rec, &QPushButton::clicked, this, [this] {
			emit capture_requested();
			accept();
		});
		m_list->setItemWidget(row, 3, cell);
	}

	// Type, host and the button cell take the width they need; Name is the
	// stretch column and absorbs whatever is left.
	//
	// **Host was the one column nobody sized**, so it kept a default width and
	// elided `hls.cdnvideo11...` while Name had several hundred pixels to
	// spare. Which host served a stream is exactly the thing somebody is
	// looking at this dialog to find out -- a media host is not the page's
	// host, and that difference is the whole point of the column.
	//
	// Sized to contents rather than stretched, because a host is bounded in a
	// way a name is not: it is a domain, and the longest one here is shorter
	// than most of the names beside it.
	m_list->resizeColumnToContents(0);
	m_list->resizeColumnToContents(2);
	m_list->resizeColumnToContents(3);

	if (!items.isEmpty()) {
		// **Counts the rows, not `items`.** `playing` adds rows of its own
		// below these, so counting `items` announced "1 item" over a list of
		// three -- a number that disagrees with what is on screen is worse
		// than no number, because the reader has to work out which is lying.
		const int rows = m_list->topLevelItemCount();
		m_status->setText(QString("%1 item%2. The first row is the stream this "
		                          "page looks to be playing.")
		                      .arg(rows).arg(rows == 1 ? "" : "s"));
	} else if (!playing.isEmpty()) {
		// The badge said this page is playing; the list must not then look
		// empty, which is how it read before these rows existed.
		m_status->setText("This page is playing video, but no stream URL could "
		                  "be found — the address is hidden, or the bytes never "
		                  "travel as one. Capture records it as it plays.");
	} else {
		// Said once, in the space it is about. The status line stays empty
		// here rather than repeating it two inches lower.
		m_empty->set_text("Nothing detected yet.\n\nMany sites only request "
		                   "the manifest when their player starts, so try "
		                   "pressing play first.");
		m_status->clear();
	}
}

void media_dialog::watch(const media_item &item) {
	// A player that cannot take a manifest gets an assembled progressive file
	// instead -- "the app compensates in the proxy for what the player lacks".
	if (item.kind == media_kind::hls && !m_players->selected_handles_streams()) {
		if (m_assembly)
			m_assembly->watch(item, m_ctx);
		else
			m_status->setText("<b>Nothing can assemble this stream.</b>");
		return;
	}

	const QString warn = m_players->warning_for(item);
	// Hand the player a localhost URL when the proxy is up, so the CDN sees
	// the page's Referer and cookies rather than a naked request (sec 11.3).
	// The page's own context, overlaid with anything this particular stream
	// asked for. A learned extractor names the headers its CDN checks, and
	// they are useless if they stop here.
	stream_context ctx = m_ctx;
	for (auto it = item.headers.cbegin(); it != item.headers.cend(); ++it) {
		const QString k = it.key().toLower();
		if (k == "referer" || k == "referrer") ctx.referer    = it.value();
		else if (k == "user-agent")            ctx.user_agent = it.value();
		else if (k == "cookie")                ctx.cookies    = it.value();
		else                                   ctx.extra.insert(it.key(), it.value());
	}
	const QUrl via = m_proxy ? m_proxy->publish(item.url, ctx) : QUrl();
	QString error;
	if (!m_players->play(item, &error, via)) {
		m_status->setText("<b>" + error.toHtmlEscaped() + "</b>");
		return;
	}
	m_status->setText(warn.isEmpty()
	                      ? QString("Opened in %1%2.").arg(m_players->selected(),
	                            via.isValid() ? " via the local proxy" : "")
	                      : "<b>Warning:</b> " + warn.toHtmlEscaped());
}

void media_dialog::save(const media_item &item) {
	// HLS is saveable now: fetch the segments and concatenate them (sec 11.2).
	if (item.kind == media_kind::hls) {
		if (m_assembly)
			m_assembly->save(item, m_ctx);
		else
			m_status->setText("<b>Nothing can assemble this stream.</b>");
		return;
	}

	QString error;
	// A learned stream carries the headers its CDN checks; without them the
	// download is refused where Watch would have succeeded.
	const int id = m_downloads->enqueue(item.url, m_node_id, &error, item.headers);
	// Where it lands, as the user will actually find it.
	//
	// On Android naming the directory is worse than useless: Qt downloads into
	// app-private storage, whose path is long, unopenable by any file manager,
	// and not where the finished file ends up anyway -- it is copied into the
	// shared Downloads collection when it completes (sec 19).
#ifdef Q_OS_ANDROID
	const QString where = QStringLiteral("Queued. It will appear in Downloads "
	                                      "when it finishes.");
#else
	const QString where =
	  QString("Queued download to %1.").arg(m_downloads->directory());
#endif
	m_status->setText(id ? where : "<b>" + error.toHtmlEscaped() + "</b>");
}
