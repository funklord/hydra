#include "main_window.h"

#include "auth_dialog.h"
#include "permission_dialog.h"
#include "screen_picker.h"
#include "cert_dialog.h"
#include "find_bar.h"
#include "tab_tree_model.h"
#include "tab_tree_view.h"
#include "tree_sort_proxy.h"
#include "state_store.h"
#include "tab_history.h"
#include "shutdown_signals.h"
#include "policy_engine.h"
#include "site_policy_dialog.h"
#include "web_view_backend.h"
#include "web_view_factory.h"
#include "address_input.h"
#include "scheme_rules.h"
#include "user_agent.h"
#include "kiosk_controller.h"
#include "reorganize_dialog.h"
#include "ollama_provider.h"
#include "claude_provider.h"
#include "media_detector.h"
#include "media_dialog.h"
#include "player_launcher.h"
#include "download_manager.h"
#include "http_download_source.h"
#include "torrent_download_source.h"
#include "downloads_dialog.h"
#include "settings_dialog.h"
#include "ytdlp_resolver.h"
#include "mse_tap.h"
#include "capture_source.h"
#include "extractor_signals.h"
#include "extractor_dialog.h"
#include "extractor_helpers.h"
#include "network_fetcher.h"
#include "ai_provider.h"
#include "local_proxy.h"
#include "filter_signals.h"
#include "filter_list.h"
#include "cosmetic_filters.h"
#ifdef Q_OS_ANDROID
#include "android_downloads.h"
#endif
#include "annoyed_dialog.h"
#include "filter_dialog.h"
#include "credential_store.h"
#include "keepass_bridge.h"
#ifdef Q_OS_ANDROID
#include "android_intents.h"
#endif
#include "session_import.h"
#include "autofill_controller.h"
#include "consent_blocker.h"
#include "consent_dialog.h"
#include "antiadblock_watch.h"
#include "autofill_script.h"
#include "element_picker.h"
#include "picker_script.h"
#include "policy.h"
#include "node.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSplitter>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QItemSelectionModel>
#include <QTreeView>
#include <QSettings>
#include <QStackedWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QToolBar>
#include <QLineEdit>
#include <QComboBox>
#include <QHeaderView>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <QLabel>
#include <QProgressBar>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QStyle>
#include <QClipboard>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QActionGroup>
#include <QAction>
#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QLocale>
#include <QStandardPaths>
#include <QDataStream>
#include <QCloseEvent>
#include <QSet>

#include <memory>

namespace {

// A toolbar icon from the desktop's own icon theme, falling back to the style.
//
// **The theme first, for the same reason the colour scheme follows the desktop:**
// a browser drawn with Breeze arrows on a Breeze desktop looks like it belongs
// there, and one drawn with its own arrows looks like a port. The freedesktop
// names are standard, so this is a lookup rather than a guess.
//
// `fallback` is a style icon for the cases every Qt style can draw. Where there
// is no such thing -- a key -- pass `SP_CustomBase` and the caller gets a null
// icon and keeps its text. That is deliberate: a toolbar with one hand-drawn
// glyph among borrowed ones looks worse than a toolbar with one word in it, and
// this is the situation the previous comment here was protecting against.
// A theme icon brought to a readable weight on this window's background.
//
// **crystalsvg's key is drawn as a pale outline**, which sits on a light
// toolbar at barely any contrast -- legible once you know it is there, easy to
// miss otherwise, and noticeably fainter than the arrows beside it. The answer
// is not to draw our own: the shape is the desktop's and should stay that way.
// Only its weight is adjusted.
//
// **Measured rather than hardcoded**, so a theme whose key is already dark is
// left alone instead of being crushed to black. The mean is taken over what is
// actually drawn -- weighted by alpha -- because an icon is mostly transparent
// and counting those pixels would report every icon as dark.
//
// Only on a light background. Pale-on-dark is correct, and a scheme-blind
// darkening would take an icon that reads well on dark chrome and make it
// disappear.
QIcon weighted_icon(const QIcon &src, const QPalette &pal) {
	if (src.isNull())
		return src;
	if (pal.color(QPalette::Window).lightness() <= 128)
		return src;
	// Dark enough to read against a light toolbar without becoming a black
	// blot beside icons that are themselves mid-grey.
	constexpr double target = 110.0;

	QList<QSize> sizes = src.availableSizes();
	if (sizes.isEmpty())
		sizes << QSize(16, 16) << QSize(22, 22) << QSize(24, 24) << QSize(32, 32);

	QIcon out;
	bool adjusted = false;
	for (const QSize &size : sizes) {
		QImage img = src.pixmap(size).toImage()
		                 .convertToFormat(QImage::Format_ARGB32);
		if (img.isNull())
			continue;
		double sum = 0.0, weight = 0.0, colour = 0.0;
		for (int y = 0; y < img.height(); ++y) {
			const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
			for (int x = 0; x < img.width(); ++x) {
				const int a = qAlpha(row[x]);
				if (!a)
					continue;
				sum    += double(qGray(row[x])) * a;
				weight += a;
				// How far from grey. `max - min` rather than a saturation in
				// HSL, which reports a near-white pixel as highly saturated
				// and would call every pale glyph coloured.
				const int hi = qMax(qMax(qRed(row[x]), qGreen(row[x])),
				                       qBlue(row[x]));
				const int lo = qMin(qMin(qRed(row[x]), qGreen(row[x])),
				                       qBlue(row[x]));
				colour += double(hi - lo) * a;
			}
		}
		// **A coloured icon is left alone, because its colour is the
		// message.** The shield's warning triangle is yellow and the annoyance
		// faces are yellow; darkening those turns a warning into a murky olive
		// and says something the icon did not mean.
		//
		// It excludes more than expected, and the surprise is worth recording:
		// crystalsvg's back and forward arrows are **blue discs**, measuring
		// 114 on this scale. They look grey on the toolbar because they are
		// usually *disabled* -- with nowhere to go back to -- and Qt draws a
		// disabled icon by desaturating and lightening it. The pale grey is
		// the state, not the artwork, which is why darkening the source would
		// not have touched it. Reload is the same disc and turns vivid blue
		// the moment a page is loaded.
		constexpr double colour_limit = 32.0;
		if (weight <= 0.0 || colour / weight > colour_limit ||
		      sum / weight <= target) {
			out.addPixmap(QPixmap::fromImage(img));
			continue;
		}
		const double factor = target / (sum / weight);
		for (int y = 0; y < img.height(); ++y) {
			QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
			for (int x = 0; x < img.width(); ++x) {
				const int a = qAlpha(row[x]);
				if (!a)
					continue;
				// Multiplied, not replaced: the glyph's own shading is what
				// makes it read as a key rather than a silhouette.
				row[x] = qRgba(int(qRed(row[x]) * factor),
				                  int(qGreen(row[x]) * factor),
				                  int(qBlue(row[x]) * factor), a);
			}
		}
		out.addPixmap(QPixmap::fromImage(img));
		adjusted = true;
	}
	// Nothing needed changing, so hand back the theme's own icon rather than a
	// re-rendered copy of it -- which would lose any size the theme can draw
	// that was not in `availableSizes`.
	return adjusted ? out : src;
}

QIcon themed_icon(const QStringList &names, QStyle *st,
                   QStyle::StandardPixmap fallback) {
	// Weighted on the way out, so every icon on this toolbar gets the same
	// treatment rather than one of them being adjusted by hand. The palette is
	// the application's: a toolbar is painted in it, and asking each caller to
	// pass one would be eleven chances to pass the wrong one.
	for (const QString &n : names) {
		const QIcon i = QIcon::fromTheme(n);
		if (!i.isNull() && !i.availableSizes().isEmpty())
			return weighted_icon(i, QApplication::palette());
	}
	// The style's own icon goes through it too. It is usually drawn to suit
	// the palette already, in which case the measurement leaves it alone --
	// but "usually" is not a reason to have two rules on one toolbar.
	return fallback == QStyle::SP_CustomBase
	           ? QIcon()
	           : weighted_icon(st->standardIcon(fallback),
	                            QApplication::palette());
}

}  // namespace


namespace {

// Schemes a browser is expected to render itself. Anything outside this set is
// not a page, which makes it a candidate for handing to something that can
// actually deal with it. Kept as a property of the web rather than of any
// engine, so the same rule holds behind the Android backend (sec 19.2).

}  // namespace

main_window::main_window(web_view_factory *factory, policy_engine *policy,
                          request_filter *filter, QWidget *parent)
  : QWidget(parent), m_factory(factory), m_policy(policy) {
	// The two interceptor consumers (architecture doc sec 10): both observe the
	// same request stream the blocker already rides, rather than adding a
	// second sensor.
	m_media      = new media_detector(m_policy, this);
	m_signals    = new filter_signals(this);
	m_annoyances = new annoyance_log();
	m_ex_signals = new extractor_signals(this);
	// Constructed before it is registered, which is not a style point: an
	// observer added while still null was harmless for exactly as long as the
	// function it landed in touched no members, and became a segfault in every
	// live driver the moment one did.
	m_antiadblock = new antiadblock_watch(this);
	m_filter = filter;
	if (filter) {
		filter->add_observer(m_media);
		filter->add_observer(m_signals);
		filter->add_observer(m_ex_signals);
		filter->add_observer(m_antiadblock);
	}
	m_extractors.load(QDir(QStandardPaths::writableLocation(
	                            QStandardPaths::AppDataLocation))
	                       .filePath("extractors.json"));
	// The sec 11.6 tap: what a page is actually feeding its <video>, for the sites
	// where watching request URLs finds nothing.
	m_mse = new mse_tap(this);
	connect(m_mse, &mse_tap::site_updated, this, [this](const QString &host) {
		refresh_media_affordance(host);
	});

	m_keepass  = new keepass_bridge(this);
	// Following Firefox, when asked to. The mirror only reports when the *tabs*
	// differ, so this connection does not fire every time Firefox saves a
	// scroll position.
	m_fx_mirror = new session_mirror(this);
	m_cr_mirror = new session_mirror(this);
	// Named so a driver can tell them apart; they are otherwise identical
	// objects and `findChildren` would return them in construction order,
	// which is not a fact worth depending on.
	m_fx_mirror->setObjectName("firefox_mirror");
	m_cr_mirror->setObjectName("chromium_mirror");
	connect(m_cr_mirror, &session_mirror::tabs_changed, this,
	        [this](const QList<session_import::imported_tab> &tabs) {
		show_mirror_tabs("chromium", "Chromium", tabs, /*from_poll=*/true);
	});
	connect(m_cr_mirror, &session_mirror::failed, this,
	        [this](const QString &why) { m_status->showMessage(why, 8000); });
	connect(m_fx_mirror, &session_mirror::tabs_changed, this,
	        [this](const QList<session_import::imported_tab> &tabs) {
		show_mirror_tabs("firefox", "Firefox", tabs, /*from_poll=*/true);
	});
	connect(m_fx_mirror, &session_mirror::failed, this,
	        [this](const QString &why) { m_status->showMessage(why, 8000); });

	m_autofill = new autofill_controller(m_keepass, m_policy, this);
	// More than one login for a site is a question only the user can answer, and
	// it is asked here rather than in the page: the controller holds the
	// passwords until the answer comes back, so the picker is a list of names
	// and the page learns nothing until a choice is made.
	connect(m_autofill, &autofill_controller::choice_needed, this,
	        [this](const QStringList &labels) {
		bool ok = false;
		const QString pick = QInputDialog::getItem(
		    this, "Which login?",
		    "More than one login is stored for this site:", labels, 0,
		    /*editable=*/false, &ok);
		// A dismissed dialog fills nothing, which is a legitimate answer and
		// not an error worth a message.
		m_autofill->choose(ok ? labels.indexOf(pick) : -1);
	});
	// The refusal is the key icon's job (sec 13.2) and there is no key icon yet, so
	// the status bar carries it meanwhile. Silence here is the thing to avoid:
	// "nothing stored for this site" and "KeePassXC is not running" produce the
	// same empty form, and only one of them is worth doing something about.
	// The key's four states. Each is a different thing to do next, which is why
	// they read differently rather than all being "autofill unavailable".
	connect(m_autofill, &autofill_controller::requested, this, [this] {
		if (!m_key_action)
			return;
		m_key_action->setEnabled(true);
		m_key_action->setText("Key");
		m_key_action->setToolTip("This page has a login form. Click to fill it "
		                          "from KeePassXC.");
	});
	connect(m_autofill, &autofill_controller::refused, this,
	        [this](const QString &why) {
		// On the key *and* in the status bar. The tooltip is where it stays
		// readable -- a status message is gone in six seconds and the form is
		// still sitting there empty.
		if (m_key_action) {
			m_key_action->setEnabled(true);
			m_key_action->setText("Key ✕");
			m_key_action->setToolTip(why);
		}
		m_status->showMessage(why, 6000);
	});
	// Offering to save, which is the one path that *writes* to the vault. Asked
	// with a plain question naming the login and the site and never showing the
	// password -- a prompt that displays it is a prompt that shoulder-surfs --
	// and never remembered as a preference: "never for this site" is a policy
	// decision and belongs in the shield beside the other per-site settings,
	// not in a checkbox on a transient dialog.
	connect(m_autofill, &autofill_controller::save_offered, this,
	        [this](const QString &login, const QString &host) {
		const QString who = login.isEmpty() ? QStringLiteral("this login")
		                                     : login;
		const bool yes = QMessageBox::question(
		    this, "Save to KeePassXC?",
		    QString("Save the password for %1 at %2?").arg(who, host)) ==
		    QMessageBox::Yes;
		m_autofill->confirm_save(yes);
	});
	connect(m_autofill, &autofill_controller::save_finished, this,
	        [this](bool, const QString &message) {
		m_status->showMessage(message, 6000);
	});

	connect(m_autofill, &autofill_controller::credentials_ready, this,
	        [this](const QString &) {
		if (!m_key_action)
			return;
		m_key_action->setEnabled(true);
		m_key_action->setText("Key ✓");
		m_key_action->setToolTip("Filled from KeePassXC. Click to fill again.");
	});
	m_consent  = new consent_blocker(m_policy, this);
	// Say it once, and say what the lever is. A page that will not run because
	// we block ads is indistinguishable from a broken site unless we tell them,
	// and only we know it was us.
	// A page that refuses to run because we block ads is fixed rather than
	// merely reported, and said out loud. The alternative -- telling the user
	// the shield exists and leaving them to it -- was tried first and is worse:
	// the message is easy to miss and the page stays broken, which reads as a
	// broken browser.
	//
	// Three things keep this from being presumptuous. It never overrides a
	// choice the user made for that site; it writes an ordinary per-site rule
	// the shield shows and can revert; and it happens once per site per session,
	// so a page that keeps asking cannot put the browser in a reload loop.
	connect(m_antiadblock, &antiadblock_watch::detected, this,
	         [this](const QString &host, const QString &what) {
		if (host.isEmpty())
			return;
		// An explicit per-site rule is the user speaking. Someone who blocked
		// ads on this site *knowing* it might break is not to be second-guessed
		// by the thing that noticed it broke.
		if (m_policy->setting_for(host, policy::feature::ads) !=
		        policy::setting::unset) {
			m_status->showMessage(
			    QString("%1 is checking for an ad blocker (%2). Your own setting "
			             "for this site is left as it is.").arg(host, what), 9000);
			return;
		}
		if (m_antiadblock_fixed.contains(host))
			return;
		m_antiadblock_fixed.insert(host);

		m_policy->set_setting(host, policy::feature::ads, policy::setting::allow);
		m_status->showMessage(
		    QString("%1 would not run with ads blocked (%2), so ads are now "
		             "allowed for this site and it is being reloaded — undo it "
		             "in the shield.").arg(host, what), 15000);

		// The allowance only affects requests from here on, and the page has
		// already been assembled without them, so it has to be loaded again or
		// the fix is invisible. Once per site, by the guard above.
		if (web_view_backend *v = current_view()) {
			if (v->url().host() == host) {
				apply_policy(v, host);
				v->reload();
			}
		}
	});
	// Non-empty from the start: a view can be built before any tree is loaded,
	// and an empty rule set is a blocker that silently does nothing.

	connect(m_consent, &consent_blocker::acted, this,
	         [this](const QString &host, const QString &choice) {
		m_status->showMessage(
		    QString("Answered the cookie banner on %1 (%2)").arg(host, choice), 6000);
	});
	// A policy change nobody asked for has to be visible when it happens, not
	// merely findable in the shield afterwards.
	connect(m_consent, &consent_blocker::relaxed_cookies, this,
	         [this](const QString &host) {
		m_status->showMessage(
		    QString("Allowed first-party cookies for %1 so its consent choice "
		             "sticks — change it in the shield").arg(host), 9000);
	});
	m_picker   = new element_picker(this);
	connect(m_picker, &element_picker::picked, this,
	         [this](const picked_element &) { open_filter_evolution(); });
	connect(m_picker, &element_picker::aborted, this, [this] {
		m_status->showMessage("Element pick cancelled.", 3000);
	});
	connect(m_keepass, &keepass_bridge::associated_changed, this,
	         [this](bool ok, const QString &msg) {
		m_status->showMessage((ok ? "KeePassXC: " : "KeePassXC: ") + msg, 6000);
	});
	connect(m_keepass, &keepass_bridge::error, this, [this](const QString &msg) {
		m_status->showMessage("KeePassXC: " + msg, 8000);
	});

	m_players   = new player_launcher;
	m_downloads = new download_manager(this);
#ifdef Q_OS_ANDROID
	// A download nobody can find afterwards has not really been downloaded: Qt's
	// download location on Android is app-private and invisible to every file
	// manager, so a finished file is copied into the shared Downloads collection.
	{
		auto *publisher = new android_downloads(m_downloads, this);
		connect(publisher, &android_downloads::published, this,
		         [this](const QString &, const QString &) {
			// Said in our own status bar rather than a system toast, because the
			// browser knows when it is showing something else that matters more.
			m_status->showMessage("Saved to Downloads.", 6000);
		});
		connect(publisher, &android_downloads::failed, this,
		         [this](const QString &path, const QString &why) {
			// Named, because the file is still there and still openable: the
			// copy failed, not the download.
			m_status->showMessage(QString("Downloaded, but not copied to "
			                               "Downloads (%1). It is at %2")
			                          .arg(why, path),
			                       10000);
		});
	}
#endif
	// The manager has no transport of its own (sec 11.4); give it one. Sources are
	// tried in order, so adding a torrent source later is one line here.
	m_downloads->add_source(new http_download_source);
	// BitTorrent is a first-class source, not a side feature (sec 11.4). It is
	// only present when the build found libtorrent; there is no degraded mode.
	// A recording is a download once it exists; it is only started differently.
	m_capture_src = new capture_source;
	m_downloads->add_source(m_capture_src);
	connect(m_capture_src, &capture_source::stop_requested, this,
	         [this](int) { if (!m_capture_url.isEmpty()) toggle_capture(); });

	if (torrent_download_source::available()) {
		m_torrents = new torrent_download_source;
		m_downloads->add_source(m_torrents);
	}
	// The sec 11.4 obligation that replaces the VPN we deliberately do not ship:
	// a source whose participation is publicly observable cannot start until
	// the user has been told what it does. The manager holds the job; this is
	// where the telling happens.
	// Queued, not direct. consent_required is emitted from inside
	// download_manager::enqueue(), and this handler opens a modal dialog -- so a
	// direct connection would run a nested event loop inside enqueue() and not
	// return until the user answered. Every caller would then have to be safe
	// against that, including the URL-scheme handler that turns an in-page
	// magnet link into a download, where a nested loop inside an engine
	// callback is a genuinely bad place to be. Deferring keeps enqueue() a
	// function that returns.
	connect(m_downloads, &download_manager::consent_required, this,
	         &main_window::confirm_public_download, Qt::QueuedConnection);

	// Links that are not pages (sec 11.4). The rule is deliberately general rather
	// than a test for one scheme: anything the browser will not render, but
	// which some download source will take, becomes a download. A magnet link
	// is simply the first thing that fits, and the shell still never names a
	// transport.
	if (factory) {
		// **A download a page started.** Reported rather than driven: the
		// engine keeps the transfer, because it may be a `blob:` the page
		// built or a url that only resolves with the session's cookies. All
		// the shell does is say where it went and whether it arrived, which
		// is the whole of what was missing -- an unaccepted download is
		// cancelled by Qt without a word, so every one of these used to look
		// like a click that did nothing.
		factory->set_download_handler([this](const QUrl &url, const QString &path,
		                                      bool finished, bool ok) {
			const QString name = QFileInfo(path).fileName();
			if (!finished) {
				m_status->showMessage(QString("Downloading %1…").arg(name), 6000);
				return;
			}
			m_status->showMessage(
			  ok ? QString("Saved %1 to %2")
			           .arg(name, QFileInfo(path).absolutePath())
			     : QString("Download of %1 failed").arg(
			           name.isEmpty() ? url.toString() : name), 12000);
		});

		factory->set_external_url_handler([this](const QUrl &url) {
			if (renders_as_page(url))
				return;
			if (!m_downloads->source_for(url)) {
				m_status->showMessage(
				  QString("Nothing here can open %1").arg(url.scheme() + ":"),
				  6000);
				return;
			}
			start_download(url);
		});
	}
	// The local proxy is optional: if it cannot listen, Watch still works and
	// simply hands over the raw URL (sec 10 -- it is an upgrade tier, not a
	// prerequisite).
	m_local_proxy = new local_proxy(this);
	m_local_proxy->start();
	m_filters   = new filter_list;
	// The cosmetic half of sec 12. It reads the same list the interceptor does, and
	// reaches pages the only way a `##` rule can: through the DOM.
	m_cosmetic  = new cosmetic_filters(m_filters, this);

	// Saved settings are applied here rather than by the settings dialog, so
	// they take effect on a run where that dialog is never opened -- otherwise
	// "settings persist" quietly means "settings persist if you go and look".
	settings_store::load_into(m_players, m_downloads, m_torrents, nullptr, nullptr);
	connect(m_media, &media_detector::site_updated,
	         this, &main_window::on_media_found, Qt::QueuedConnection);
	// The security spine is wired up before we get here: the factory owns the
	// profile with the interceptor and cookie filter already installed on it
	// (architecture doc sec 6/sec 7.3), and the policy engine is shared with them.

	m_model = new tab_tree_model(this);
	m_proxy = new tree_sort_proxy(this);
	m_proxy->setSourceModel(m_model);

	m_save_timer = new QTimer(this);
	m_save_timer->setSingleShot(true);
	m_save_timer->setInterval(1500);   // debounce structural saves
	connect(m_save_timer, &QTimer::timeout, this, &main_window::flush_tree);

	// **The view's own debounce, and deliberately not the tree's.** The two
	// are wired the same way and to the same purpose -- see the comment on
	// `structure_changed` below, which this follows -- but they must not share
	// a timer. `flush_tree` writes the whole outline and every tab's history;
	// hanging the view state off it would rewrite a ten-thousand-node file
	// every time somebody dragged a window edge, and dragging an edge is the
	// most frequent thing on this list by a wide margin.
	//
	// Longer than the tree's, because what feeds it is a gesture rather than
	// an edit: a resize or a drag through the tree emits continuously and
	// stops when the hand stops, and the point of the interval is to be past
	// the end of the gesture rather than inside it.
	m_view_timer = new QTimer(this);
	m_view_timer->setSingleShot(true);
	m_view_timer->setInterval(2500);
	connect(m_view_timer, &QTimer::timeout, this, &main_window::save_view_state);

	// **And the third thing a crash used to take: the tabs' own pasts.**
	// A blob reached disk only when its view was suspended, which happens on
	// the way out and nowhere else, so a crash returned every live tab to its
	// current url with nothing behind it.
	//
	// 5000ms, the longest of the three, and the two reasons pull the same way.
	// It is the most expensive writer -- measured below -- and it is the only
	// one whose loss window is bounded by a crash alone, since every ordinary
	// exit writes the blobs through `save_everything`. What feeds it is also a
	// discrete event rather than a gesture: a page load, of which a redirect
	// chain delivers several, and five seconds coalesces the burst.
	//
	// **Measured before it was built, which is what settled that it could be.**
	// `save_state()` is `QDataStream << *history()` and nothing else, so it was
	// timed on its own against Qt WebEngine, offscreen, on this machine:
	//
	//     history depth   1     4     12     30 entries
	//     per view       21    80    131    346 us
	//     blob          778  3220   9756  24624 bytes
	//
	// About 10us and 815 bytes per history entry over a small fixed cost. The
	// worst arrangement anybody is likely to have -- twenty tabs each thirty
	// pages deep -- is 6.9ms and 492kB, which is under one frame at 60Hz and
	// only after a load has settled. The open question was whether this was
	// affordable at all; it is, by a wide margin, and the number is here so
	// that nobody has to ask again.
	m_blob_timer = new QTimer(this);
	m_blob_timer->setSingleShot(true);
	m_blob_timer->setInterval(5000);
	connect(m_blob_timer, &QTimer::timeout, this, &main_window::flush_blobs);

	// A logout, a shutdown or a Ctrl-C ends the process without ever closing
	// the window, so `closeEvent` -- the only thing that writes the view state
	// and the tab blobs -- never ran for any of them. See shutdown_signals.h
	// for why the handler cannot do the saving itself.
	//
	// Quit rather than close: `save_everything()` has already done what
	// `closeEvent` would do, and calling `close()` here would run the whole
	// list a second time over views it has just suspended.
	auto *shutdown = new shutdown_signals(this);
	connect(shutdown, &shutdown_signals::received, this, [this](int signo) {
		qInfo("caught signal %d; saving before exit", signo);
		save_everything();
		QCoreApplication::quit();
	});
	if (!shutdown->armed())
		qWarning("shutdown signals not installed; a signalled exit will lose "
		          "the view state and any unsaved tabs");

	// --- Layout: menu bar, toolbar, splitter, status bar -----------------
	// No QMainWindow, so each piece of window furniture is an ordinary child
	// widget in the layout (see the note on the class declaration).
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	outer->addWidget(build_menu_bar());

	// --- Toolbar --------------------------------------------------------
	QToolBar *bar = new QToolBar(this);
	bar->setMovable(false);

	// Icons from the style, with the characters as their text. On the desktop
	// the characters alone were fine; on a phone the font had no glyph for the
	// reload arrow
	// and the reload button rendered as an empty box, while back and forward
	// happened to survive. Asking the style for the icon is both prettier and
	// not a bet on what fonts a platform ships.
	// The Go menu's own actions, added to the toolbar as well: one command, one
	// action, one enabled state. The toolbar is icon-only, so the menu text is
	// never drawn here -- and if a platform ships no icon for one of them, what
	// appears is the word rather than the box a missing glyph used to leave.
	QAction *back_act   = m_back_action;
	QAction *fwd_act    = m_fwd_action;
	QAction *reload_act = m_reload_action;
	// **The drawer button first, before Back.** It was fourth, after the
	// navigation trio, which put the one control that reveals *where you are* in
	// the middle of the controls that move you around. Leftmost is where the
	// phone apps that have this pattern put it -- Claude's among them -- and it
	// is the position a thumb finds without looking.
	//
	// Only shown in drawer mode; on a wide window the tree is already visible
	// and a button to reveal it would be a control that does nothing.
	m_drawer_action = bar->addAction("☰");
	bar->addAction(back_act);
	bar->addAction(fwd_act);
	bar->addAction(reload_act);
	m_drawer_action->setIcon(themed_icon({ "view-list-symbolic", "view-list-details",
		                                      "format-justify-fill" }, style(),
		                                    QStyle::SP_FileDialogDetailedView));
	m_drawer_action->setStatusTip("Show or hide the tab tree");
	m_drawer_action->setVisible(false);
	connect(m_drawer_action, &QAction::triggered, this,
	         [this] { set_drawer_open(!m_drawer_open); });

	back_act->setIcon(themed_icon({ "go-previous" }, style(), QStyle::SP_ArrowBack));
	fwd_act->setIcon(themed_icon({ "go-next" }, style(), QStyle::SP_ArrowForward));
	reload_act->setIcon(themed_icon({ "view-refresh" }, style(),
	                                 QStyle::SP_BrowserReload));
	back_act->setToolTip("Back");
	fwd_act->setToolTip("Forward");
	reload_act->setToolTip("Reload");
	bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
	update_navigation();

	// An `address_line` rather than a plain `QLineEdit`, so the on-screen
	// keyboard is asked for a Go key -- see `address_input.h` for why a hint
	// cannot do it.
	m_address = new address_line(this);
	m_address->setPlaceholderText("Address");
	m_address->setClearButtonEnabled(true);
	// **What the on-screen keyboard should do here, which nothing had told
	// it.** No input-method hints were set anywhere in this application, so on
	// a phone this box behaved like a message box: the first letter
	// capitalised, and predictive text composing and correcting as you go.
	// That is the reported "editing is a bit wonky", and it reaches further
	// than untidiness -- while an IME is composing, the key that should submit
	// commits the composition instead, so the address never loads and the box
	// simply sits there.
	//
	// **`ImhUrlCharactersOnly` is deliberately not among these**, tempting as
	// it looks. This box takes search terms as well as addresses --
	// `navigate_to_address` decides between them -- and a URL keyboard has no
	// space bar, so it would trade a wonky address for an unusable search.
	// `ImhLatinOnly` is out for the same reason: a search is whatever somebody
	// wants to type.
	m_address->setInputMethodHints(Qt::ImhNoAutoUppercase |
	                                Qt::ImhNoPredictiveText);
#ifdef Q_OS_ANDROID
	// **A Go button, because the keyboard's own key does not submit.**
	// Reproduced on a Galaxy Note 9: the action key reads *Next*, not Go, and
	// tapping it moves focus out of the field instead of navigating -- so the
	// address sits there, nothing loads, and the keyboard closes. A return
	// sent afterwards goes nowhere either, the field no longer having focus.
	//
	// The hints above fix the typing and cannot fix this: which action key an
	// IME offers is its own decision, and Qt exposes no hint that asks for Go.
	// Rather than guess at one, this stops depending on the keyboard at all --
	// which is what every mobile browser does, and is a control a person can
	// see.
	//
	// Android only. On the desktop Return already works and a button beside
	// the clear cross would be clutter for a key everybody has.
	QAction *go = m_address->addAction(
	  themed_icon({ "go-jump", "media-playback-start" }, style(),
	               QStyle::SP_ArrowForward),
	  QLineEdit::TrailingPosition);
	go->setToolTip("Go to this address");
	connect(go, &QAction::triggered, this, &main_window::navigate_to_address);
#endif
	connect(m_address, &QLineEdit::returnPressed, this, &main_window::navigate_to_address);
	bar->addWidget(m_address);

	// The media affordance sits next to the policy shield and stays hidden
	// until something is detected -- detection is progressive, so it fades in a
	// beat after load rather than being present and empty (sec 11.3).
	m_media_action = bar->addAction("Media");
	m_media_action->setIcon(themed_icon({ "applications-multimedia",
		                                     "media-playback-start" }, style(),
		                                   QStyle::SP_MediaPlay));
	m_media_action->setToolTip("Watch or download media on this page");
	m_media_action->setVisible(false);
	connect(m_media_action, &QAction::triggered, this, &main_window::open_media);

	// sec 13.2 asks for a key icon in the field or the toolbar, and now there is
	// one. This was a *text* action, on the argument that inventing a glyph for
	// one affordance would make it the odd one out -- true, and answered by not
	// inventing one: `dialog-password` is a standard freedesktop name that every
	// icon theme ships, so the key is the desktop's key, drawn to match the
	// arrows beside it.
	//
	// If a theme has neither name the icon comes back null and the action keeps
	// its word, which is what the whole toolbar did until today. An icon-only
	// button with nothing behind it would be worse than the text ever was.
	m_key_action = bar->addAction("Key");
	m_key_action->setIcon(themed_icon({ "dialog-password", "password" }, style(),
	                                   QStyle::SP_CustomBase));
	// An icon needs this in a way a word did not: with the label gone, the
	// tooltip is the only thing that says what the button is.
	m_key_action->setToolTip("Fill a saved password from KeePassXC");
	m_key_action->setStatusTip("Ask KeePassXC for this site's logins");
	// **Permanent, and disabled until this page has a login to fill.**
	//
	// It used to appear only when a page raised a form, which made the feature
	// invisible until the moment it was needed -- and something that appears
	// without being asked for is read as a notification rather than as a
	// control somebody can look for. A button that is always in the same place
	// can be found, and its enabled state answers "does this page have a login
	// at all" without anybody having to click.
	//
	// Disabled rather than live-and-explaining, because the honest failure
	// here is silence: with no fields on the page a fill has nothing to write,
	// so a live button would query the vault, offer a picker, and then appear
	// to do nothing. `blocked_reason` has no case for it and should not grow
	// one -- the page either has a form or it does not, and that is knowable
	// here without asking anybody.
	//
	// It also means a click is only possible when there is something to fill,
	// so the automatic request the page script makes on load
	// (`passwordFields().length` in `autofill_script.h`) is the only thing that
	// ever enables it. Nothing has to tell a manual click apart from a
	// detected form, because the disabled state makes the first impossible.
	reset_key_action();
	connect(m_key_action, &QAction::triggered, this, [this] {
		// Ask again, through the whole gate. Cheaper designs were available --
		// re-opening the last picker, or caching the entries -- and both mean
		// holding credentials for longer than the fill that asked for them.
		// Re-asking costs one round trip to a local socket and keeps sec 13.3.
		if (m_autofill)
			m_autofill->request_credentials(m_autofill->page_origin());
	});

	// **On the toolbar rather than in a menu, and that is the whole design.**
	// The value of this button is that it costs one click *at the moment*
	// something got through. Two clicks and a menu someone has to learn is a
	// button nobody presses while annoyed, which is the only time it is worth
	// pressing. See annoyance_log.h.
	m_annoyed_action = bar->addAction("Annoyed");
	m_annoyed_action->setIcon(themed_icon({ "face-angry", "face-sad",
		                                       "emblem-important" }, style(),
		                                     QStyle::SP_MessageBoxWarning));
	m_annoyed_action->setToolTip("Something got through here");
	m_annoyed_action->setStatusTip("Record what this page was doing, and pick a "
	                                "tool if one fits");
	connect(m_annoyed_action, &QAction::triggered, this,
	         &main_window::report_annoyance);

	// **Only when there is something to confirm.** A satisfaction button that
	// is always there measures who likes pressing buttons; this one appears
	// after rules are applied and asks the one question nothing else can
	// answer -- did they break the page? Over-blocking is silent: no error, no
	// console message, nothing in the request log, just a player that does not
	// start. Hidden again the moment it is answered.
	m_confirm_action = bar->addAction("Still working?");
	m_confirm_action->setIcon(themed_icon({ "face-smile", "dialog-question",
		                                       "emblem-default" }, style(),
		                                     QStyle::SP_MessageBoxQuestion));
	m_confirm_action->setToolTip("New rules were applied here — did they break "
	                              "anything?");
	m_confirm_action->setVisible(false);
	connect(m_confirm_action, &QAction::triggered, this,
	         &main_window::confirm_rules);

	QAction *shield_act = bar->addAction("Shield");
	shield_act->setIcon(themed_icon({ "security-high", "security-medium",
		                                 "channel-secure" }, style(),
		                               QStyle::SP_VistaShield));
	shield_act->setToolTip("Site controls");
	shield_act->setStatusTip("Permissions and filtering for this site");
	connect(shield_act, &QAction::triggered, this, &main_window::open_site_controls);

	outer->addWidget(bar);

	// --- Central splitter ----------------------------------------------
	m_tree = new tab_tree_view(this);
	m_tree->setModel(m_proxy);
	// One place to persist, however the change was made -- a drag, a rename, a
	// new folder. The tree file is the canonical record and a change that
	// survived only until the next launch would be worse than a refusal.
	connect(m_model, &tab_tree_model::structure_changed, this,
	        &main_window::save_tree_soon);
	// And the same for the view state, which the model cannot see. Opening a
	// folder and moving the current row change nothing in the tree file, so
	// `structure_changed` never fires for them and until now nothing but
	// `closeEvent` ever wrote them down.
	connect(m_tree, &QTreeView::expanded,  this, &main_window::save_view_soon);
	connect(m_tree, &QTreeView::collapsed, this, &main_window::save_view_soon);
	// After `setModel`, which is what creates the selection model: connecting
	// above it reads a null pointer, and Qt's connect on one is a runtime
	// warning rather than a compile error.
	if (QItemSelectionModel *sel = m_tree->selectionModel())
		connect(sel, &QItemSelectionModel::currentChanged,
		        this, &main_window::save_view_soon);
	// Locking the tab that is showing changes what Back and Forward may do, and
	// the toolbar has no other reason to look again. Cheap enough to run for
	// every structural change rather than adding a signal that means "a lock
	// moved": it reads two booleans off the current view.
	connect(m_model, &tab_tree_model::structure_changed, this,
	        &main_window::update_navigation);
	// A node about to be deleted may have a live view, and the model knows
	// nothing about views. Without this the view survived its node: it stayed in
	// the stack showing a page for a tab that no longer existed, stayed in the
	// map under an id nothing could resolve, and -- the part that made it more
	// than a leak -- stopped the live-view cap working, since the cap gives up
	// when it cannot resolve the victim it picked.
	connect(m_model, &tab_tree_model::about_to_remove, this,
	        &main_window::forget_subtree);
	// An id changes in exactly one case -- a row dragged out of a mirror, whose
	// id was minted in that mirror's namespace. Everything here that is keyed
	// by id has to follow it, or a tab that was open a moment ago is a live
	// view under a name nothing resolves: invisible, uncloseable, and holding
	// a slot against the live-view cap.
	connect(m_model, &tab_tree_model::id_changed, this,
	         [this](const QString &was, const QString &now) {
		if (web_view_backend *v = m_views_by_id.take(was))
			m_views_by_id.insert(now, v);
		const int at = m_lru.indexOf(was);
		if (at >= 0)
			m_lru[at] = now;
		// A mirrored tab has no state of its own -- a mirror is never saved --
		// but it can have been opened and suspended while it was on screen, and
		// that blob is real. Moved rather than dropped: the tab the user is
		// keeping is the one they were just looking at.
		if (m_state && m_state->has_state(was)) {
			m_state->save(now, m_state->load(was));
			m_state->remove(was);
		}
	});
	m_tree->setHeaderHidden(true);
	m_tree->setUniformRowHeights(true);
	// The title takes what is left; the history count takes exactly what it
	// needs. `setStretchLastSection` has to be turned off first or the last
	// section ignores the resize mode and fills the panel, which puts the
	// count against the far edge with a gulf between it and its row.
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(tab_tree_model::title_column,
	                                        QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(tab_tree_model::history_column,
	                                        QHeaderView::ResizeToContents);
	// The menu lives in the view; these are the only two entries it cannot
	// carry out itself -- opening needs an engine and the stacked widget,
	// suspending needs the state store.
	// Through a lambda rather than the member directly: `open_node` grew a
	// second, defaulted argument, and Qt resolves a pointer-to-member
	// connection on the whole signature -- a default does not make the slot
	// narrower than the signal.
	connect(m_tree, &tab_tree_view::open_requested, this,
	         [this](node *n) { open_node(n); });
	// A page out of an imported history opens *below* the tab it belongs to,
	// which is the sub-tab gesture the whole tree is built around (sec 5.5) --
	// and which leaves the record itself untouched.
	connect(m_tree, &tab_tree_view::history_open_requested, this,
	         [this](node *parent, const QUrl &url) {
		if (parent)
			open_child_tab(parent, url);
	});
	connect(m_tree, &tab_tree_view::suspend_requested, this, &main_window::suspend_node);
	connect(m_tree, &tab_tree_view::lock_requested, this, &main_window::toggle_lock);
	connect(m_tree, &tab_tree_view::open_externally_requested, this,
	        [this](node *n) { if (n) open_url_externally(QUrl(n->url)); });
	m_tree->expandAll();
	connect(m_tree, &QTreeView::activated, this, &main_window::on_tree_activated);
#ifdef Q_OS_ANDROID
	// A tap is not a double-click, and `activated` is what a double-click emits
	// on this style -- so on the device the tree looked responsive (rows
	// highlighted) and opened nothing, with the status bar still reading
	// "0 / 4 live" after several attempts. Touch gets `clicked` as well.
	//
	// Android only, deliberately: making a single click open a tab on the
	// desktop would change a behaviour nobody asked to have changed.
	connect(m_tree, &QTreeView::clicked, this, &main_window::on_tree_activated);
#endif
	connect(m_tree, &QTreeView::clicked,   this, &main_window::on_tree_activated);

	m_stack = new QStackedWidget(this);
	m_placeholder = new QLabel("Select a tab from the tree", this);
	m_placeholder->setObjectName("placeholder");
	m_placeholder->setAlignment(Qt::AlignCenter);
	m_stack->addWidget(m_placeholder);

	// Sort and Search filter the *tree*, so they belong above the tree rather
	// than in the toolbar beside Back/Forward/Address -- those act on the page.
	// Putting a control next to what it does not control is a small lie about
	// the layout, and it costs a beat every time to work out which pane the
	// search box searches.
	QWidget *sidebar = new QWidget(this);
	m_sidebar = sidebar;
	QVBoxLayout *side = new QVBoxLayout(sidebar);
	side->setContentsMargins(4, 4, 4, 0);
	side->setSpacing(4);

	m_search = new QLineEdit(sidebar);
	m_search->setPlaceholderText("Search tree");
	m_search->setClearButtonEnabled(true);
	connect(m_search, &QLineEdit::textChanged, this, &main_window::on_search_changed);
	side->addWidget(m_search);

	QHBoxLayout *sort_row = new QHBoxLayout;
	sort_row->setContentsMargins(0, 0, 0, 0);
	sort_row->setSpacing(4);
	sort_row->addWidget(new QLabel("Sort:", sidebar));
	m_sort_box = new QComboBox(sidebar);
	m_sort_box->addItem("Tree order");
	m_sort_box->addItem("Title A–Z");
	m_sort_box->addItem("Newest");
	m_sort_box->addItem("Recently seen");
	connect(m_sort_box, &QComboBox::currentIndexChanged,
	         this, &main_window::on_sort_mode_changed);
	connect(m_sort_box, &QComboBox::currentIndexChanged,
	         this, &main_window::save_view_soon);
	sort_row->addWidget(m_sort_box, 1);
	side->addLayout(sort_row);

	side->addWidget(m_tree, 1);

	QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
	m_splitter = splitter;
	splitter->addWidget(sidebar);
	splitter->addWidget(m_stack);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({280, 900});
	outer->addWidget(splitter, 1);   // takes all the leftover height

	// --- Status bar ------------------------------------------------------
	m_status = new QStatusBar(this);
	m_tab_counts = new QLabel(this);
	// Named so a driver can read the live-view count without scanning every
	// label for one whose text happens to match a pattern.
	m_tab_counts->setObjectName("tab_counts");

	// **Only on screen while something is loading.** A progress bar that is
	// always there, empty, is a permanent claim that something is happening;
	// one that appears is the only thing on the status bar that moves, which
	// is what makes it readable out of the corner of an eye.
	m_progress = new QProgressBar(this);
	m_progress->setObjectName("load_progress");
	m_progress->setRange(0, 100);
	m_progress->setTextVisible(false);
	m_progress->setMaximumWidth(120);
	m_progress->setMaximumHeight(12);
	m_progress->hide();
	m_status->addPermanentWidget(m_progress);
	m_status->addPermanentWidget(m_tab_counts);
	m_status->showMessage("Ready");
	m_find = new find_bar(this);
	outer->addWidget(m_find);
	connect(m_find, &find_bar::search, this,
	         [this](const QString &t, bool forward, bool fresh) {
		if (web_view_backend *v = current_view())
			v->find_text(t, forward, fresh);
	});
	connect(m_find, &find_bar::dismissed, this, [this] {
		// Clearing the term is what drops the engine's highlight. Hiding the
		// bar alone would leave the page marked up with a search nothing on
		// screen still refers to.
		if (web_view_backend *v = current_view())
			v->find_text(QString(), true, true);
		m_find->hide();
		if (web_view_backend *v = current_view())
			v->widget()->setFocus();
	});

	outer->addWidget(m_status);

	setWindowTitle("Hydra");

#ifdef Q_OS_ANDROID
	// **The Back button, which nothing was answering.** Qt's default for an
	// unhandled `Qt::Key_Back` is to finish the activity, so Back closed the
	// browser from anywhere -- including with a menu open, which is how it was
	// reported: menus that could not be dismissed without losing the app.
	//
	// On the application rather than on this window, because a popup takes the
	// keyboard grab while it is up and a filter installed here would never see
	// the one press that matters most.
	qApp->installEventFilter(this);
#endif
	resize(1180, 760);
#ifdef Q_OS_ANDROID
	// Nothing text-shaped gets focus at startup. Qt gives it to the first
	// focusable widget, which here is the address bar, and on a phone that
	// raises the soft keyboard over half the window before the user has asked
	// for anything -- seen on the device the moment the drawer worked.
	m_stack->setFocus(Qt::OtherFocusReason);
#endif

	// Realize this window with the visual the web engine needs, *before* it is
	// ever shown.
	//
	// The first web view added to a top-level that is already on screen makes
	// Qt recreate that window's native handle, and X then unmaps and remaps it:
	// measured at 30 fps, the whole browser window vanishes and the desktop
	// shows through for about a third of a second on the first tab open.
	// Building a view here and dropping it again forces the same handle to be
	// created up front, where there is nothing on screen to lose.
	//
	// Done through the factory rather than by naming an engine, so the seam
	// holds -- a backend with no such requirement simply pays a cheap create
	// and destroy (sec 19.2).
	if (m_factory) {
		web_view_backend *warm = m_factory->create_view(this);
		m_stack->addWidget(warm->widget());
		// Loading is what does it, not creating: the engine instantiates its
		// render widget on the first load, and *that* is what forces the
		// window to be rebuilt. A blank load is enough.
		warm->load(QUrl("about:blank"));
		winId();
		QWidget *w = warm->widget();
		m_stack->removeWidget(w);
		delete w;                           // the backend is parented to it
	}

	update_status();
	// Last, because it reads the stacked widget: called from the toolbar
	// builder it would find no stack and give up, and nothing else would ask
	// again until a page opened -- which left the three buttons enabled over an
	// empty window, the exact state this exists to prevent.
	page_changed();
}

QMenuBar *main_window::build_menu_bar() {
	// **Arranged the way desktop applications settled on between about 1995 and
	// 2010**, because that is the arrangement people already know: File, Edit,
	// View, Go, Tools, Help, in that order; the destructive and the rare below
	// the common; Quit last in File and About last in Help; related items in
	// separator-delimited groups of a few rather than one long list.
	//
	// The list this replaces had grown by appending. Tools held twenty items in
	// the order they were built -- Downloads, then an AI parser, then a video
	// capture, then Settings, then KeePassXC, then two importers with a sync
	// toggle wedged between them and an "open in another app" in the middle of
	// the pair. There was no Edit menu at all, so Undo lived at the bottom of
	// Tools and rename and delete lived nowhere. Nothing was wrong with any one
	// addition; the order was simply nobody's decision.
	QMenuBar *menu = new QMenuBar(this);
	// Keep the bar inside the window; this is an X11-only app and a platform
	// menu bar would detach it from a widget we lay out ourselves.
	menu->setNativeMenuBar(false);

	// ---- File: make things, bring things in, write things out, leave --------
	QMenu *file_menu = menu->addMenu("&File");
	QAction *new_tab_act = file_menu->addAction("New &Tab", QKeySequence::AddTab,
	                                            this, &main_window::new_tab);
	new_tab_act->setStatusTip("Add an empty tab under the selected folder");
	QAction *new_folder_act =
	    file_menu->addAction("New &Folder", QKeySequence("Ctrl+Shift+N"), this,
	                          &main_window::new_folder);
	new_folder_act->setStatusTip("Add a folder under the selection");
	file_menu->addSeparator();

	QAction *loc_act = file_menu->addAction("Open &Location…",
	                                         QKeySequence("Ctrl+L"), this, [this] {
		if (m_address) {
			m_address->setFocus(Qt::ShortcutFocusReason);
			m_address->selectAll();
		}
	});
	loc_act->setStatusTip("Type an address");
	QAction *ext = file_menu->addAction("Open in &Another App…", this, [this] {
		if (web_view_backend *v = current_view())
			open_url_externally(v->url());
		else
			m_status->showMessage("No page is open.", 5000);
	});
	ext->setStatusTip("Hand this address to another application — on a phone "
	                   "that is how audio keeps playing with the screen off");

	// Greyed rather than absent when there is no page, and greyed again on a
	// backend that cannot print at all: Android's WebView prints through the
	// system PrintManager and is not wired to it, so an unconditional entry
	// there would be a menu item that does nothing.
	m_print_action = file_menu->addAction("&Print…", QKeySequence::Print, this,
	                                       [this] {
		if (web_view_backend *v = current_view())
			v->print();
	});
	m_print_action->setStatusTip("Print this page");
	m_print_action->setEnabled(false);
	file_menu->addSeparator();

	// Both importers together, which is the one thing the flat list made
	// impossible: they were separated by an unrelated action and a sync toggle.
	QMenu *import_menu = file_menu->addMenu("&Import");
	QAction *imp = import_menu->addAction("Tabs from &Firefox", this,
	                                       [this] { import_firefox_tabs(); });
	imp->setStatusTip("Read the tabs Firefox has open and show them in a folder "
	                   "of their own");
	QAction *impc = import_menu->addAction("Tabs from &Chromium", this,
	                                        [this] { import_chromium_tabs(); });
	impc->setStatusTip("Replay Chromium's session log and show the tabs it "
	                    "leaves open");

	QAction *save_act = file_menu->addAction("&Save Tree", QKeySequence::Save,
	                                         this, &main_window::flush_tree);
	save_act->setStatusTip("Write the canonical tree file now");
	file_menu->addSeparator();
	QAction *quit_act = file_menu->addAction("&Quit", QKeySequence::Quit,
	                                         this, &QWidget::close);
	quit_act->setStatusTip("Suspend live tabs, save, and exit");

	// ---- Edit: the menu this application did not have -----------------------
	//
	// Undo first, which is where three decades of software has put it. It was at
	// the bottom of Tools, below four AI actions, under a name that only made
	// sense if you already knew what had happened.
	QMenu *edit_menu = menu->addMenu("&Edit");
	// Ctrl+Shift+Z rather than Ctrl+Z, so this never steals undo from a focused
	// text field in the page or the address bar.
	m_undo_action = edit_menu->addAction("&Undo Reorganize",
	                                      QKeySequence("Ctrl+Shift+Z"), this,
	                                      &main_window::undo_reorganize);
	m_undo_action->setEnabled(false);
	m_undo_action->setStatusTip("Put the tree back the way it was before the "
	                             "last accepted reorganization");
	edit_menu->addSeparator();

	QAction *copy_addr = edit_menu->addAction("&Copy Address",
	                                           QKeySequence("Ctrl+Shift+C"), this,
	                                           [this] {
		if (node *n = selected_node())
			if (!n->url.isEmpty())
				QGuiApplication::clipboard()->setText(n->url);
	});
	copy_addr->setStatusTip("Copy the selected tab's address");
	QAction *dup_act = edit_menu->addAction("Dup&licate", this, [this] {
		if (node *n = selected_node())
			m_model->duplicate_node(n);
	});
	dup_act->setStatusTip("Make a copy of the selection beside it");
	edit_menu->addSeparator();

	QAction *ren_act = edit_menu->addAction("&Rename…", QKeySequence(Qt::Key_F2),
	                                         this, [this] {
		if (node *n = selected_node())
			m_tree->edit_properties(n);
	});
	ren_act->setStatusTip("Edit the selection's title, address and notes");
	QAction *del_act = edit_menu->addAction("&Delete", QKeySequence::Delete, this,
	                                         [this] {
		if (node *n = selected_node())
			m_model->remove_node(n);
	});
	del_act->setStatusTip("Remove the selection and everything inside it");
	edit_menu->addSeparator();
	// **Ctrl+F searches the page**, which is what it does in every browser and
	// therefore what somebody arrives already expecting. The tree filter had it
	// and moves to Ctrl+Shift+F; that was a decision to ask about rather than
	// take, because it is a binding somebody was using daily.
	//
	// Only one action may hold a QKeySequence. Two is an ambiguous overload,
	// and Qt then fires neither of them reliably -- so this was briefly broken
	// for the tree filter as well as for itself.
	//
	// The mnemonic is on Page, because Find in Tree already has the F.
	QAction *page_find_act = edit_menu->addAction("Find on &Page",
	                                               QKeySequence::Find,
	                                               this, &main_window::open_find);
	page_find_act->setStatusTip("Search the page in front of you");
	edit_menu->addSeparator();

	QAction *all_act = edit_menu->addAction("Select &All", QKeySequence::SelectAll,
	                                         this, [this] { m_tree->selectAll(); });
	all_act->setStatusTip("Select every visible row");
	QAction *find_act = edit_menu->addAction("&Find in Tree…",
	                                          QKeySequence("Ctrl+Shift+F"), this,
	                                          [this] {
		if (m_search) {
			m_search->setFocus(Qt::ShortcutFocusReason);
			m_search->selectAll();
		}
	});
	find_act->setStatusTip("Filter the tree by title or address");

	// ---- View ---------------------------------------------------------------
	QMenu *view_menu = menu->addMenu("&View");
	QMenu *sort_menu = view_menu->addMenu("&Sort By");
	// The sort actions drive the toolbar combo rather than the proxy directly,
	// so the two controls cannot drift out of sync.
	static const char *sort_names[] = { "&Tree Order", "Title &A–Z",
		                                  "&Newest", "&Recently Seen" };
	QActionGroup *sort_group = new QActionGroup(this);
	for (int i = 0; i < 4; ++i) {
		QAction *a = sort_menu->addAction(sort_names[i]);
		a->setCheckable(true);
		a->setChecked(i == 0);
		sort_group->addAction(a);
		connect(a, &QAction::triggered, this, [this, i] { m_sort_box->setCurrentIndex(i); });
	}
	view_menu->addSeparator();
	view_menu->addAction("&Expand All", this, [this] { m_tree->expandAll(); });
	view_menu->addAction("&Collapse All", this, [this] { m_tree->collapseAll(); });
	view_menu->addSeparator();
	// Full screen last in View, which is where it has been since this menu had
	// a Toolbars submenu above it.
	// Page zoom. **A ladder rather than a multiplier**, because repeated
	// multiplication lands on levels nobody chose -- 1.331 -- and the way back
	// to 100% then depends on how you got there. These are the steps every
	// browser uses, and Actual Size is an absolute rather than an undo.
	view_menu->addSeparator();
	QAction *zin  = view_menu->addAction("Zoom &In", QKeySequence::ZoomIn, this,
	                                      [this] { step_zoom(+1); });
	QAction *zout = view_menu->addAction("Zoom &Out", QKeySequence::ZoomOut, this,
	                                      [this] { step_zoom(-1); });
	QAction *zoff = view_menu->addAction("&Actual Size", QKeySequence("Ctrl+0"),
	                                      this, [this] { step_zoom(0); });
	zin->setStatusTip("Make this page larger");
	zout->setStatusTip("Make this page smaller");
	zoff->setStatusTip("Back to 100%");
	view_menu->addSeparator();

	m_kiosk_action = view_menu->addAction("&Kiosk Mode", QKeySequence(Qt::Key_F11),
	                                       this, &main_window::toggle_kiosk);
	m_kiosk_action->setCheckable(true);
	m_kiosk_action->setStatusTip("Fullscreen chrome-less presentation; Esc returns");

	view_menu->addSeparator();
	// **A sub-tab, not a window or a pane**, which costs nothing here because
	// `view-source` is already a scheme `renders_as_page` accepts, so the
	// source is an ordinary tab and inherits suspend, restore, find-on-page and
	// the rest without any of them being told about it. Filing it under the
	// page it belongs to is what the tree is for: the alternative, a second
	// window, is the thing sec 5.5 exists to avoid.
	// **So&urce, not &Source.** `&Sort By` already holds Alt+S in this menu, and
	// two items sharing a mnemonic means the first press selects rather than
	// activates and the second cycles -- which reads as a menu entry that does
	// not work. `try_menus` caught it; nothing about the code says it.
	// Alt+U also agrees with the Ctrl+U accelerator, so the letter is the same
	// one either way in.
	m_source_action = view_menu->addAction("View Page So&urce",
	                                        QKeySequence("Ctrl+U"), this,
	                                        [this] { view_page_source(); });
	m_source_action->setStatusTip("Open this page's HTML as a sub-tab");
	m_source_action->setEnabled(false);

	// **Request desktop site**, which is the only thing that opens a site that
	// refuses phones outright. `teams.microsoft.com` is the measured case: it
	// redirects a mobile user agent to `/v2/unsupported-browser#isMobile=true`
	// server-side, and Chrome on Android gets the same page, so nothing about
	// looking more like a browser helps.
	//
	// Checkable and per tab, because it is a deliberate lie told for one page
	// rather than a claim about the machine -- and it reads its state back from
	// the view, so a tab that never had it set shows it off.
	//
	// Shown everywhere, enabled where it means something: on the desktop the
	// backend answers false and ignores it, and an entry that silently does
	// nothing is worse than one that is not there. `refresh_page_actions`
	// disables it when the current backend does not implement it.
	m_desktop_site_action = view_menu->addAction("Request &Desktop Site");
	m_desktop_site_action->setCheckable(true);
	m_desktop_site_action->setStatusTip(
	  "Ask this tab's pages to treat it as a desktop browser, and reload");
	connect(m_desktop_site_action, &QAction::triggered, this, [this](bool on) {
		if (web_view_backend *v = current_view())
			v->set_desktop_site(on);
	});

	// ---- Go -----------------------------------------------------------------
	QMenu *go_menu = menu->addMenu("&Go");
	// **Created here and lent to the toolbar, rather than made twice.** These
	// were three separate QActions wired to the same three slots, which meant
	// two enabled states per command: greying the toolbar's Back would have
	// left the menu's Back on, offering a route to the same nothing. The menu
	// bar is built before the toolbar, so this is where they come from.
	m_back_action = go_menu->addAction("&Back", QKeySequence::Back, this,
	                                    &main_window::go_back);
	m_fwd_action  = go_menu->addAction("&Forward", QKeySequence::Forward, this,
	                                    &main_window::go_forward);
	go_menu->addSeparator();
	m_reload_action = go_menu->addAction("&Reload", QKeySequence::Refresh, this,
	                                      &main_window::reload_page);

	// ---- Tools: grouped, with Settings last ---------------------------------
	//
	// Settings at the bottom under a separator is the strongest convention this
	// menu has, and it was fourth from the top between a video capture and an
	// AI parser.
	QMenu *tools_menu = menu->addMenu("&Tools");
	QAction *dl = tools_menu->addAction("&Downloads…", QKeySequence("Ctrl+J"),
	                                     this, &main_window::open_downloads);
	dl->setStatusTip("Show transfers in progress and finished");
	tools_menu->addSeparator();

	QMenu *media_menu = tools_menu->addMenu("&Media");
	QAction *find = media_menu->addAction("&Find Media on This Page…", this,
	                                       &main_window::find_media_with_ytdlp);
	find->setStatusTip("Ask yt-dlp what the video on this page is");
	m_capture_action = media_menu->addAction("&Capture Playing Video", this,
	                                          &main_window::toggle_capture);
	m_capture_action->setCheckable(true);
	m_capture_action->setStatusTip("Record what this page plays, for sites where "
	                                "no stream URL can be found");
	media_menu->addSeparator();
	QAction *learn = media_menu->addAction("&Learn This Site…", this,
	                                        &main_window::learn_this_site);
	learn->setStatusTip("Ask for a parser that finds this site's stream, and "
	                     "check it against what the page really requested");

	// Everything KeePassXC in one place, in the order a person meets it: pair,
	// use, unpair.
	QMenu *pw_menu = tools_menu->addMenu("&Passwords");
	QAction *kp = pw_menu->addAction("&Connect to KeePassXC…", this,
	                                  &main_window::toggle_password_manager);
	kp->setStatusTip(keepass_bridge::supported()
	                     ? QStringLiteral("Pair with a running KeePassXC for autofill")
	                     : keepass_bridge::unavailable_reason());
	kp->setEnabled(keepass_bridge::supported());
	// Generating is triggered from here rather than from the page. A page could
	// be given a way to ask, and then a page could ask unprompted -- and a
	// browser that hands out passwords because a script requested one is a
	// browser with a new attack surface for no benefit. The shell asks, and the
	// injected script puts the answer in the field that wanted it.
	QAction *kpg = pw_menu->addAction("&Generate Password", this, [this] {
		if (m_autofill)
			m_autofill->request_generated_password(m_autofill->page_origin());
	});
	kpg->setStatusTip("Ask KeePassXC for a password and put it in this page's "
	                   "new-password field");
	kpg->setEnabled(keepass_bridge::supported());
	pw_menu->addSeparator();
	// A pairing that is kept between runs has to be removable between runs, and
	// from here rather than from a keyring editor. It is the user's record of
	// having let this program at their vault; storing it silently and offering
	// no way back would be the wrong half of the feature to build.
	QAction *kpf = pw_menu->addAction("&Forget Pairing", this,
	                                   &main_window::forget_keepass_pairing);
	kpf->setStatusTip(
	    credential_store::available()
	        ? QStringLiteral("Remove the stored pairing; KeePassXC will ask "
	                          "again next time")
	        : credential_store::unavailable_reason());
	// Enabled on whether there is one to forget, asked now rather than assumed.
	kpf->setEnabled(keepass_bridge::supported() &&
	                keepass_bridge::pairing_is_stored());

	// The two mirrors together. They are the same feature twice and were four
	// items apart, so turning both on meant finding the second one by reading.
	QMenu *follow_menu = tools_menu->addMenu("Follow Other &Browsers");
	// Off unless asked for. Reading another program's files on a schedule is
	// not something to start doing because the feature exists, and the label
	// says what it costs rather than only what it gives.
	QAction *sync = follow_menu->addAction("Keep &Firefox Tabs in Sync");
	sync->setCheckable(true);
	sync->setStatusTip("Re-read Firefox's session every 15 seconds while it is "
	                    "running");
	connect(sync, &QAction::toggled, this, [this](bool on) {
		if (!on) {
			m_fx_mirror->stop();
			m_status->showMessage("No longer following Firefox.", 5000);
			return;
		}
		const QString path = session_import::firefox_session_path(
		  session_import::firefox_profile());
		if (path.isEmpty()) {
			m_status->showMessage("No Firefox session file to follow.", 8000);
			return;
		}
		m_fx_mirror->start("firefox", path);
	});
	QAction *syncc = follow_menu->addAction("Keep &Chromium Tabs in Sync");
	syncc->setCheckable(true);
	syncc->setStatusTip("Re-read Chromium's session every 15 seconds; it flushes "
	                     "about every 2.5 seconds, so this is the fresher of the two");
	connect(syncc, &QAction::toggled, this, [this](bool on) {
		if (!on) {
			m_cr_mirror->stop();
			m_status->showMessage("No longer following Chromium.", 5000);
			return;
		}
		const QString path = session_import::chromium_session_path(
		  session_import::chromium_profile());
		if (path.isEmpty()) {
			m_status->showMessage("No Chromium session file to follow.", 8000);
			return;
		}
		// Chromium's session flushes about every 2.5 s, six times more often
		// than Firefox's timer assumed. Measured at 1.3 ms per read of a real
		// 2.2 MB file, so following it more closely costs nothing worth
		// counting -- see the constant's own note.
		m_cr_mirror->start("chromium", path,
		                    session_mirror::k_chromium_interval_ms);
	});

	tools_menu->addSeparator();
	QAction *reorg = tools_menu->addAction("&Reorganize Tree with AI…", this,
	                                        &main_window::open_reorganizer);
	reorg->setStatusTip("Propose a new tree layout; nothing changes until you accept");
	QAction *eva = tools_menu->addAction("Evolve Ad &Filters…", this,
	                                      &main_window::open_filter_evolution);
	eva->setStatusTip("Propose filter rules for ads that slipped through here");
	QAction *zap = tools_menu->addAction("&Zap an Element…", this,
	                                      &main_window::start_element_picker);
	zap->setStatusTip("Click a leaked ad; Escape cancels");
	QAction *banners = tools_menu->addAction("&Cookie Banners We Missed…", this,
	                                          &main_window::open_site_rules);
	banners->setStatusTip("Teach a rule from a consent banner nothing matched");

	// **"Something got through here", by name, where somebody can find it.**
	//
	// It has been on the toolbar since it existed, and on a desktop that is
	// enough: hover it and the tooltip says what it is. A phone has no hover, so
	// on the handset it was an unlabelled icon between two other unlabelled
	// icons, and the tooltip -- the only thing that ever said what it does --
	// was unreachable.
	//
	// Reported in the plainest way there is: asked to press it, the answer came
	// back "i don't know where something got through here button is". The button
	// was two taps away the whole time and might as well not have existed.
	//
	// The same QAction as the toolbar's, so there is one enabled state and one
	// command; the menu entry carries the tooltip's words as its label, which is
	// what somebody is looking for when they go hunting.
	m_annoyed_menu_action =
	  tools_menu->addAction("Something Got Through &Here…", this,
	                         [this] { report_annoyance(); });
	m_annoyed_menu_action->setStatusTip(
	  "Record what this page was doing, and pick a tool if one fits");

	tools_menu->addSeparator();
	tools_menu->addAction("S&ite Controls…", this, &main_window::open_site_controls);
	QAction *prefs = tools_menu->addAction("&Settings…", QKeySequence::Preferences,
	                                        this, &main_window::open_settings);
	prefs->setStatusTip("Player, download folder and BitTorrent options");

	// ---- Help ---------------------------------------------------------------
	QMenu *help_menu = menu->addMenu("&Help");
	help_menu->addAction("&About", this, &main_window::on_about);

	return menu;
}

// A page asking to fill the screen, granted through the same stage kiosk mode
// uses. One mechanism rather than two: the stage already reparents a view into
// a frameless fullscreen top-level and puts it back afterwards, and a second
// way of doing that would be a second set of bugs.
//
// **The preset is not the kiosk one, though.** Kiosk is a presentation *policy*
// -- it can reload on an idle timer and on a dead renderer, both aimed at an
// unattended screen. Applied to somebody watching a video those are wrong in
// the same way: the page reloads underneath them and playback restarts. So the
// config here is the saved one with exactly those two disabled, and the home
// set to the page being watched rather than a kiosk target.
void main_window::present_fullscreen(web_view_backend *view, bool on) {
	if (!view)
		return;
	if (!on) {
		// **Keyed on which view the stage holds, never on `current_view()`.**
		// Entering removes the view from the stack, so while the stage is up
		// `current_view()` does not return it -- and the first version tested
		// exactly that, so a page asking to leave fullscreen was not recognised
		// as the presenting one and the stage stayed up. With F11 bound to a
		// window that is hidden, that left no way out at all.
		if (m_kiosk && m_kiosk->active() && m_kiosk->view() == view)
			toggle_kiosk();          // shares the `left` handler, so one path back
		return;
	}
	if (view != current_view())
		return;                       // only the visible tab may take the screen
	if (m_kiosk && m_kiosk->active())
		return;                       // already presenting; nothing to change

	m_page_fullscreen = true;
	toggle_kiosk();
	if (!m_kiosk || !m_kiosk->active()) {
		// Presentation refused. Tell the page rather than leaving it laid out
		// for a screen it never got.
		m_page_fullscreen = false;
		view->exit_fullscreen();
	}
}

void main_window::toggle_kiosk() {
	if (!m_kiosk) {
		m_kiosk = new kiosk_controller(this);
		// Forgetting between kiosk sessions is profile-wide work, and the
		// factory is what owns the profile. Without this the controller can
		// only warn that it was asked to forget and had nothing to ask.
		m_kiosk->set_view_factory(m_factory);
		// Coming back from kiosk: put the view on screen again and resync the
		// menu check, which also covers Esc exiting without going through here.
		connect(m_kiosk, &kiosk_controller::left, this, [this] {
			// **Not `m_kiosk->view()`, which is already null here.** `exit()`
			// clears its view before emitting this, so asking the controller
			// what came back always answered nothing and the whole restore
			// below was skipped: the widget never became a stack page again,
			// and the page was never told its fullscreen had ended. That is
			// the shape of all three symptoms -- a view the stack cannot find
			// so the shown tab and the selected row disagree, a page still
			// laid out for a screen it no longer has, and `requestFullscreen`
			// doing nothing because the page thinks it is already there.
			//
			// So the shell remembers what it handed over. It is the only side
			// that still knows.
			web_view_backend *back = m_kiosk_view;
			m_kiosk_view = nullptr;
			if (back) {
				// **addWidget, not the setParent() `exit()` already did.**
				// Re-parenting to a QStackedWidget makes a widget its child
				// and not one of its pages, so `currentWidget()` can never
				// return it and `current_view()` cannot see it.
				m_stack->addWidget(back->widget());
				// Esc, or anything else that ended the presentation. A page
				// that asked for fullscreen and was never told it ended keeps
				// its fullscreen layout and its own control stops working.
				if (m_page_fullscreen)
					back->exit_fullscreen();
			}
			m_page_fullscreen = false;
			m_kiosk_action->setChecked(false);
			show();
			// The view that came back, not `current_view()` -- that reads the
			// stack's current widget, so asking it here sets the current widget
			// to itself and changes nothing.
			if (back)
				m_stack->setCurrentWidget(back->widget());
			else if (web_view_backend *v = current_view())
				m_stack->setCurrentWidget(v->widget());
			sync_page_context();
			update_navigation();
			update_status();
		});
	}

	if (m_kiosk->active()) {
		m_kiosk->exit();
		return;
	}

	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Kiosk mode needs an open tab", 4000);
		m_kiosk_action->setChecked(false);
		return;
	}

	// Saved settings, not the controller's compiled-in defaults: this is the
	// whole difference between the kiosk page existing and mattering.
	kiosk_config c = settings_store::kiosk();
	if (m_page_fullscreen) {
		// **A page may not borrow the lockdown.** `allow_escape=false` is a
		// deliberate choice for an unattended screen somebody set up; applied
		// to a website that asked for the screen it means the site decides you
		// cannot leave, which is not a thing a page is allowed to decide. The
		// two reloads are off for the same reason as below -- a video should
		// not restart underneath whoever is watching it.
		c.allow_escape = true;
		c.watchdog = false;
		c.idle_reset_seconds = 0;
		// **Nor the forgetting.** Clearing between sessions is a decision
		// about an unattended screen somebody set up, and it deletes the
		// cookies and logins of whoever set it up. A video going fullscreen
		// is not consent to be signed out of every site -- and this route is
		// the same controller, so without this line the kiosk leak fix
		// becomes a data-loss bug for anyone who turned it on.
		c.clear_between_sessions = false;
	}
	// A configured home wins, because someone who set one meant it for exactly
	// this. With none, the tab you were on is the sensible thing to show and is
	// what it has always done.
	if (!c.home.isValid() || c.home.toString().isEmpty())
		c.home = v->url();
	m_kiosk->set_config(c);

	m_stack->removeWidget(v->widget());   // the controller takes it from here
	m_kiosk_view = v;                      // the only record of what to restore
	if (m_kiosk->enter(v, m_stack)) {
		m_kiosk_action->setChecked(true);
		hide();                            // the stage is its own fullscreen window
	} else {
		m_stack->addWidget(v->widget());
		m_kiosk_action->setChecked(false);
	}
}

void main_window::open_reorganizer() {
	// Which backend, and whether one is allowed at all, is a single global
	// setting resolved in one place (sec 9.1). The dialog still gates an external
	// provider behind review-before-send either way.
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	// sec 9.4: snapshot before applying, so any accepted change is one keystroke
	// to revert. Structure only -- payloads follow ids and are never touched.
	const tree_snapshot before = m_model->take_snapshot();
	reorganize_dialog dlg(m_model, chosen, this);
	if (dlg.exec() == QDialog::Accepted) {
		m_undo = before;
		m_undo_action->setEnabled(true);
		m_tree->expandAll();
		mark_dirty();
	}
}

void main_window::on_media_found(const QString &site_host, int count) {
	Q_UNUSED(count)
	refresh_media_affordance(site_host);
}

void main_window::refresh_media_affordance(const QString &site_host) {
	web_view_backend *v = current_view();
	if (!v || v->url().host() != site_host)
		return;   // detection on a background tab; don't retitle this one

	const int  found   = m_media->count_for(site_host);
	const bool playing = m_mse && m_mse->active_for(site_host);
	m_media_action->setVisible(found > 0 || playing);

	if (found > 0) {
		m_media_action->setText(QString("Media (%1)").arg(found));
		m_media_action->setToolTip("Watch or download media on this page");
		return;
	}

	// The sec 11.6 case: the page is demonstrably playing, and watching request
	// URLs found nothing. Saying so is worth more than an empty badge, which
	// on such a site is simply a lie.
	qint64 buffered = 0;
	QString mime;
	for (const mse_stream &s : m_mse->streams_for(site_host)) {
		buffered += s.bytes;
		if (mime.isEmpty())
			mime = s.mime;
	}
	m_media_action->setText("Media (playing)");
	m_media_action->setToolTip(
	  QString("This page is playing video (%1 buffered, %2), but no stream "
	           "URL was detected — try Tools ▸ Find Media on This Page.")
	      .arg(QLocale().formattedDataSize(buffered), mime));
}

void main_window::open_media() {
	web_view_backend *v = current_view();
	if (!v) {
		// The odd one out: every other action needing a page says so, and this
		// returned in silence. Hard to reach, since the Media button is hidden
		// until something is detected -- but "the button did nothing" is the
		// worst thing a button can do, and the difference is one line.
		m_status->showMessage("Open a page first — there is no media without "
		                       "one.", 5000);
		return;
	}
	QString node_id;
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value() == v) { node_id = it.key(); break; }

	// Anything this site has been taught (sec 11.5) runs before the list is built,
	// so a learned stream appears beside whatever detection found on its own.
	apply_extractor(v->url().host(), v->url());

	// The context a CDN expects: the page that loaded the stream, and this
	// browser's own User-Agent.
	stream_context ctx;
	ctx.referer = v->url().toString();
	media_dialog dlg(m_media, m_players, m_downloads, m_local_proxy, m_mse, this);
	connect(&dlg, &media_dialog::capture_requested, this, [this] {
		// Queued: the dialog is closing itself, and starting a capture reloads
		// the page -- not something to do from inside its own exec().
		QTimer::singleShot(0, this, [this] {
			if (m_capture_url.isEmpty())
				toggle_capture();
		});
	});
	dlg.set_site(v->url().host(), node_id, ctx);
	dlg.exec();
}

void main_window::report_annoyance() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first — there is nothing to report "
		                       "about an empty tab.", 5000);
		return;
	}
	const QUrl page = v->url();
	const QString host = page.host();
	if (host.isEmpty()) {
		m_status->showMessage("This page has no site to file against.", 5000);
		return;
	}

	// **Nothing is captured here that was not already being collected.**
	// `filter_signals` accumulates the ad-shaped requests and the full corpus
	// as a side effect of the interceptor; this takes a copy of them at the
	// moment the button was pressed. That is the whole trick -- the report is a
	// marker on existing evidence, so filing one costs a click and no memory.
	annoyance_report r;
	r.when     = QDateTime::currentDateTime();
	r.host     = host;
	r.page     = page.toString();
	if (m_signals) {
		r.capabilities = m_signals->capabilities_for(host);
		r.suspects = m_signals->suspects_for(host);
		// `observed_for`, not `count_for`: the latter counts *suspects*, so
		// using it here made the dialog say "N requests seen, N of them
		// ad-shaped" for every page. Caught by pointing the driver at a real
		// site, where both numbers came back 0 while every check passed.
		r.observed = m_signals->observed_for(host).size();
	}

	// Filed *before* the dialog, deliberately. Somebody who presses this and
	// then closes the window has still told us something, and losing that
	// because they did not pick one of three tools would make the button a
	// worse version of the tools it is meant to feed.
	if (m_annoyances) {
		m_annoyances->add(r);
		if (!m_annoyances_path.isEmpty())
			m_annoyances->save(m_annoyances_path);
	}

	annoyed_dialog dlg(r, this);
	dlg.exec();
	const annoyed_dialog::action chose = dlg.chosen();
	if (m_annoyances) {
		m_annoyances->set_outcome(host, annoyed_dialog::name_of(chose));
		if (!m_annoyances_path.isEmpty())
			m_annoyances->save(m_annoyances_path);
	}

	switch (chose) {
	case annoyed_dialog::action::zap:     start_element_picker();   break;
	case annoyed_dialog::action::evolve:  open_filter_evolution();  break;
	case annoyed_dialog::action::consent: open_site_rules();        break;
	case annoyed_dialog::action::recorded:
		m_status->showMessage(
		    QString("Recorded. %1 report%2 filed against %3.")
		        .arg(m_annoyances ? m_annoyances->count_for(host) : 0)
		        .arg((m_annoyances && m_annoyances->count_for(host) == 1) ? "" : "s")
		        .arg(host), 6000);
		break;
	}
}

void main_window::open_filter_evolution() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first — filter evolution works from "
		                      "what that page requested.", 5000);
		return;
	}
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	// What the list held before, so what the proposal added can be named
	// afterwards. The dialog does not report it and does not need to: a rule is
	// its text, and the difference of two sets of texts is exactly the set that
	// would have to be removed to undo this.
	QSet<QString> before;
	if (m_filters)
		for (const filter_rule &r : m_filters->rules())
			before.insert(r.text);

	filter_dialog dlg(m_signals, m_filters, chosen, v->url().host(),
	                   m_picker->last(), this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	if (!m_filters_path.isEmpty())
		m_filters->save(m_filters_path);

	QStringList added;
	if (m_filters)
		for (const filter_rule &r : m_filters->rules())
			if (!before.contains(r.text))
				added << r.text;
	offer_confirmation(added, v->url().host());
}

// Put the question only when there is one to put. Nothing added means nothing
// to have broken, and an "is it still working?" after a no-op is the kind of
// prompt that teaches people to ignore prompts.
void main_window::offer_confirmation(const QStringList &added,
                                      const QString &host) {
	if (added.isEmpty() || host.isEmpty())
		return;
	m_unconfirmed_rules = added;
	m_unconfirmed_host  = host;
	if (m_confirm_action)
		m_confirm_action->setVisible(true);
	m_status->showMessage(
	    QString("%1 rule%2 applied to %3. If the page stops working, the "
	             "toolbar can undo them.")
	        .arg(added.size()).arg(added.size() == 1 ? "" : "s").arg(host),
	    9000);
}

void main_window::confirm_rules() {
	if (m_unconfirmed_rules.isEmpty()) {
		if (m_confirm_action)
			m_confirm_action->setVisible(false);
		return;
	}
	QMessageBox box(this);
	box.setWindowTitle("Did those rules break anything?");
	box.setObjectName("confirm_rules");
	box.setText(QString("%1 rule%2 %3 applied to <b>%4</b>.")
	                .arg(m_unconfirmed_rules.size())
	                .arg(m_unconfirmed_rules.size() == 1 ? "" : "s")
	                .arg(m_unconfirmed_rules.size() == 1 ? "was" : "were")
	                .arg(m_unconfirmed_host.toHtmlEscaped()));
	// The rules themselves, because "undo the thing you cannot see" is not a
	// question anybody can answer well.
	box.setDetailedText(m_unconfirmed_rules.join('\n'));
	box.setInformativeText(
	    "Over-blocking is silent — a rule that kills a player or a login form "
	    "produces no error at all. If something here stopped working, this is "
	    "the moment to say so.");
	QPushButton *keep = box.addButton("Still &Works", QMessageBox::AcceptRole);
	QPushButton *undo = box.addButton("It &Broke — Undo Them",
	                                   QMessageBox::DestructiveRole);
	box.setDefaultButton(keep);
	box.exec();

	QAbstractButton *answer = box.clickedButton();
	if (answer == undo) {
		int removed = 0;
		for (const QString &text : std::as_const(m_unconfirmed_rules))
			if (m_filters && m_filters->remove(text))
				++removed;
		if (m_filters && !m_filters_path.isEmpty())
			m_filters->save(m_filters_path);
		m_status->showMessage(
		    QString("Removed %1 rule%2. Reloading %3.")
		        .arg(removed).arg(removed == 1 ? "" : "s").arg(m_unconfirmed_host),
		    8000);
		// Reloaded, because a cosmetic rule is applied at page load and the
		// page in front of somebody who just said it is broken is still broken
		// until it is fetched again.
		if (web_view_backend *v = current_view())
			v->reload();
	} else if (answer == keep) {
		m_status->showMessage(
		    QString("Kept %1 rule%2 for %3.")
		        .arg(m_unconfirmed_rules.size())
		        .arg(m_unconfirmed_rules.size() == 1 ? "" : "s")
		        .arg(m_unconfirmed_host), 6000);
	}
	// Answered either way -- including dismissed, which is an answer of "do not
	// ask me again about this" rather than a reason to keep nagging.
	m_unconfirmed_rules.clear();
	m_unconfirmed_host.clear();
	if (m_confirm_action)
		m_confirm_action->setVisible(false);
}

void main_window::toggle_password_manager() {
	if (!keepass_bridge::supported()) {
		// The reason, not a guess at it: on a desktop without libsodium the
		// answer is to install it, and on Android there is nothing to install.
		m_status->showMessage(keepass_bridge::unavailable_reason(), 8000);
		return;
	}
	if (!m_keepass->connected()) {
		connect(m_keepass, &keepass_bridge::ready, this, [this] {
			// A stored pairing is tested rather than re-created, so the user is
			// not asked to confirm a new connection on every launch (sec 13.1).
			//
			// This comment described what was *meant* to happen for as long as
			// there was nowhere to store a pairing: `associated()` only ever
			// answered for this process's memory, which on a fresh launch is
			// empty, so the test branch was unreachable and every launch asked
			// the user to confirm again. Loading it is what makes it true.
			if (m_keepass->restore_pairing() || m_keepass->associated())
				m_keepass->test_association();
			else
				m_keepass->associate();
		}, Qt::SingleShotConnection);
		m_status->showMessage("Connecting to KeePassXC…", 4000);
		m_keepass->start();
	} else if (!m_keepass->associated()) {
		m_keepass->associate();
	} else {
		m_status->showMessage("Already paired with KeePassXC.", 4000);
	}
}

void main_window::forget_keepass_pairing() {
	if (!keepass_bridge::pairing_is_stored()) {
		m_status->showMessage(credential_store::available()
		                          ? QStringLiteral("No stored KeePassXC pairing.")
		                          : credential_store::unavailable_reason(),
		                      6000);
		return;
	}
	// Confirmed, because the cost of undoing it is the one step that needs a
	// human at the KeePassXC end -- this is cheap to click and not cheap to
	// reverse.
	if (QMessageBox::question(
	        this, "Forget KeePassXC pairing",
	        "Remove the stored pairing?\n\nKeePassXC will ask you to confirm a "
	        "new connection the next time this browser asks for a password. "
	        "Nothing in your vault is changed.") != QMessageBox::Yes)
		return;
	const bool gone = m_keepass->forget_pairing();
	m_status->showMessage(gone ? QStringLiteral("KeePassXC pairing forgotten.")
	                           : QStringLiteral(
	                                 "Could not remove the stored pairing."),
	                      6000);
}

void main_window::undo_reorganize() {
	if (!m_undo.valid())
		return;
	const int n = m_model->restore_snapshot(m_undo);
	m_undo = tree_snapshot{};        // one level, as sec 9.4 specifies
	m_undo_action->setEnabled(false);
	m_tree->expandAll();
	mark_dirty();
	m_status->showMessage(QString("Reverted the reorganization (%1 nodes).").arg(n),
	                       5000);
}

void main_window::open_site_rules() {
	consent_dialog dlg(m_consent, m_site_rules_path, this);
	dlg.exec();
}

void main_window::start_element_picker() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 4000);
		return;
	}
	m_status->showMessage("Click the element to block — Escape cancels.", 0);
	m_picker->begin(v->url().toString());
}

void main_window::on_about() {
	// **The address is escaped because this box is rich text.**
	// `QMessageBox::about` renders HTML, so a bare <nabeel@vibes.se> is an
	// unknown tag and is dropped silently -- the line would read "Copyright
	// (C) 2026 Nabeel Sowan" with the address simply gone, which is the half
	// of it a reader would need to act on.
	QMessageBox::about(this, "About Hydra",
	  "<b>Hydra</b><br>"
	  "A tab-tree browser over an embedded Chromium, with a per-site "
	  "security policy engine.<br><br>"
	  "Copyright (C) 2026 Nabeel Sowan &lt;nabeel@vibes.se&gt;<br>"
	  "All rights reserved. No licence is granted.");
}

void main_window::update_status() {
	const int live = m_views_by_id.size();
	m_tab_counts->setText(QString("%1 / %2 live").arg(live).arg(k_max_live_views));
}

bool main_window::event(QEvent *e) {
	// A QMainWindow forwards status tips to its status bar for free. Without
	// one, hovering a menu item would do nothing, so route them here.
	if (e->type() == QEvent::StatusTip) {
		const QString tip = static_cast<QStatusTipEvent *>(e)->tip();
		if (m_status)
			m_status->showMessage(tip.isEmpty() ? QStringLiteral("Ready") : tip);
		return true;
	}
	return QWidget::event(e);
}

main_window::~main_window() {
	delete m_players;
	delete m_filters;
	// Not a QObject, so it has no parent to take it. Kept as a raw pointer to
	// match the two above rather than being the one member with a different
	// lifetime idiom.
	delete m_annoyances;
	// Leave kiosk while our children still exist: the controller hands the
	// presented view's widget back to the stack, and doing that after the
	// stack has been destroyed is a use-after-free. Destructor bodies run
	// before QObject tears down children, so here is the right place.
	if (m_kiosk && m_kiosk->active())
		m_kiosk->exit();
}

bool main_window::load_tree(const QString &path) {
	// **The directory has to exist already, and refusing is the fix for a bug
	// rather than fussiness.**
	//
	// Everything this window persists is derived from the tree's own
	// directory -- the state store, the view state, the policy, the filters --
	// and `state_store` creates its directory with `mkpath`, which builds
	// every missing component. So an argument that is not a real path becomes
	// a directory tree rooted wherever the browser happened to be started.
	//
	// Measured twice in this repository, by two different accounts and neither
	// noticed at the time: `hydra file:///tmp/x/page.html` left a directory
	// literally named `file:` in the checkout, holding `tmp/x/` and an empty
	// `state/` beneath it. `QFileInfo("file:///tmp/x/page.html").absolutePath()`
	// is the *relative* path `file:/tmp/x`, because a url is not a path and
	// nothing here had said so.
	//
	// A tree path naming a directory that does not exist is a typo, a url, or
	// something else that was never a tree. Creating it silently is what turned
	// both incidents into junk in a git repository instead of one line on
	// stderr. Nothing is assigned before this returns, so every writer below
	// stays guarded by the `isEmpty()` checks it already has and the refusal
	// leaves no trace anywhere.
	//
	// Whether a `file:` url should be opened as a page rather than read as a
	// tree is a separate question, deliberately decided in `main.cpp` and not
	// answered here.
	const QString dir = QFileInfo(path).absolutePath();
	if (!QFileInfo(dir).isDir()) {
		qCritical("tree: %s is not in an existing directory (%s); refusing to "
		           "create one, because an argument that is not a path is how "
		           "a url became a directory tree here twice",
		           qPrintable(path), qPrintable(dir));
		return false;
	}

	m_tree_path = path;
	delete m_state;
	m_state = new state_store(dir + "/state");

	// .ini, and the loader reads a policy.json left by an older build once --
	// the first save writes the new file and the old one simply stops being
	// consulted. Nobody has to migrate anything by hand.
	// The view's own state, beside the policy and for the same reason: it is
	// this window's, it is readable, and somebody who wants it forgotten can
	// delete a file rather than hunt through a store they cannot see.
	m_view_path = dir + "/view.ini";
	m_policy_path = dir + "/policy.ini";
	m_policy->load(m_policy_path);   // no-op if the file doesn't exist yet

	// The AI/user-authored filter list lives beside the rest, kept separate
	// from any imported EasyList so upstream updates cannot clobber it (sec 12.5).
	// Beside the policy, because a record of what somebody found annoying is a
	// record of where they have been, and it belongs where they can read and
	// clear it rather than in a store they cannot see.
	m_annoyances_path = dir + "/annoyances.ini";
	if (m_annoyances)
		m_annoyances->load(m_annoyances_path);

	m_filters_path = dir + "/filters-ai.txt";
	const bool filters_read = m_filters->load(m_filters_path);
	// Where the rules came from and how many, on request. A filter list that is
	// silently empty and one that is silently ignored look identical from a page,
	// and telling them apart by reasoning has already cost a round.
	if (qEnvironmentVariableIsSet("HYDRA_FILTER_DEBUG"))
		qWarning("filters: %s %s, %lld rule(s)", qPrintable(m_filters_path),
		          filters_read ? "read" : "MISSING",
		          static_cast<long long>(m_filters->rules().size()));
	// And hand them to the interceptor, which is the only thing that can act on
	// them. Loaded first so a rule accepted in an earlier session is in force
	// before the first request of this one.
	if (m_filter)
		m_filter->set_filter_list(m_filters);

	// Consent-banner rules, beside the rest and in the same spirit: data, not
	// code. The built-in set is always present; the file adds to it. This is
	// the unit a future exchange between users would move, which is why it is a
	// file from the start rather than something to be extracted from the binary
	// later (sec 7.1, `cookie_notices`).
	m_site_rules_path = dir + "/site-rules.ini";
	site_rules cr;
	if (!cr.load(m_site_rules_path))
		cr = site_rules::defaults();
	m_consent->set_rules(cr);
	m_antiadblock->set_rules(cr);

	const bool ok = m_model->load(path);
	// After the nodes exist and before anything draws: the tree shows a count
	// from this, so restoring it later would flash a row that claimed no past
	// and then quietly gained one.
	restore_histories();
	restore_view_state();
	return ok;
}

// **Both of these, because `saveGeometry` records size and position.** Keeping
// only the resize would remember a window that had been dragged to another
// corner as still being in the old one, which looks less like a missing feature
// than like the restore being broken.
void main_window::moveEvent(QMoveEvent *event) {
	QWidget::moveEvent(event);
	save_view_soon();
}

void main_window::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	save_view_soon();
	update_layout_mode();
	if (m_drawer_mode && m_sidebar) {
		// Keep the drawer the right size and where it belongs, whichever state
		// it is in -- a rotation while it is open must not leave it half off.
		const int w = qMin(int(width() * 0.82), 420);
		const int top = m_splitter ? m_splitter->y() : 0;
		m_sidebar->resize(w, height() - top - (m_status ? m_status->height() : 0));
		m_sidebar->move(m_drawer_open ? 0 : -w, top);
	}
}

void main_window::update_layout_mode() {
	const bool narrow = width() < k_drawer_threshold;
	if (narrow == m_drawer_mode || !m_sidebar || !m_splitter)
		return;
	m_drawer_mode = narrow;
	m_drawer_action->setVisible(narrow);

	// **The empty page told people to use something that was not on screen.**
	// At this width the tree is an overlay behind the drawer button, so "select
	// a tab from the tree" names a thing the window is not showing -- and the
	// button that reveals it had just appeared, unexplained, in a toolbar the
	// person had been using without it.
	if (m_placeholder)
		m_placeholder->setText(narrow
		    ? "No tab open.\n\nThe list of tabs is behind the button left of "
		       "the address bar."
		    : "Select a tab from the tree");

	if (narrow) {
		// Out of the splitter and on top of the window. The stack then takes
		// the whole width, which is the entire point: a page on a phone gets
		// the screen rather than a third of it.
		m_sidebar->setParent(this);
		m_sidebar->setAutoFillBackground(true);
		m_sidebar->raise();
		set_drawer_open(false, false);
		m_sidebar->show();
	} else {
		// Back where it came from, at the position it had.
		m_splitter->insertWidget(0, m_sidebar);
		m_sidebar->move(0, 0);
		m_splitter->setSizes({280, qMax(400, width() - 280)});
		m_drawer_open = false;
	}
}

bool main_window::eventFilter(QObject *watched, QEvent *event) {
#ifdef Q_OS_ANDROID
	// **Press only.** Consuming the release as well is not needed and would be
	// one more thing to get wrong; Qt raises the activity's back handling from
	// the press, so refusing that is enough to keep the browser open.
	if (event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Back && handle_back())
			return true;
	}

	// **A tap outside a popup closes it**, which Qt does for itself on a desktop
	// and does not do here.
	//
	// A `Qt::Popup` grabs the mouse and dismisses itself on a press outside its
	// own rectangle -- that is how every combo-box drop-down and every menu is
	// closed with a click elsewhere. On Android that grab does not produce the
	// press this needs, so drop-downs stayed open until something else was
	// tapped and the shield had no way out at all short of the Back button.
	// Reported from the phone as exactly that, twice: "drop down lists don't
	// close when tapping outside them" and "no button to close shield menu".
	//
	// `activePopupWidget` is the innermost one, so a drop-down inside the shield
	// closes first and the shield closes on the next tap -- the order somebody
	// expects, rather than both vanishing together.
	if (event->type() == QEvent::MouseButtonPress) {
		if (QWidget *popup = QApplication::activePopupWidget()) {
			auto *mouse = static_cast<QMouseEvent *>(event);
			if (!popup->geometry().contains(mouse->globalPosition().toPoint())) {
				popup->close();
				return true;
			}
		}
	}

	// **Tapping beside the drawer closes it**, which is what every drawer does
	// and what this one did not. A `QMenu` gets this from Qt for nothing --
	// `Qt::Popup` grabs the mouse and closes itself on a press outside -- but
	// the tab drawer is an ordinary widget slid into place, so nothing was
	// dismissing it and the only way out was the button it came from.
	//
	// The press is consumed, so the first tap closes the drawer and does not
	// also press whatever was under it. That is the same bargain a scrim makes
	// everywhere else: while the drawer is open the rest of the window is a way
	// of closing it rather than a set of controls.
	if (event->type() == QEvent::MouseButtonPress && m_drawer_open && m_sidebar) {
		auto *mouse = static_cast<QMouseEvent *>(event);
		const QPoint global = mouse->globalPosition().toPoint();
		QWidget *hit = QApplication::widgetAt(global);
		const bool in_drawer = hit && (hit == m_sidebar || m_sidebar->isAncestorOf(hit));
		// Only presses that land on this window: a press in a dialog or a menu
		// over the top is that widget's business, not the drawer's.
		const bool ours = hit && (hit == this || isAncestorOf(hit));
		if (ours && !in_drawer) {
			set_drawer_open(false);
			return true;
		}
	}
#endif
	return QWidget::eventFilter(watched, event);
}

bool main_window::handle_back() {
	// **A popup first, because it is the thing in front of everything else.**
	// `activePopupWidget` is any `Qt::Popup` -- every menu, every combo-box
	// drop-down. Closing it is what Back means while one is open, and it was
	// closing the browser instead.
	if (QWidget *popup = QApplication::activePopupWidget()) {
		popup->close();
		return true;
	}
	// Then a dialog. Rejected rather than accepted: Back is a way out, not a
	// way of agreeing to something, and every dialog in this project treats a
	// dismissal as the safe answer.
	if (QWidget *modal = QApplication::activeModalWidget()) {
		if (auto *dlg = qobject_cast<QDialog *>(modal))
			dlg->reject();
		else
			modal->close();
		return true;
	}
	// The tab drawer, which is over the page in the same sense.
	if (m_drawer_open) {
		set_drawer_open(false);
		return true;
	}
	// Then the page's own history, which is what Back means to somebody
	// browsing and is the reason the button exists.
	if (web_view_backend *v = current_view()) {
		if (v->can_go_back()) {
			go_back();
			return true;
		}
	}
	// Nothing left to undo. Not consumed, so Android does what it does with an
	// unhandled Back -- which at the root of a browser is the right thing.
	return false;
}

void main_window::set_drawer_open(bool open, bool animate) {
	if (!m_drawer_mode || !m_sidebar)
		return;
	m_drawer_open = open;

	// **Tell the page it is being covered.** On a backend Qt draws, this is a
	// no-op and the stacking below is the whole story. On Android the page is
	// a native View in the Activity, composited above everything Qt paints, so
	// the drawer slides out underneath it and cannot be seen -- correctly
	// positioned, correctly sized, `isVisible()` true, and behind the web
	// view. Measured on an SM-N960F: with no page open the tree appeared, with
	// one open it did not, and the log said x=0 337x703 visible=1 both times.
	//
	// The page comes back only once the drawer has finished sliding away, or
	// it reappears over a drawer still in motion and the animation reads as
	// the tree vanishing rather than closing.
	if (web_view_backend *v = current_view()) {
		if (open)
			v->set_obscured(true);
		else if (!animate)
			v->set_obscured(false);
	}

	const int w = m_sidebar->width();
	const int to = open ? 0 : -w;
	if (!animate) {
		m_sidebar->move(to, m_sidebar->y());
		return;
	}
	if (!m_drawer_anim)
		m_drawer_anim = new QPropertyAnimation(m_sidebar, "pos", this);
	m_drawer_anim->stop();
	m_drawer_anim->setDuration(180);
	m_drawer_anim->setEasingCurve(QEasingCurve::OutCubic);
	m_drawer_anim->setStartValue(m_sidebar->pos());
	m_drawer_anim->setEndValue(QPoint(to, m_sidebar->y()));
	// Restore the page when the close animation ends. `SingleShotConnection`
	// because a new connection is made on every toggle and they would
	// otherwise accumulate, each one firing on somebody else's animation.
	if (!open) {
		connect(m_drawer_anim, &QPropertyAnimation::finished, this, [this] {
			web_view_backend *v = m_drawer_open ? nullptr : current_view();
			if (v)
				v->set_obscured(false);
		}, Qt::SingleShotConnection);
	}
	m_drawer_anim->start();
	if (open)
		m_sidebar->raise();
}

web_view_backend *main_window::current_view() const {
	// Linear over at most k_max_live_views entries, which beats keeping a
	// second piece of state in sync with the stack.
	QWidget *w = m_stack->currentWidget();
	for (web_view_backend *v : m_views_by_id)
		if (v->widget() == w)
			return v;
	return nullptr;
}

// **A folder has no page; an empty tab does.** This used to refuse any node
// with an empty url, which quietly took `new_tab` with it: `add_tab` makes a
// row with no address on purpose -- that is what a new tab *is* -- so opening
// one returned here having done nothing.
//
// Nothing said so. The row appeared in the tree, and the stack kept showing the
// previous tab, so `current_view()` -- which reads the stack rather than the
// tree -- went on answering with the tab before. `new_tab` then focuses the
// address bar, inviting an address, and `navigate_to_address` loaded it into
// whatever was showing. That is one defect wearing both of the reported
// symptoms: the new row keeps the title `add_tab` gave it because no page ever
// loads into it, and the address typed for it opens somewhere else.
//
// So the guard is about being a *folder*, which is the thing that genuinely has
// no page, and an empty tab opens `about:blank` -- already this tree's spelling
// for "no address yet", the properties dialog having used it as the address
// placeholder since before this.
void main_window::open_node(node *n, bool load_now) {
	if (!n || n->is_folder())
		return;

	n->last_seen = QDateTime::currentDateTime();

	web_view_backend *view = m_views_by_id.value(n->id, nullptr);
	if (!view) {
		view = m_factory->create_view(this);
		m_views_by_id.insert(n->id, view);
		m_stack->addWidget(view->widget());

		apply_policy(view, QUrl::fromUserInput(n->url).host());

		// Autofill plumbing (architecture doc sec 13.2): the content script runs
		// in an isolated world and talks to a bridge object the shell owns.
		// The page never sets its own origin -- the shell does, on navigation.
		view->set_script_bridge(m_autofill, "hydraAutofill");
		view->inject_script("hydra-autofill",
		                     QString::fromUtf8(autofill_script::source()));
		// The element picker rides the same seam -- that is why it waited for
		// step 7 rather than shipping with the rest of sec 12.
		view->set_script_bridge(m_picker, "hydraPicker");
		view->inject_script("hydra-picker",
		                     QString::fromUtf8(picker_script::source()));
		// sec 11.6, in two halves. The relay is privileged and stays isolated with
		// the others; the hook is the only thing that goes into the page's own
		// world, and it carries nothing.
		// The consent banner, answered rather than merely hidden (sec 7.1's
		// cookie_notices). Same isolated world as the others: it clicks the
		// page's own buttons, so the page must not be able to rewrite it.
		view->set_script_bridge(m_consent, consent_blocker::bridge_name());
		// On subframes too, which is the whole difference between answering a
		// CMP and looking at one: vendors ship them as iframes.
		view->inject_script("hydra-consent", consent_blocker::script_source(), true);
		view->set_script_bridge(m_cosmetic, cosmetic_filters::bridge_name());
		view->inject_script("hydra-cosmetic", cosmetic_filters::script_source());
		view->set_script_bridge(m_mse, mse_tap::bridge_name());
		view->inject_script("hydra-mse-relay", mse_tap::relay_source());
		view->inject_main_world_script("hydra-mse-hook", mse_tap::hook_source());

#ifdef Q_OS_ANDROID
		// **Android only, because only Android has something to hide here.** The
		// desktop's brand list says `Chromium`, which is true and is what a site
		// expects from a Chromium-based browser; the WebView's says
		// `Android WebView`, which is read as "not a browser" -- measured on the
		// handset, where Teams refused a corrected user-agent string of
		// Chrome/140 while the brands still named the WebView.
		//
		// Main world rather than isolated, necessarily: the point is to replace
		// an object the page itself will read.
		view->inject_main_world_script("hydra-client-hints",
		                                user_agent::client_hints_shim());
#endif

		// The page's own name for itself, which the tree had no way to hear.
		// `set_page_title` refuses it on a tab somebody has named, so this can
		// be connected unconditionally.
		connect(view, &web_view_backend::title_changed, this,
		        [this, view](const QString &t) {
			if (view == current_view())
				update_window_title();
			const QString id = m_views_by_id.key(view);
			if (node *n = id.isEmpty() ? nullptr : m_model->node_by_id(id))
				if (m_model->set_page_title(n, t))
					save_tree_soon();
		});
		connect(view, &web_view_backend::url_changed, this, [this, view](const QUrl &u) {
			apply_policy(view, u.host());
			if (view == current_view()) {
				sync_page_context();
				update_navigation();
				update_address(u.toString());
			}
		});
		connect(view, &web_view_backend::history_changed, this, [this, view] {
			if (view == current_view())
				update_navigation();
		});
		connect(view, &web_view_backend::certificate_rejected, this,
		         [this, view](const QUrl &u, const QString &why) {
			if (view == current_view())
				report_certificate_rejected(u, why);
		});
		connect(view, &web_view_backend::new_window_requested, this,
		         [this, view](const QUrl &u, bool user, web_view_backend **adopt) {
			if (view == current_view())
				open_new_window(u, user, adopt);
		});
		// `window.close()`. Queued rather than acted on inside the signal: the
		// view is about to be destroyed with the node, and Chromium is still
		// on the stack that emitted this.
		connect(view, &web_view_backend::close_requested, this, [this, view] {
			const QString id = m_views_by_id.key(view);
			if (id.isEmpty())
				return;
			QMetaObject::invokeMethod(this, [this, id] {
				if (node *n = m_model->node_by_id(id))
					m_model->remove_node(n);
			}, Qt::QueuedConnection);
		});

		connect(view, &web_view_backend::render_process_gone, this,
		         [this, view] {
			if (view == current_view())
				report_render_crash(view->url().host());
		});
		connect(view, &web_view_backend::link_hovered, this,
		         [this, view](const QUrl &u) {
			if (view == current_view())
				show_link_target(u);
		});
		// Not guarded on being the current view, unlike its neighbours here. A
		// print survives navigating away or switching tabs, and the outcome is
		// the one thing about printing the window cannot otherwise show -- a
		// job that failed after the user moved on would report nothing at all.
		connect(view, &web_view_backend::fullscreen_requested, this,
		         [this, view](bool on) { present_fullscreen(view, on); });
		connect(view, &web_view_backend::print_finished, this, [this](bool ok) {
			m_status->showMessage(ok ? "Printed." : "Nothing was printed.", 5000);
		});
		connect(view, &web_view_backend::find_result, this,
		         [this, view](int matches, int active) {
			if (view == current_view())
				m_find->set_result(matches, active);
		});
		connect(view, &web_view_backend::load_progress, this, [this, view](int p) {
			if (view == current_view())
				on_load_progress(p);
		});
		connect(view, &web_view_backend::load_finished, this, [this, view](bool ok) {
			if (view == current_view())
				on_load_finished(ok);
			// **Every view, not only the current one**, which is the whole
			// reason this is not inside the branch above. A background tab
			// finishing a load has moved its history exactly as much as the
			// one in front, and its past is worth the same. The chrome only
			// needs updating for the tab being looked at; the record does not
			// care which tab is visible.
			//
			// A failed load still counts: a navigation that ends in an error
			// page is a history entry, and Chromium records it as one.
			Q_UNUSED(ok)
			save_blobs_soon(m_views_by_id.key(view));
		});

		// Feature permissions (geo/cam/mic/notifications) answered from policy,
		// and from the person when policy says to ask. The backend maps its
		// engine's own feature enum onto policy::feature, so what arrives here
		// is already in this project's vocabulary.
		//
		// **This was a one-line policy lookup returning a bool**, which is why
		// `ask` could not exist: there was nowhere for "wait, I am going to put
		// a dialog on the screen" to go. A blocked camera reached the page as a
		// `NotAllowedError` and reached the person as nothing, so a video call
		// that Hydra had deliberately stopped was indistinguishable from one
		// that was simply broken -- which cost a real diagnosis here.
		policy_engine *pe = m_policy;
		view->set_permission_decider(
		  [this, pe](const QUrl &origin, policy::feature f,
		              web_view_backend::permission_answer answer) {
			const QString host = origin.host();
			const policy::setting s = pe->effective_setting(f, host);

			// **Recorded where a person can see it**, which is the whole
			// difference from the debug switch this used to have. A capability
			// answered invisibly is the failure this browser exists to make
			// visible, and the evidence belongs beside the requests that got
			// through rather than in a log only a developer with a cable can
			// read.
			//
			// Noted before the prompt rather than after, so a question that is
			// still on screen is already in the record: somebody who gives up
			// on a dialog and files a report instead has told us the more
			// interesting thing.
			if (m_signals)
				m_signals->note_capability(host, policy::feature_label(f),
				                            origin.toString(),
				                            policy::setting_word(s));

			if (s != policy::setting::ask) {
				// The ordinary case, and it answers without touching the
				// screen. `allow` or anything else -- `block`, and `unset`,
				// which the engine resolves to block rather than leaving a
				// feature nobody configured wide open.
				answer(s == policy::setting::allow);
				return;
			}

			// Asked once per site per feature per run. The alternative is a
			// page that calls `getUserMedia` in a retry loop putting a dialog
			// on the screen as fast as one can be dismissed, which is not a
			// hypothetical failure mode -- it is how a page bullies someone
			// into pressing Allow.
			const QString key = host + QChar('\n') +
			                     QString::fromLatin1(policy::feature_name(f));
			const auto seen = m_session_permissions.constFind(key);
			if (seen != m_session_permissions.constEnd()) {
				answer(*seen);
				return;
			}

			permission_dialog dlg(host, f, origin.scheme() == "https", this);
			const bool grant = dlg.exec() == QDialog::Accepted && dlg.granted();

			if (dlg.remember()) {
				// Written to the site rules, where the shield can show it and
				// the person can take it back. Saved immediately rather than on
				// the debounce: an answer to a permission prompt is exactly the
				// kind of decision somebody would be furious to have to give
				// twice because the browser was killed in between.
				pe->set_setting(host, f, grant ? policy::setting::allow
				                                : policy::setting::block);
				pe->save(m_policy_path);
			} else {
				m_session_permissions.insert(key, grant);
			}
			answer(grant);
		});

		// What to share, once the shield has said the site may share at all.
		//
		// **Nothing is remembered here, and that is the design rather than an
		// omission.** The permission above offers to stick to a site; this
		// deliberately does not, because "may this site present" and "send
		// *that* window, now" are different decisions and only the first is
		// about the site. A meeting allowed to present last week has not been
		// allowed to present whatever happens to be open today.
		view->set_capture_chooser(
		  [this](const QUrl &origin, QAbstractListModel *screens,
		          QAbstractListModel *windows,
		          web_view_backend::capture_answer answer) {
			screen_picker dlg(origin.host(), screens, windows, this);
			dlg.exec();
			// `chosen_row()` is the answer on every path -- it is cleared on
			// `rejected`, so Escape and the close button need no special case
			// here and there is no result code to remember to check.
			answer(dlg.is_screen(), dlg.chosen_row());
		});

		// What the shield *would* say, without asking anybody. Used to build the
		// Permissions API shim on Android, where a page that politely checks
		// before calling is otherwise told "no" whatever the truth is.
		view->set_capability_peek(
		  [pe](const QUrl &origin, policy::feature f) {
			return pe->effective_setting(f, origin.host());
		});

		// The rest of the story, from below the shield. The decider above
		// records what this browser decided; this records what happened next,
		// so a report can say "we allowed it and the system did too" -- which
		// is the sentence that moves a complaint about a page off this browser.
		view->set_capability_note(
		  [this](const QUrl &origin, policy::feature f, const QString &outcome) {
			if (m_signals)
				m_signals->note_capability(origin.host(),
				                            policy::feature_label(f),
				                            origin.toString(), outcome);
		});

		// **Asked, rather than declined on the person's behalf.** Nothing
		// answered this, so a site wanting HTTP authentication simply failed
		// to load and said nothing about why. The dialog runs inside the
		// callback because that is when Qt can still use the answer.
		view->set_authenticator([this](const QUrl &url, const QString &realm,
		                                QString *user, QString *password) {
			auth_dialog dlg(url.host(), realm, url.scheme() == "https", this);
			if (dlg.exec() != QDialog::Accepted)
				return false;
			*user     = dlg.user();
			*password = dlg.password();
			return true;
		});

		// The proxy asking, in words that say it is the proxy. Same dialog,
		// different framing -- see auth_dialog::asker for why the distinction
		// is the whole point rather than a nicety.
		view->set_proxy_authenticator([this](const QString &proxy_host,
		                                      const QString &realm,
		                                      QString *user, QString *password) {
			// Encryption is unknown here and assumed absent, which is the safe
			// direction: proxy credentials on a plain CONNECT are readable, and
			// a warning shown when it was not needed costs a sentence, while
			// one withheld when it was needed costs the password.
			auth_dialog dlg(proxy_host, realm, false, this,
			                 auth_dialog::asker::proxy);
			if (dlg.exec() != QDialog::Accepted)
				return false;
			*user     = dlg.user();
			*password = dlg.password();
			return true;
		});

		// Which certificate identifies you to this site, if any.
		view->set_certificate_chooser(
		  [this](const QUrl &url,
		          const QList<web_view_backend::certificate_offer> &offered) {
			cert_dialog dlg(url.host(), offered, this);
			dlg.exec();
			return dlg.chosen();
		});

		// A locked tab keeps its page: the navigation opens a sub-tab below it
		// instead (sec 5.5). Answered here rather than in the backend because the
		// lock is a property of the *node*, and the seam deliberately knows
		// nothing about the tree.
		view->set_navigation_decider([this, view](const QUrl &u, bool main_frame,
		                                           bool user_initiated) {
			return allow_navigation(view, u, main_frame, user_initiated);
		});


		if (!load_now) {
			// Left empty on purpose: the caller is adopting an engine window
			// request, which navigates the page itself.
		} else if (n->type == node_type::suspended_tab && m_state &&
		            m_state->has_state(n->id)) {
			// Restore the session blob this node was suspended into.
			view->restore_state(m_state->load(n->id));
			m_state->remove(n->id);
		} else {
			view->load(n->url.isEmpty()
			               ? QUrl(QStringLiteral("about:blank"))
			               : QUrl::fromUserInput(n->url));
		}
	}

	n->type = node_type::open_tab;
	m_model->refresh_node(n);
	apply_zoom(view, n->id);
	m_stack->setCurrentWidget(view->widget());
	// The tree follows the page. Without this the highlight stayed on whatever
	// was last clicked, so the row that was showing and the row that looked
	// selected were different rows -- and `selected_parent()` reads the second
	// one to decide where the next new tab is filed.
	if (m_tree)
		m_tree->show_node(n);
	// Everything the chrome asserts belongs to the page in front of you: the
	// bar would otherwise report the last tab's load against this one, the
	// button would offer to stop a load that is not this tab's, and the find
	// count would claim matches from a page this is not.
	sync_page_context();
	update_address(view->url().toString());
	page_changed();
	touch_lru(n->id);
	enforce_live_cap(n->id);
	mark_dirty();
	update_status();
}


// Which site the page-scoped bridges are answering about.
//
// This used to happen in one place -- the current view's `url_changed` -- and
// that was wrong twice over.
//
// **Switching tabs did not update it.** Activating an already-loaded tab
// navigates nothing, so no signal arrived and the consent blocker went on
// answering `active_now()` and `rules_json()` for the site of the tab before it.
// A bridge deliberately built so the page cannot name its own host is not much
// use if the shell then names the wrong one.
//
// **And the ordering only worked by accident.** `activate_node()` calls
// `view->load()` before `setCurrentWidget()`. Qt WebEngine emits `urlChanged`
// asynchronously, so the signal landed after the switch and the
// `view == current_view()` test passed. The Android backend emits it from inside
// `load()`, synchronously, so it arrived while the previous view was still
// current, the test failed, and the host was never set at all -- cosmetic
// filtering silently did nothing there while every unit test passed. Reading it
// from whatever is current, after the switch, does not depend on which way a
// backend emits.
// **A button that does nothing is the worst kind of button**, and Back, Forward
// and Reload were all three of them on an empty tab: always enabled, silently
// no-ops. The same argument that put a status message behind the Media button
// applies here, except that a navigation button has a better answer than a
// message -- it can simply look unavailable, which is what every other browser
// does and what somebody is already reading the toolbar for.
//
// Asked of the view each time rather than cached. The alternative is mirroring
// two booleans that Chromium changes underneath us, and history is exactly the
// kind of state that moves without asking (redirects, in-page navigations,
// restored sessions).
void main_window::update_navigation() {
	// Both, and the stack is the one that matters: this is called from the
	// toolbar builder to set the initial state, which runs *before* the stacked
	// widget exists. Guarding only on the actions segfaulted every window.
	if (!m_back_action || !m_stack)
		return;
	web_view_backend *v = current_view();

	// A locked tab does not walk out of its page backwards either (sec 5.5).
	// Greying the buttons is the honest way to say so: the alternative is a
	// button that looks available and refuses, which is the shape this window
	// spent a whole pass removing. Reload stays on -- reloading a pinned page
	// is still the same page.
	bool pinned = false;
	if (v) {
		const QString id = m_views_by_id.key(v);
		if (node *n = id.isEmpty() ? nullptr : m_model->node_by_id(id))
			pinned = n->locked;
	}

	// Printing is not navigation, so a locked tab prints like any other: the
	// lock pins where the tab may go, not what may be done with the page it is
	// already showing.
	if (m_print_action)
		m_print_action->setEnabled(v && v->can_print());
	if (m_source_action)
		m_source_action->setEnabled(v && has_viewable_source(v->url()));

	// **Its checked state comes from the view, not from the last click.** The
	// toggle is per tab, so switching tabs has to show that tab's answer --
	// otherwise it reports whatever the previously-focused tab was set to,
	// which is the sort of control that teaches people not to trust it.
	//
	// Disabled where the backend does not implement it. On the desktop the base
	// class answers false and ignores the setter, and a menu entry that
	// silently does nothing is worse than one that is greyed.
	if (m_desktop_site_action) {
		const bool live = v != nullptr;
#ifdef Q_OS_ANDROID
		m_desktop_site_action->setEnabled(live);
#else
		m_desktop_site_action->setEnabled(false);
#endif
		QSignalBlocker block(m_desktop_site_action);
		m_desktop_site_action->setChecked(live && v->desktop_site());
	}

	m_back_action->setEnabled(v && !pinned && v->can_go_back());
	m_fwd_action->setEnabled(v && !pinned && v->can_go_forward());
	m_reload_action->setEnabled(v != nullptr);
}

// **A window called "Hydra" tells the task switcher nothing.** Every window
// carried the same name whatever it was showing, so two of them side by side
// were indistinguishable, and a tab opened five minutes ago could not be found
// again from a window list. Every browser puts the page in the title for this
// reason.
//
// The page's own title where it has one, the host where it does not -- a
// document with no <title> is usually one where the address is the only thing
// that identifies it. Truncated, because a title is a label rather than a
// document: some pages carry a paragraph in theirs, and a window manager given
// one either elides it in the middle or lets it push everything else out of
// the switcher.
// **Nothing to search is a message, not a silent no-op.** The bar would
// otherwise open over an empty window and report no matches, which reads as a
// broken search rather than as an absent page.
namespace {

// The ladder, in the order a person steps through it. Chromium's own set, so
// that a page zoomed here looks like the same page zoomed anywhere else.
constexpr double k_zoom_steps[] = {
	  0.25, 0.33, 0.50, 0.67, 0.75, 0.90, 1.00,
	  1.10, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00, 4.00, 5.00,
};
constexpr int k_zoom_count = int(sizeof(k_zoom_steps) / sizeof(k_zoom_steps[0]));

// The nearest rung to where the page actually is. Asked of the view rather
// than remembered, because something else may have set it -- kiosk mode does,
// and a remembered index would then step from a level that is no longer true.
int nearest_step(double factor) {
	int best = 0;
	for (int i = 1; i < k_zoom_count; ++i)
		if (qAbs(k_zoom_steps[i] - factor) < qAbs(k_zoom_steps[best] - factor))
			best = i;
	return best;
}

}  // namespace

// Zoom is **per tab and remembered**, which is the behaviour that makes it
// worth having: one site wants 125% permanently and the rest do not, and a
// zoom that resets on every navigation has to be redone constantly.
void main_window::step_zoom(int direction) {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first \u2014 there is nothing to "
		                       "zoom.", 5000);
		return;
	}
	double factor = 1.0;
	if (direction != 0) {
		int at = nearest_step(v->zoom_factor()) + direction;
		factor = k_zoom_steps[qBound(0, at, k_zoom_count - 1)];
	}
	v->set_zoom_factor(factor);

	const QString id = m_views_by_id.key(v);
	if (!id.isEmpty()) {
		// 100% is the absence of a setting, not a setting: keeping it would
		// grow an entry for every tab ever looked at.
		if (qFuzzyCompare(factor, 1.0))
			m_zoom.remove(id);
		else
			m_zoom.insert(id, factor);
	}
	m_status->showMessage(QString("Zoom %1%").arg(qRound(factor * 100)), 3000);
}

void main_window::apply_zoom(web_view_backend *view, const QString &node_id) {
	if (view)
		view->set_zoom_factor(m_zoom.value(node_id, 1.0));
}

void main_window::open_find() {
	if (!current_view()) {
		m_status->showMessage("Open a page first \u2014 there is nothing to "
		                       "search yet.", 5000);
		return;
	}
	m_find->begin();
}

// **The Reload button becomes Stop while a page is arriving**, which is what
// every browser does with that slot and the reason it works: the two are never
// wanted at the same moment, and a page that will not finish loading is exactly
// when somebody reaches for the toolbar.
//
// One action rather than two, so the Go menu gets it as well without a second
// entry that would be wrong half the time. Only offered while a load is
// actually running, because `stop()` does nothing on a backend that cannot
// abandon one, and a button that does nothing is the worst kind.
// **The status bar, not a tooltip.** A tooltip follows the pointer and covers
// the thing being pointed at; the status bar is where a browser has put this
// since before tooltips existed, and it is out of the way of the page.
//
// Elided in the *middle*, which is the whole point: a long url that keeps its
// scheme and host but loses its query still answers "where does this go", while
// one truncated from the right loses the host -- the only part that matters for
// deciding whether to click.
// **A tab whose renderer dies goes blank and used to say nothing.** Only kiosk
// mode listened for this, because kiosk needs to self-heal on an unattended
// screen; interactively the page simply vanished, with no explanation and no
// hint that anything could be done about it.
//
// Not reloaded automatically, which is the difference from kiosk. A page that
// crashes on load crashes again on reload, and an automatic retry turns one
// blank page into a loop that also eats the machine. Reload is offered instead,
// and it genuinely recovers: Qt starts a new render process for it.
//
// The message has no timeout. It describes a state the window is still in, and
// a state that expires after five seconds leaves somebody looking at a blank
// page wondering what they missed.
// **Another window is a child tab**, which is this browser's whole answer to
// the question: the tree already shows what came from what, and a link that
// spawns a window is exactly that relationship. A separate top-level window
// would throw the relationship away and leave two trees to reconcile.
//
// **A click is not a popup.** Chromium's own rule, and the right one: the popup
// setting exists to stop pages opening windows nobody asked for, not to break
// target="_blank" links. So a user-initiated request opens and is switched to,
// while a script-initiated one is checked against the policy for the page that
// asked -- and refused *out loud*, because a blocked popup that says nothing is
// indistinguishable from the broken silence this whole change is fixing.
// **What failed, not just that it failed.** Before the load-failure message
// existed this was silent; with it, a certificate problem read as "could not be
// loaded", which is what a site being down reads as too. Those want different
// responses from a person -- try later, versus do not type anything into this
// -- so they must not share a sentence.
//
// No timeout, for the reason the crash notice has none: it describes why the
// window is showing what it is showing, and that stays true until something
// else loads.
void main_window::report_certificate_rejected(const QUrl &url,
                                               const QString &reason) {
	const QString host = url.host();
	m_cert_url = url;
	m_status->showMessage(
	  QString("%1 was not loaded: its certificate could not be trusted (%2).")
	      .arg(host.isEmpty() ? QStringLiteral("That site") : host,
	            reason.isEmpty() ? QStringLiteral("no reason given") : reason));
}

// Whether `view` may go to `url`, and what happens instead when it may not.
//
// **Only a locked tab ever says no**, so the ordinary path through here is one
// hash lookup and a false. The engine calls this while it is deciding, so it
// has to be cheap and it has to answer synchronously.
//
// What counts as leaving the page, and why each case is what it is:
//
// - **A subframe** navigating is not the page going anywhere. An ad slot or an
//   embedded player changing its own url would otherwise spawn a sub-tab for
//   something the user never clicked.
// - **The same document** -- a fragment jump, or the same address again -- is
//   where the tab already is. Pinning would make an in-page anchor unusable and
//   a reload spawn a duplicate.
// - **A person** clicking, typing or submitting gets a sub-tab, which is the
//   feature.
// - **A page** redirecting itself or navigating by script gets refused and
//   said so. It is not the user browsing, and a page that could spawn tabs by
//   scripting its own location would turn one pinned tab into a stream of
//   them. This is the same line the popup rule already draws.
//
// Back and forward never arrive here for a locked tab: `update_navigation`
// greys them, because a pinned tab that could still be walked backwards out of
// its page is not pinned.
bool main_window::allow_navigation(web_view_backend *view, const QUrl &url,
                                    bool in_main_frame, bool user_initiated) {
	if (!view || !in_main_frame || !url.isValid())
		return true;

	const QString id = m_views_by_id.key(view);
	node *n = id.isEmpty() ? nullptr : m_model->node_by_id(id);
	if (!n || !n->locked)
		return true;

	// **Compared against the node's url, not the view's.** The node's url is
	// the pin -- written when the lock was applied -- and the view's is
	// unreliable exactly when it matters: opening a locked tab loads its page
	// through this same check, and at that moment the view is still empty, so
	// comparing against it would make a locked tab spawn a sub-tab of itself
	// instead of loading.
	//
	// The fragment is dropped from both: an anchor within the pinned document
	// is not leaving it, and pinning those would make an in-page link unusable.
	QUrl pinned(n->url);
	QUrl there = url;
	pinned.setFragment(QString());
	there.setFragment(QString());
	if (pinned == there)
		return true;

	if (!user_initiated) {
		m_status->showMessage(
		  QString("%1 is locked, so the page could not send it to %2.")
		      .arg(n->title.isEmpty() ? QStringLiteral("This tab") : n->title,
		            url.host().isEmpty() ? url.toString() : url.host()), 6000);
		return false;
	}

	// The sub-tab, and then the browsing continues in it -- but **not from
	// inside this call**. This runs while the engine is deciding a navigation,
	// and building a second view underneath it there means creating a page,
	// wiring a profile and starting a load re-entrantly. The answer the engine
	// needs is `false`, now; the tab can be a moment later.
	//
	// The parent is carried by **id rather than by pointer** for the reason
	// everything else in this window is: a node can be deleted between the
	// queueing and the running, and an id that no longer resolves is a lookup
	// that fails instead of a pointer that crashes.
	const QString parent_id = n->id;
	const QUrl target = url;
	QMetaObject::invokeMethod(this, [this, parent_id, target] {
		if (node *p = m_model->node_by_id(parent_id))
			open_child_tab(p, target);
	}, Qt::QueuedConnection);

	m_status->showMessage(
	  QString("%1 is locked, so %2 opens below it.")
	      .arg(n->title.isEmpty() ? QStringLiteral("That tab") : n->title,
	            url.host().isEmpty() ? url.toString() : url.host()), 6000);
	return false;
}

// Pin or unpin a node, with the address it is pinned to.
//
// The shell does this rather than the tree because **a node's url does not
// follow the page**: only the title does, so a tab opened at one address and
// browsed to another still records the first, and the live view is the only
// thing that knows where it actually is. Locking means "keep this page", so
// the page showing is what gets written.
void main_window::toggle_lock(node *n) {
	if (!n)
		return;
	QString pin;
	if (web_view_backend *v = m_views_by_id.value(n->id, nullptr))
		if (!v->url().isEmpty())
			pin = v->url().toString();
	if (m_model->set_locked(n, !n->locked, pin))
		update_navigation();
}

// A child tab under `parent`, opened and switched to. The shared half of "a
// new tab came from this one", which a locked tab's navigation and a page's
// window request both mean.
node *main_window::open_child_tab(node *parent, const QUrl &url) {
	if (!parent || !url.isValid())
		return nullptr;
	node *made = m_model->add_tab(parent,
	                               url.host().isEmpty() ? url.toString() : url.host(),
	                               url.toString());
	if (!made)
		return nullptr;
	save_tree_soon();
	open_node(made);
	return made;
}

// **Built with `FullyEncoded`**, which is the whole of the difficulty. The
// address becomes the tail of another url, so a query string that survives
// `toString()` unencoded -- an `&`, a `#`, a space -- is re-parsed as part of
// the outer one and the source view opens somewhere else, or nowhere. The
// encoded form has no character that means anything to the outer parse.
//
// Routed through open_new_window() rather than add_tab() so the source lands
// as a child of the page it belongs to, and so a page whose popups are blocked
// cannot have that policy applied to a tab the user asked for by name: this is
// user-initiated by construction, since a menu item and a keystroke are the
// only two ways to reach it.
void main_window::view_page_source() {
	web_view_backend *v = current_view();
	if (!v)
		return;
	const QUrl page = v->url();
	if (!has_viewable_source(page)) {
		m_status->showMessage("This page has no source to show.", 5000);
		return;
	}
	open_new_window(QUrl("view-source:" + page.toString(QUrl::FullyEncoded)),
	                 true);
}

node *main_window::open_new_window(const QUrl &url, bool user_initiated,
                                    web_view_backend **adopt) {
	if (!url.isValid() || url.isEmpty())
		return nullptr;

	web_view_backend *from = current_view();
	const QString asker = from ? from->url().host() : QString();
	if (!user_initiated &&
	     !m_policy->is_allowed(policy::feature::popups, asker)) {
		m_status->showMessage(
		  QString("Blocked a window %1 tried to open. Allow popups for this "
		           "site to let it through.")
		      .arg(asker.isEmpty() ? QStringLiteral("this page") : asker), 8000);
		return nullptr;
	}

	// Under the tab that asked, where the tree shows the relationship; at the
	// root if the asking tab cannot be found, which beats dropping it.
	//
	// **This comment described the design and the code did something else** for
	// as long as a tab could hold no children: the node went to `n->parent`, so
	// a window opened from a link became a *sibling* of the page that opened it
	// and the relationship the tree was supposed to show was not recorded
	// anywhere. Sub-tabs (sec 5.5) lifted that restriction, and this is the line
	// that had been waiting for it.
	node *parent = nullptr;
	if (from) {
		const QString id = m_views_by_id.key(from);
		if (node *n = id.isEmpty() ? nullptr : m_model->node_by_id(id))
			parent = n;
	}
	node *made = m_model->add_tab(parent, url.host().isEmpty() ? url.toString()
	                                                            : url.host(),
	                               url.toString());
	if (!made)
		return nullptr;
	save_tree_soon();

	if (user_initiated) {
		// Without loading when the request is being adopted: `openIn` does the
		// navigation, and doing it here as well would fetch a one-time url
		// twice -- which for an OAuth popup means spending the state parameter
		// before the real navigation uses it.
		open_node(made, /*load_now=*/adopt == nullptr);
		if (adopt)
			*adopt = m_views_by_id.value(made->id, nullptr);
	} else {
		// Allowed, but not given the foreground: a page that opens a window
		// while you are reading is not entitled to take the page away from you.
		m_status->showMessage(QString("%1 opened a window; it is a new tab.")
		                          .arg(asker.isEmpty() ? QStringLiteral("This page")
		                                                : asker), 6000);
	}
	return made;
}

void main_window::report_render_crash(const QString &host) {
	set_loading(false);
	if (m_progress)
		m_progress->hide();
	m_status->showMessage(host.isEmpty()
	                          ? QStringLiteral("This page stopped responding. "
	                                            "Reload to try again.")
	                          : QString("%1 stopped responding. Reload to try "
	                                     "again.").arg(host));
}

void main_window::show_link_target(const QUrl &url) {
	m_link_shown = !url.isEmpty();
	if (url.isEmpty()) {
		// Not a timed message: the pointer left the link, and a target left on
		// screen after that is a claim about where the pointer is now.
		m_status->clearMessage();
		return;
	}
	const QString text = url.toString();
	const int room = 110;
	m_status->showMessage(text.size() <= room
	                          ? text
	                          : text.left(room / 2) + QChar(0x2026) +
	                                text.right(room / 2 - 1));
}

// The page in front of you changed, whether by opening a tab, switching to
// another, or losing the one that was there. Everything the chrome asserts
// about a page is re-derived here rather than at each of the four callers.
void main_window::page_changed() {
	if (!m_stack)
		return;              // still being built; nothing to say yet
	set_loading(false);
	if (m_progress) {
		m_progress->hide();
		m_progress->setValue(0);
	}
	if (m_find)
		m_find->clear_result();
	// Only what this window put there. Clearing unconditionally would wipe
	// whatever else the status bar was saying -- "Ready" at startup, or the
	// result of the action that caused this.
	if (m_link_shown) {
		m_status->clearMessage();
		m_link_shown = false;
	}
	update_navigation();
	update_window_title();
}

void main_window::set_loading(bool loading) {
	if (m_loading == loading || !m_reload_action)
		return;
	m_loading = loading;
	if (loading) {
		m_reload_action->setText("&Stop");
		m_reload_action->setToolTip("Stop loading");
		m_reload_action->setIcon(themed_icon({ "process-stop" }, style(),
		                                      QStyle::SP_BrowserStop));
	} else {
		m_reload_action->setText("&Reload");
		m_reload_action->setToolTip("Reload");
		m_reload_action->setIcon(themed_icon({ "view-refresh" }, style(),
		                                      QStyle::SP_BrowserReload));
	}
}

void main_window::on_load_progress(int percent) {
	// Something is loading, so whatever the status bar was saying about the
	// last page -- a crash notice, in particular -- has been acted on.
	if (!m_loading)
		m_status->clearMessage();
	set_loading(true);
	m_progress->setValue(percent);
	if (!m_progress->isVisible())
		m_progress->show();
}

// **A page that does not arrive has to say so.** Chromium draws its own error
// document for a network failure, but nothing was drawn for the cases it
// handles silently, and the bar would simply have sat at whatever it reached.
// The host rather than the whole url: what failed is a site, and a url long
// enough to be interesting is long enough to push the rest of the status bar
// off the end.
void main_window::on_load_finished(bool ok) {
	set_loading(false);
	m_progress->hide();
	m_progress->setValue(0);
	if (ok) {
		m_cert_url.clear();
		return;
	}
	// **The specific answer wins, for the page it was about.** A refused
	// certificate fails the load, so this arrives immediately afterwards, and
	// replacing "its certificate could not be trusted" with "could not be
	// loaded" would lose the only part that said what to do differently.
	// Matched on the host rather than remembered as a flag: an unrelated
	// failure later has a different host and speaks for itself.
	web_view_backend *failed = current_view();
	if (!m_cert_url.isEmpty() && failed &&
	     failed->url().host() == m_cert_url.host()) {
		m_cert_url.clear();
		return;
	}
	web_view_backend *v = current_view();
	const QString host = v ? v->url().host() : QString();
	m_status->showMessage(host.isEmpty()
	                          ? QStringLiteral("That page could not be loaded.")
	                          : QString("%1 could not be loaded.").arg(host),
	                       6000);
}

void main_window::update_window_title() {
	web_view_backend *v = current_view();
	QString page = v ? v->page_title().trimmed() : QString();
	if (page.isEmpty() && v)
		page = v->url().host();
	if (page.size() > 70)
		page = page.left(69) + QChar(0x2026);
	setWindowTitle(page.isEmpty() ? QStringLiteral("Hydra")
	                               : page + QStringLiteral(" \u2014 Hydra"));
}

void main_window::sync_page_context() {
	web_view_backend *v = current_view();
	if (!v)
		return;
	const QUrl u = v->url();
	if (m_consent)
		m_consent->set_page_host(u.host());
	if (m_cosmetic)
		m_cosmetic->set_page_host(u.host());
	// The key belongs to the page that raised it: a new document has not asked
	// for anything yet, and a tick left over from the last one would claim a
	// fill that did not happen here. The button stays where it is; only what
	// it says and whether it can be pressed go back to the start.
	reset_key_action();
	if (m_autofill) {
		m_autofill->set_page_origin(
		  u.adjusted(QUrl::RemovePath | QUrl::RemoveQuery |
		              QUrl::RemoveFragment).toString());
	}
}

void main_window::suspend_node(node *n) {
	if (!n)
		return;
	web_view_backend *view = m_views_by_id.value(n->id, nullptr);
	if (!view)
		return;

	// Never tear down the view a kiosk session is presenting. Without this the
	// live-view cap could suspend the tab that is currently fullscreen.
	if (m_kiosk && m_kiosk->active() && m_kiosk->view() == view)
		return;

	if (m_state)
		m_state->save(n->id, view->save_state());

	if (view == current_view())
		m_stack->setCurrentIndex(0);  // back to placeholder
	QWidget *w = view->widget();
	m_stack->removeWidget(w);
	m_views_by_id.remove(n->id);
	m_lru.removeAll(n->id);
	w->deleteLater();   // the backend is parented to its widget and goes too

	n->type = node_type::suspended_tab;
	m_model->refresh_node(n);
	mark_dirty();
	update_status();
	page_changed();
}

void main_window::touch_lru(const QString &id) {
	m_lru.removeAll(id);
	m_lru.prepend(id);
}

void main_window::enforce_live_cap(const QString &keep_id) {
	while (m_views_by_id.size() > k_max_live_views) {
		QString victim;
		for (auto it = m_lru.crbegin(); it != m_lru.crend(); ++it) {
			if (*it != keep_id && m_views_by_id.contains(*it)) {
				victim = *it;
				break;
			}
		}
		if (victim.isEmpty())
			break;
		if (node *n = m_model->node_by_id(victim)) {
			suspend_node(n);
			continue;
		}
		// A victim the tree no longer knows. This used to `break`, which meant
		// one unresolvable entry stopped the cap being enforced *at all* for the
		// rest of the session -- silently, and the symptom would have been
		// memory rather than anything pointing here. Drop the entry and carry
		// on; `forget_subtree` should have removed it already, so reaching this
		// is a belt-and-braces path rather than the expected one.
		m_lru.removeAll(victim);
		m_views_by_id.remove(victim);
	}
}

void main_window::on_tree_activated(const QModelIndex &proxy_index) {
	// Picking a tab is what the drawer was opened for, so it gets out of the
	// way. Leaving it up would cover the page the user just asked for.
	if (m_drawer_mode)
		set_drawer_open(false);
	if (!proxy_index.isValid())
		return;
	node *n = m_model->node_for_index(m_proxy->mapToSource(proxy_index));
	open_node(n);
}

// Where a new node goes: beside whatever is selected, or at the top level when
// nothing is. Dropping it at the root regardless would be simpler and wrong --
// a new tab made while working inside a folder belongs in that folder.
// Coalesced, because titles arrive in bursts. `titleChanged` fires several
// times during an ordinary page load -- the url, then the real title, sometimes
// a revision from script -- and writing the whole tree for each would be a lot
// of io for one settled answer.
//
// This is the debounce the shell already had for structural saves rather than a
// second one beside it. The first version of this function built its own timer
// and the compiler caught the duplicate member, which is the same "two records
// of one thing" the reorder flag and the live-view map both turned out to be.
// Hand an address to another application.
//
// **The one place where the two platforms mean genuinely different things**, so
// it says which rather than pretending they are the same. On Android this is an
// untyped `ACTION_VIEW`, and the apps that answer for a site are the ones that
// can do what a browser tab cannot: keep audio playing with the screen off.
// YouTube pauses itself when its page is hidden and Android suspends a process
// with no foreground service, so a tab is the wrong container for listening to
// something -- while VLC, NewPipe and YouTube itself run a media notification
// and are built for exactly that.
//
// On desktop it goes to the system's default handler, which for an http address
// is usually another browser. That is a smaller feature and an honest one; it
// is not pretending to be the Android behaviour.
void main_window::open_url_externally(const QUrl &url) {
	if (!url.isValid() || url.scheme().isEmpty() || url.scheme() == "about") {
		m_status->showMessage("There is no address to hand over.", 6000);
		return;
	}
#ifdef Q_OS_ANDROID
	QString error;
	if (!android_intents::open_externally(url, &error))
		m_status->showMessage(error, 8000);
	else
		m_status->showMessage("Handed to another app.", 4000);
#else
	// Not `QDesktopServices` blindly: it answers true for a great many things,
	// and a silent nothing is the failure this project keeps writing down.
	if (!QDesktopServices::openUrl(url))
		m_status->showMessage("Nothing on this system offered to open that "
		                       "address.", 8000);
	else
		m_status->showMessage("Opened in this system's default application.", 4000);
#endif
}

// Everything under `n`, because deleting a folder takes what is inside it and
// each of those may have a view and a state blob of its own.
void main_window::forget_subtree(node *n) {
	if (!n)
		return;
	for (node *c : n->children)
		forget_subtree(c);

	if (web_view_backend *view = m_views_by_id.value(n->id, nullptr)) {
		if (view == current_view())
			m_stack->setCurrentIndex(0);   // back to the placeholder
		QWidget *w = view->widget();
		m_stack->removeWidget(w);
		w->deleteLater();
	}
	m_views_by_id.remove(n->id);
	m_lru.removeAll(n->id);
	// The saved state goes with it. A blob for a node that no longer exists is
	// unreachable by anything except an id collision, which is the one way it
	// could ever be read again -- into the wrong tab.
	if (m_state)
		m_state->remove(n->id);
	update_status();
	page_changed();
}

// The key's resting state: on the toolbar, greyed, saying why.
//
// One function so that creation and every navigation cannot drift apart. They
// did not have to agree while the button was hidden -- absent is absent -- and
// the moment it became permanent, two places had to describe the same state in
// the same words.
void main_window::reset_key_action() {
	if (!m_key_action)
		return;
	m_key_action->setText("Key");
	// The platform's answer outranks the page's: where KeePassXC cannot be
	// reached at all, saying "no login form on this page" would send somebody
	// looking at the wrong thing.
	if (!keepass_bridge::supported()) {
		m_key_action->setEnabled(false);
		m_key_action->setToolTip(keepass_bridge::unavailable_reason());
		return;
	}
	m_key_action->setEnabled(false);
	m_key_action->setToolTip("No login form on this page. This fills saved "
	                          "logins from KeePassXC when there is one.");
}

void main_window::save_tree_soon() {
	if (!m_tree_path.isEmpty() && m_save_timer)
		m_save_timer->start();
}

// The same shape as `save_tree_soon`, for the file beside the tree. Every
// caller is a place the *view* changed and the tree did not -- a folder
// opened, the current row moved, the sort changed, the window resized -- none
// of which the structural signal can see, because none of them touches the
// model.
void main_window::save_view_soon() {
	if (!m_view_path.isEmpty() && m_view_timer)
		m_view_timer->start();
}

void main_window::save_blobs_soon(const QString &id) {
	if (id.isEmpty() || !m_state || !m_blob_timer)
		return;
	m_blobs_dirty.insert(id);
	m_blob_timer->start();
}

// Write the history of every tab that has navigated since this last ran.
//
// **Only those, which is the difference between this and `save_everything`.**
// That one serialises every live view because it is about to destroy them all;
// this runs while the browser is in use, and rewriting twenty unchanged blobs
// because one tab followed a link is work with nothing to show for it. The
// dirty set is what makes the cost proportional to what actually happened.
//
// And it does not suspend anything. `suspend_node` writes a blob too, but as
// the first step of tearing the view down; this is the same write with none of
// the rest of it, which is what a checkpoint has to be.
void main_window::flush_blobs() {
	if (!m_state)
		return;
	const QSet<QString> ids = m_blobs_dirty;
	m_blobs_dirty.clear();
	for (const QString &id : ids) {
		// A tab suspended between the navigation and this firing has already
		// had its blob written by `suspend_node`, and has no live view to ask.
		// Not an error, and the commonest way an id here goes stale.
		if (web_view_backend *view = m_views_by_id.value(id, nullptr))
			m_state->save(id, view->save_state());
	}
}

// The row itself, where `selected_parent` answers "where would a new child go".
// Both exist because the menu needs the first and creation needs the second, and
// one standing in for the other is how "New Folder" started making siblings of
// the thing that was clicked.
node *main_window::selected_node() const {
	const QModelIndexList sel = m_tree->selectionModel()
	  ? m_tree->selectionModel()->selectedIndexes() : QModelIndexList();
	for (const QModelIndex &i : sel) {
		if (i.column() != 0)
			continue;
		if (node *n = m_model->node_for_index(m_proxy->mapToSource(i)))
			return n;
	}
	return nullptr;
}

node *main_window::selected_parent() const {
	const QModelIndexList sel = m_tree->selectionModel()
	  ? m_tree->selectionModel()->selectedIndexes() : QModelIndexList();
	for (const QModelIndex &i : sel) {
		if (i.column() != 0)
			continue;
		if (node *n = m_model->node_for_index(m_proxy->mapToSource(i)))
			return n->is_folder() ? n : n->parent;
	}
	return nullptr;
}

void main_window::open_url(const QUrl &url) {
	if (!url.isValid() || url.isEmpty())
		return;
	// Under the root rather than the selection: nothing is selected when this
	// arrives from another application, and a link from outside is not a child
	// of whatever happened to be highlighted.
	node *t = m_model->add_tab(nullptr, QString(), url.toString());
	if (!t)
		return;
	m_tree->expandAll();
	open_node(t);
	update_address(url.toString());
}

void main_window::new_tab() {
	node *t = m_model->add_tab(selected_parent(), QString(), QString());
	if (!t)
		return;
	m_tree->expandAll();
	open_node(t);
	// The address bar, because an empty tab is a question about where to go.
	if (m_address) {
		m_address->setFocus();
		m_address->selectAll();
	}
}

void main_window::new_folder() {
	if (node *f = m_model->add_folder(selected_parent(), QString())) {
		m_tree->expandAll();
		// Named on the spot: a folder called "New folder" is one somebody has
		// to come back and rename, and they will not.
		const QModelIndex at = m_proxy->mapFromSource(m_model->index_for_node(f));
		if (at.isValid()) {
			m_tree->setCurrentIndex(at);
			m_tree->scrollTo(at);
		}
		bool ok = false;
		const QString name = QInputDialog::getText(
		    this, "New folder", "Name:", QLineEdit::Normal, f->title, &ok);
		if (ok && !name.trimmed().isEmpty())
			m_model->update_node(f, name.trimmed(), f->url, f->tags);
	}
}

void main_window::import_firefox_tabs() {
	const QString profile = session_import::firefox_profile();
	const QString path    = session_import::firefox_session_path(profile);
	if (path.isEmpty()) {
		m_status->showMessage(
		    profile.isEmpty()
		        ? "No Firefox profile found."
		        : "That Firefox profile has no session file to read.", 8000);
		return;
	}

	QString error;
	const QList<session_import::imported_tab> tabs =
	  session_import::firefox_tabs(path, &error);
	if (tabs.isEmpty()) {
		m_status->showMessage(error.isEmpty() ? "No open tabs found." : error, 8000);
		return;
	}

	show_mirror_tabs("firefox", "Firefox", tabs, /*from_poll=*/false);
	m_status->showMessage(
	    QString("Read %1 tabs from Firefox, as of %2. Drag one into your tree to "
	             "keep it.")
	        .arg(tabs.size())
	        .arg(QFileInfo(path).lastModified().toString("HH:mm")), 12000);
}

// The mirror folder, from either route. Shared so a poll and a menu click
// cannot drift into building the tree two different ways.
void main_window::import_chromium_tabs() {
	const QString profile = session_import::chromium_profile();
	const QString path    = session_import::chromium_session_path(profile);
	if (path.isEmpty()) {
		m_status->showMessage(profile.isEmpty()
		                          ? "No Chromium profile found."
		                          : "That Chromium profile has no session file.",
		                      8000);
		return;
	}
	QString error;
	const QList<session_import::imported_tab> tabs =
	  session_import::chromium_tabs(path, &error);
	if (tabs.isEmpty()) {
		m_status->showMessage(error.isEmpty() ? "No open tabs found." : error, 8000);
		return;
	}
	show_mirror_tabs("chromium", "Chromium", tabs, /*from_poll=*/false);
	m_status->showMessage(
	    QString("Replayed %1 tabs from Chromium, as of %2. Drag one into your "
	             "tree to keep it.")
	        .arg(tabs.size())
	        .arg(QFileInfo(path).lastModified().toString("HH:mm")), 12000);
}

void main_window::show_mirror_tabs(const QString &source, const QString &label,
                                    const QList<session_import::imported_tab> &tabs,
                                    bool from_poll) {
	QList<node *> nodes;
	for (const session_import::imported_tab &t : tabs) {
		node *n = new node;
		// Ids scoped to the mirror. A mirrored tab is never written to the tree
		// file and never gets a state blob, but it *is* in `m_id_index` while
		// it is on screen -- so an id that collided with a real tab's would make
		// `node_by_id` answer with somebody else's session, and that lookup is
		// what the lifecycle and the AI payload both use.
		n->id        = QString("%1-%2").arg(source).arg(nodes.size());
		n->type      = node_type::unopened_tab;
		n->title     = t.title;
		n->url       = t.url;
		n->created   = QDateTime::currentDateTime();
		n->last_seen = n->created;
		// The reason the mirror is worth more than a list of addresses: the
		// other browser's session file records where each tab had *been*, and
		// this is the only moment that record is available. It is dropped when
		// the mirror is replaced, and kept for good the moment somebody drags
		// the row into their own tree.
		n->history   = t.history;
		nodes << n;
	}
	m_model->replace_mirror(source,
	                         QString("%1 (%2 tabs)").arg(label).arg(nodes.size()),
	                         nodes);
	// Expanding the whole tree is right for a menu click and wrong for a
	// background refresh: it would fold the user's folders open again every
	// time Firefox opened a tab. The row signals already leave everything else
	// alone, so a poll touches nothing but the mirror.
	if (!from_poll)
		m_tree->expandAll();
	else
		m_status->showMessage(
		    QString("%1 now has %2 tabs open.").arg(label).arg(nodes.size()), 4000);
}


void main_window::on_sort_mode_changed(int combo_index) {
	using SM = tree_sort_proxy::sort_mode;
	static const SM modes[] = { SM::tree_order, SM::title_asc,
		                          SM::newest_created, SM::recently_seen };
	if (combo_index >= 0 && combo_index < 4)
		m_proxy->set_sort_mode(modes[combo_index]);
	m_tree->expandAll();
}

void main_window::on_search_changed(const QString &text) {
	m_proxy->set_search_text(text);
	if (!text.trimmed().isEmpty())
		m_tree->expandAll();
}

void main_window::navigate_to_address() {
	const QString text = m_address->text().trimmed();

	// A magnet link is not a page. Handing it to the engine would produce an
	// error page, so route it to the download manager instead -- which is what
	// "first class" means in practice: the same box, the same result as any
	// other download link (sec 11.4).
	const QUrl direct(text);
	if (m_downloads->source_for(direct) &&
	    direct.scheme().compare("magnet", Qt::CaseInsensitive) == 0) {
		start_download(direct);
		return;
	}

	// **Not everything typed here is an address**, and until this it was
	// treated as one: `QUrl::fromUserInput` turned terms into either an
	// invalid url, which loaded nothing and said nothing, or a guess like
	// `http://weather tomorrow`. `looks_like_address` decides, and it is a
	// separate unit because the interesting half is a privacy question rather
	// than a parsing one -- see its header for why a bare hostname must never
	// end up in a search box.
	const bool searching = !looks_like_address(text);
	const QUrl target = searching
	                      ? search_url(text, settings_store::search_engine())
	                      : QUrl::fromUserInput(text);
	if (searching && !target.isValid()) {
		m_status->showMessage("The search engine setting has no %1 in it, so "
		                       "there is nowhere to put the search terms.",
		                       8000);
		return;
	}

	if (web_view_backend *v = current_view()) {
		v->load(target);
		return;
	}

	// **With no tab open this used to do nothing at all.** The address bar is
	// enabled and inviting on an empty window -- the empty state next to it says
	// "Select a tab from the tree" -- so typing an address and pressing return is
	// an obvious thing to try, and it silently discarded what was typed. That is
	// the same defect as a navigation button that looks available and refuses,
	// which this window spent a pass removing; it survived because nothing had
	// looked at the window with no tab in it.
	//
	// Opening a tab is what every other browser does with a typed address, and
	// the tree is where this one keeps tabs, so it goes in at the root -- there
	// is no "current" folder to prefer when nothing is selected.
	if (text.isEmpty() || !target.isValid())
		return;
	if (open_child_tab(m_model->root(), target))
		m_status->showMessage(QString("Opened %1 in a new tab.")
		                          .arg(target.host().isEmpty() ? target.toString()
		                                                        : target.host()), 6000);
}

void main_window::start_download(const QUrl &url) {
	QString error;
	// A download belongs to the node it came from (sec 11.2), so a torrent lands
	// in the tree beside every other download rather than in a separate world.
	QString node_id;
	if (web_view_backend *v = current_view()) {
		for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
			if (it.value() == v) { node_id = it.key(); break; }
	}

	const int id = m_downloads->enqueue(url, node_id, &error);
	if (!id) {
		m_status->showMessage(error, 8000);
		return;
	}
	m_address->clear();
	// Show the list rather than announce it in the status bar: a download the
	// user cannot watch is not a first-class download (sec 11.2).
	open_downloads();
}

ai_provider *main_window::choose_ai(QString *why) {
	if (!m_local_ai) {
		m_local_ai    = new ollama_provider(this);
		m_external_ai = new claude_provider(this);
		settings_store::load_into(nullptr, nullptr, nullptr, m_local_ai,
		                           m_external_ai);
	}

	// Re-probe every time rather than trusting a cached answer. Ollama is
	// started and stopped like any other local service, so a result from
	// earlier in the session says nothing about now -- and the old code probed
	// asynchronously then read the answer immediately, which meant the first
	// use of the day *never* saw a running local model and quietly used the
	// external one instead.
	QApplication::setOverrideCursor(Qt::WaitCursor);
	const bool local_ok = m_local_ai->probe_now();
	QApplication::restoreOverrideCursor();
	const bool ext_ok   = m_external_ai->available();

	switch (settings_store::ai_mode()) {
		case ai_choice::local_only:
			// Deliberately does *not* fall back. The whole value of this
			// setting is that "nothing leaves this machine" keeps meaning that
			// on the day the local model is not running (sec 1, sec 9.1).
			if (local_ok)
				return m_local_ai;
			if (why)
				*why = "No local model is answering, and the AI backend is set "
				       "to local only. Start Ollama, or change it in Settings.";
			return nullptr;

		case ai_choice::external:
			if (ext_ok)
				return m_external_ai;
			if (why)
				*why = "Claude is selected but has no API key. Set one in "
				       "Settings, or ANTHROPIC_API_KEY.";
			return nullptr;

		case ai_choice::automatic:
			break;
	}

	// Local-first: a reachable local model handles everything and nothing
	// leaves the machine; the external one is the fallback, still gated behind
	// review-before-send by the dialogs.
	if (local_ok)
		return m_local_ai;
	if (ext_ok)
		return m_external_ai;
	if (why)
		*why = "No AI provider: start Ollama locally, or set ANTHROPIC_API_KEY.";
	return nullptr;
}

int main_window::apply_extractor(const QString &host, const QUrl &page) {
	if (host.isEmpty() || !m_extractors.has(host))
		return 0;
	const QList<evidence_request> ev = m_ex_signals->evidence_for(host);
	if (ev.isEmpty())
		return 0;

	// Judged again on every run, not merely when it was accepted. A stored
	// script is re-run against fresh evidence each time, and the same rule
	// applies: it may only return an address this visit actually requested.
	// A site that changes shape therefore stops producing results rather than
	// producing wrong ones.
	const extractor_verdict v =
	    site_extractor::check(m_extractors.source_for(host), page, ev);
	if (!v.usable) {
		m_status->showMessage(
		    QString("The saved extractor for %1 no longer matches this page (%2)")
		        .arg(host, v.message.left(90)), 9000);
		return 0;
	}

	media_item item;
	item.kind = v.result.kind == "hls"    ? media_kind::hls
	          : v.result.kind == "dash"   ? media_kind::dash
	                                      : media_kind::direct;
	item.url       = v.result.url;
	item.site_host = host;
	item.headers   = v.result.headers;   // the whole point of asking for them
	item.label     = QString("Learned · %1").arg(v.result.kind);
	m_media->add_item(host, item);
	return 1;
}

void main_window::learn_this_site() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 5000);
		return;
	}
	QString why;
	ai_provider *chosen = choose_ai(&why);
	if (!chosen) {
		m_status->showMessage(why, 8000);
		return;
	}

	const QString host = v->url().host();
	extractor_dialog dlg(m_ex_signals, &m_extractors, chosen, host, v->url(), this);

	// The sec 11.5.1 helper tier, and only where this site has been trusted with
	// it. `extractor_fetch` defaults to block, so on an ordinary site the
	// script gets no `hydra` at all and cannot tell the surface exists.
	//
	// Both live on the stack beside the modal dialog on purpose: a helper_host
	// must outlive the run it was handed to, and the run cannot outlive an
	// exec() that has not returned.
	std::unique_ptr<network_fetcher> fetcher;
	std::unique_ptr<helper_allowlist> allow;
	std::unique_ptr<helper_host> helpers;
	if (m_policy && m_policy->is_allowed(policy::feature::extractor_fetch, host)) {
		stream_context ctx;
		ctx.referer = v->url().toString();
		fetcher = std::make_unique<network_fetcher>(ctx);
		allow   = std::make_unique<helper_allowlist>();
		allow->observe(m_ex_signals->evidence_for(host));
		helpers = std::make_unique<helper_host>(allow.get(), fetcher->as_function(),
		                                         helper_budget{});
		// The same ranking the content-type probe uses, handed to the script so
		// its budget is spent where the answer usually is rather than on
		// whatever the page happened to request first.
		QStringList ranked;
		for (const evidence_request &r : extractor_dialog::candidates(
		         m_ex_signals->evidence_for(host), v->url(), helper_budget{}.max_calls))
			ranked << r.url.toString();
		helpers->set_candidates(ranked);
		dlg.use_helpers(helpers.get());
	}

	if (dlg.exec() != QDialog::Accepted)
		return;

	const QString dir = QStandardPaths::writableLocation(
	    QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	m_extractors.save(QDir(dir).filePath("extractors.json"));
	const int found = apply_extractor(host, v->url());
	m_status->showMessage(
	    found ? QString("Extractor saved for %1, and it found a stream — see the "
	                     "media list.").arg(host)
	          : QString("Extractor saved for %1.").arg(host), 9000);
	refresh_media_affordance(host);
}

void main_window::toggle_capture() {
	web_view_backend *v = current_view();
	if (!v) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Open a page first.", 5000);
		return;
	}

	// --- stop --------------------------------------------------------------
	if (!m_capture_url.isEmpty()) {
		if (m_capture_timer)
			m_capture_timer->stop();
		m_capture_action->setText("&Capture Playing Video");
		const qint64 got = m_local_proxy->captured_bytes(m_capture_url);
		m_local_proxy->close_capture(m_capture_url);
		m_capture_url.clear();
		m_capture_action->setChecked(false);

		if (m_capture_src && m_capture_job) {
			m_capture_src->ended(m_capture_job, got > 0,
			                      got > 0 ? QString()
			                              : QString("Nothing was captured."));
			m_capture_job = 0;
		}
		if (got <= 0) {
			QFile::remove(m_capture_path);
			m_status->showMessage("Nothing was captured — the page may not use "
			                       "Media Source, or never started playing.", 9000);
			return;
		}
		// Offer it the same way anything else found on this page is offered, so
		// Watch and the download list need to know nothing about capture.
		media_item item;
		item.kind      = media_kind::direct;
		item.url       = QUrl::fromLocalFile(m_capture_path);
		item.site_host = v->url().host();
		item.label     = QString("Captured · %1")
		                     .arg(QLocale().formattedDataSize(got));
		m_media->add_item(item.site_host, item);
		m_status->showMessage(QString("Captured %1 to %2")
		                          .arg(QLocale().formattedDataSize(got),
		                               m_capture_path), 12000);
		return;
	}

	// --- start -------------------------------------------------------------
	if (!m_local_proxy || !m_local_proxy->listening()) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Cannot capture: the local proxy is not listening.",
		                       8000);
		return;
	}
	const QString host = v->url().host();
	m_capture_path = QDir(m_downloads->directory())
	                     .filePath(QString("%1-%2.mp4")
	                                   .arg(host.isEmpty() ? "capture" : host,
	                                        QDateTime::currentDateTime()
	                                            .toString("yyyyMMdd-hhmmss")));
	m_capture_url = m_local_proxy->open_capture(m_capture_path);
	if (m_capture_url.isEmpty()) {
		m_capture_action->setChecked(false);
		m_status->showMessage("Cannot capture: could not open " + m_capture_path,
		                       8000);
		return;
	}

	// The hook has to be in place before the player builds its MediaSource, so
	// arming means injecting and reloading. Catching a stream mid-playback
	// would miss the init segment and produce a file nothing can decode.
	v->inject_main_world_script("hydra-mse-capture",
	                             mse_tap::capture_source(m_capture_url));
	m_capture_action->setChecked(true);
	m_status->showMessage("Capturing — reloading so the recorder is in place "
	                       "before playback starts. Press play, then turn this "
	                       "off when done.", 12000);

	// Without this the whole recording is silent: arm it, walk away, and
	// nothing says whether anything is being written until it is stopped. The
	// commonest way to get an empty file is forgetting to press play, and that
	// deserves to be said out loud rather than discovered at the end.
	// From here it is an ordinary job: the downloads window shows it with the
	// same progress and the same Cancel as anything else (sec 11.6).
	QString node_id;
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value() == v) { node_id = it.key(); break; }
	m_capture_job = m_downloads->adopt(m_capture_src, v->url(), node_id);
	m_capture_src->began(m_capture_job, m_capture_path);

	m_capture_last   = 0;
	m_capture_warned = false;
	m_capture_clock.start();
	if (!m_capture_timer) {
		m_capture_timer = new QTimer(this);
		m_capture_timer->setInterval(500);
		connect(m_capture_timer, &QTimer::timeout, this, &main_window::poll_capture);
	}
	m_capture_timer->start();

	v->reload();
}

void main_window::poll_capture() {
	if (m_capture_url.isEmpty() || !m_local_proxy)
		return;
	const qint64 got  = m_local_proxy->captured_bytes(m_capture_url);
	const qint64 ms   = qMax<qint64>(1, m_capture_clock.elapsed());
	const QString size = QLocale().formattedDataSize(got);

	// On the action itself, so the count is visible wherever the menu is, not
	// only while a transient status message happens to be showing.
	m_capture_action->setText(QString("&Capture Playing Video — %1").arg(size));

	if (got > m_capture_last) {
		m_capture_last   = got;
		m_capture_warned = false;
		const QString rate = QLocale().formattedDataSize(got * 1000 / ms) + "/s";
		if (m_capture_src)
			m_capture_src->progressed_to(m_capture_job, got, rate);
		m_status->showMessage(QString("Capturing %1 (%2)").arg(size, rate));
		return;
	}

	// Nothing arriving. Say why, once, rather than letting it look like work.
	if (!m_capture_warned && m_capture_clock.elapsed() > 12000) {
		m_capture_warned = true;
		m_status->showMessage(
		  got == 0
		      ? QString("Capturing, but nothing has arrived — press play. If "
		                 "the page is already playing, it may not use Media "
		                 "Source, and nothing here can record it.")
		      : QString("Capture paused at %1 — the page has stopped feeding "
		                 "its player.").arg(size),
		  15000);
	}
}

void main_window::find_media_with_ytdlp() {
	web_view_backend *v = current_view();
	if (!v) {
		m_status->showMessage("Open a page first.", 5000);
		return;
	}
	if (!m_ytdlp)
		m_ytdlp = new ytdlp_resolver(this);
	if (!m_ytdlp->available()) {
		m_status->showMessage(m_ytdlp->description(), 8000);
		return;
	}
	if (m_ytdlp->busy()) {
		m_status->showMessage("Still looking…", 3000);
		return;
	}

	const QUrl page = v->url();
	const QString host = page.host();
	m_status->showMessage(QString("Asking yt-dlp about %1…").arg(host));

	// One-shot: these are reconnected per request so a later answer cannot
	// arrive against a page the user has since left.
	m_ytdlp->disconnect(this);
	connect(m_ytdlp, &ytdlp_resolver::resolved, this,
	         [this, host](const resolved_media &m) {
		int added = 0;
		for (const media_format &f : m.formats) {
			if (!f.has_video && !f.has_audio)
				continue;
			media_item item;
			// Trust yt-dlp's protocol rather than re-guessing from the URL --
			// knowing this authoritatively is the entire point of asking it.
			item.kind = (f.protocol.startsWith("m3u8")) ? media_kind::hls
			          : (f.protocol.startsWith("http_dash")) ? media_kind::dash
			                                                 : media_kind::direct;
			item.url       = f.url;
			item.site_host = host;
			item.label     = QString("%1 %2%3")
			                     .arg(m.title.isEmpty() ? host : m.title,
			                          f.height ? QString::number(f.height) + "p"
			                                   : f.ext,
			                          f.note.isEmpty() ? QString()
			                                           : " · " + f.note);
			m_media->add_item(host, item);
			++added;
		}
		m_status->showMessage(
		  added ? QString("yt-dlp (%1): %2 stream(s) found.")
		              .arg(m.extractor).arg(added)
		        : QString("yt-dlp found nothing playable."), 8000);
		if (added)
			open_media();
	}, Qt::SingleShotConnection);
	connect(m_ytdlp, &ytdlp_resolver::failed, this, [this](const QString &e) {
		// yt-dlp's own words: "Unsupported URL" is the answer that matters,
		// and it is exactly the case sec 11.6's tap exists for.
		m_status->showMessage("yt-dlp: " + e, 10000);
	}, Qt::SingleShotConnection);

	m_ytdlp->resolve(page);
}

void main_window::open_settings() {
	QString ignored;
	choose_ai(&ignored);          // make sure the providers exist to configure
	settings_dialog dlg(m_players, m_downloads, m_torrents, m_local_ai,
	                     m_external_ai, m_policy, m_filters, m_filters_path,
	                     m_consent, m_site_rules_path, this, m_factory);
	dlg.exec();
	// Global defaults may have moved, and every live view was configured from
	// the old ones. Re-apply rather than wait for the next navigation, or the
	// setting appears not to have taken until the page is reloaded.
	for (auto it = m_views_by_id.cbegin(); it != m_views_by_id.cend(); ++it)
		if (it.value())
			apply_policy(it.value(), it.value()->url().host());
}

void main_window::open_downloads() {
	if (!m_downloads_ui)
		m_downloads_ui = new downloads_dialog(m_downloads, m_players,
		                                      m_local_proxy, this);
	m_downloads_ui->show();
	m_downloads_ui->raise();
	m_downloads_ui->activateWindow();
}

void main_window::confirm_public_download(const QString &source_id,
                                           const QString &note, int job_id) {
	download_source *src = m_downloads->source_by_id(source_id);
	const QString name = src ? src->display_name() : source_id;

	QMessageBox box(this);
	box.setIcon(QMessageBox::Warning);
	box.setWindowTitle(name + " is not a private download");
	box.setText(note);
	box.setInformativeText(
	  "Continue only if that is acceptable for what you are downloading. "
	  "This choice is remembered for the rest of this session.");
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
	box.setDefaultButton(QMessageBox::No);

	if (box.exec() == QMessageBox::Yes) {
		m_downloads->set_consent(source_id, true);
	} else {
		m_downloads->cancel(job_id);
		m_status->showMessage("Download cancelled.", 5000);
	}
}

void main_window::go_back() {
	if (web_view_backend *v = current_view())
		v->back();
}

void main_window::go_forward() {
	if (web_view_backend *v = current_view())
		v->forward();
}

void main_window::reload_page() {
	// The same action, so the same slot: which of the two it means is a
	// property of the moment rather than of the button.
	if (m_loading) {
		if (web_view_backend *v = current_view())
			v->stop();
		set_loading(false);
		m_progress->hide();
		m_status->showMessage("Stopped.", 3000);
		return;
	}
	if (web_view_backend *v = current_view())
		v->reload();
}

void main_window::mark_dirty() {
	m_save_timer->start();  // debounced flush_tree()
}

void main_window::flush_tree() {
	if (!m_tree_path.isEmpty())
		m_model->save(m_tree_path);
	persist_histories();
}

// Write the imported back/forward records that are not on disk yet.
//
// **Written once, not on every flush.** A history is a record of where a tab
// had been before it arrived, so it does not change afterwards -- which makes
// "the file exists" a sufficient test and keeps a debounced save from
// rewriting a few hundred small files every time a row is renamed. It also
// leaves a file somebody has edited by hand alone, which the format is
// human-readable in order to allow.
void main_window::persist_histories(node *from) {
	if (!m_state)
		return;
	node *n = from ? from : (m_model ? m_model->root() : nullptr);
	if (!n)
		return;
	// A mirror is somebody else's session and is never written to the tree, so
	// writing its histories would leave state/ full of blobs keyed to ids that
	// no saved tree mentions.
	if (!n->mirror.isEmpty())
		return;
	if (!n->history.is_empty() && !n->id.isEmpty() && !m_state->has_history(n->id))
		m_state->save_history(n->id, tab_history_codec::encode(n->history));
	for (node *c : n->children)
		persist_histories(c);
}

// And the other direction, once the tree is in memory.
void main_window::restore_histories(node *from) {
	if (!m_state)
		return;
	node *n = from ? from : (m_model ? m_model->root() : nullptr);
	if (!n)
		return;
	if (!n->id.isEmpty() && m_state->has_history(n->id))
		n->history = tab_history_codec::decode(m_state->load_history(n->id));
	for (node *c : n->children)
		restore_histories(c);
}

// **The address bar shows the address; the status bar does not repeat it.**
// This echoed the url into the status bar on every navigation, which put the
// same string on screen twice a few pixels apart -- and, worse, made every
// navigation wipe whatever the status bar was saying. A load failure, a
// refused certificate, a blocked popup and a dead renderer all say their piece
// there, and all of them could be erased by the next url_changed. The status
// bar is for what is *happening*; the address bar is for where you are.
void main_window::update_address(const QString &url) {
	m_address->setText(url);
}

void main_window::apply_policy(web_view_backend *view, const QString &host) {
	if (!view)
		return;
	using F = policy::feature;
	view_settings s;
	s.javascript = m_policy->is_allowed(F::javascript, host);
	s.images     = m_policy->is_allowed(F::images, host);
	s.autoplay   = m_policy->is_allowed(F::autoplay, host);
	s.popups     = m_policy->is_allowed(F::popups, host);
	// **Scrollbars are the kiosk's, not the policy's, and this used to hand
	// them back.** `view_settings` is built fresh here and `scrollbars`
	// defaults to true, so every re-apply -- and one runs on each navigation
	// -- restored the bars kiosk mode had turned off. An unattended screen
	// grew scrollbars the moment it followed a link, which is precisely the
	// thing kiosk mode exists to prevent and precisely when nobody is watching
	// it happen.
	//
	// Asked rather than remembered: the controller owns the state and can
	// answer, and a second copy of it here would be a second thing to keep
	// right.
	s.scrollbars = !(m_kiosk && m_kiosk->active());
	view->apply_settings(s);

	// **"Always ask as a desktop for this site", applied on arrival.**
	//
	// Not part of `view_settings`, because it is not a setting the engine
	// carries: it changes the user agent, which is read when a page is fetched,
	// so it can only take effect on a load. Setting it here means the first
	// visit to such a site loads as a phone, the backend notices the mismatch
	// and reloads -- costing one extra round trip and then landing on the page
	// that was wanted. That is the honest cost of a switch that has to be
	// decided before the request it affects.
	//
	// It is a no-op unless the value actually changes, which is what stops the
	// reload from looping: after the second load the view already agrees with
	// the policy and nothing further happens. The per-tab toggle in the View
	// menu writes the same state, so a site rule and a one-off use the same
	// path and cannot disagree about what the view is currently doing.
	const bool want_desktop = m_policy->is_allowed(F::desktop_site, host);
	if (view->desktop_site() != want_desktop)
		view->set_desktop_site(want_desktop);
}

void main_window::open_site_controls() {
	if (!m_policy_dialog) {
		m_policy_dialog = new site_policy_dialog(m_policy, this);
		connect(m_policy_dialog, &site_policy_dialog::policy_changed,
		         this, &main_window::on_policy_changed);
	}
	QString host;
	if (web_view_backend *v = current_view())
		host = v->url().host();
	m_policy_dialog->set_host(host);
	m_policy_dialog->move(m_address->mapToGlobal(QPoint(0, m_address->height() + 2)));
	m_policy_dialog->show();
	m_policy_dialog->raise();
}

void main_window::on_policy_changed() {
	if (!m_policy_path.isEmpty())
		m_policy->save(m_policy_path);
	if (web_view_backend *v = current_view()) {
		apply_policy(v, v->url().host());
		v->reload();
	}
}

namespace {

// The folders somebody has open, by node id.
//
// **Ids, not row numbers.** A row number is a position in a tree that reorders
// itself -- sorting changes it, and so does anything that adds a sibling -- so
// a saved row would restore the wrong folder as soon as the shape moved. An id
// is "short, opaque, stable for the node's lifetime" by `node.h`'s own
// promise, which is exactly the property this needs.
void collect_open_folders(const tab_tree_model *model,
                           const QSortFilterProxyModel *proxy,
                           const QTreeView *view, node *n, QStringList &out) {
	for (node *c : n->children) {
		if (!c->is_folder())
			continue;
		// Through the proxy, because the view knows nothing else. `isExpanded`
		// takes the index the *view* uses, and handing it a source index is the
		// kind of mistake that reads as "no folders were open" rather than as
		// an error.
		const QModelIndex idx = proxy->mapFromSource(model->index_for_node(c));
		if (idx.isValid() && view->isExpanded(idx))
			out << c->id;
		collect_open_folders(model, proxy, view, c, out);
	}
}

}  // namespace

void main_window::save_view_state() const {
	if (m_view_path.isEmpty() || !m_model || !m_tree || !m_proxy)
		return;
	QSettings v(m_view_path, QSettings::IniFormat);
	QStringList open;
	collect_open_folders(m_model, m_proxy, m_tree, m_model->root(), open);
	v.setValue("open_folders", open.join(QLatin1Char(',')));
	if (node *n = selected_node())
		v.setValue("current", n->id);
	else
		v.remove("current");
	// Base64 because saveGeometry is binary and this file is meant to be
	// readable and hand-editable like the policy beside it; a raw blob in an
	// ini is neither.
	v.setValue("geometry", QString::fromLatin1(saveGeometry().toBase64()));
	if (m_sort_box)
		v.setValue("sort", m_sort_box->currentIndex());
}

void main_window::restore_view_state() {
	// **No file means first run, and first run keeps the old behaviour.**
	// Everything expanded is the right thing to show somebody who has never
	// arranged anything; it is only wrong as an answer to somebody who has.
	if (m_view_path.isEmpty() || !QFileInfo::exists(m_view_path)) {
		m_tree->expandAll();
		return;
	}
	QSettings v(m_view_path, QSettings::IniFormat);

	const QByteArray geom =
	  QByteArray::fromBase64(v.value("geometry").toString().toLatin1());
	if (!geom.isEmpty())
		restoreGeometry(geom);
	if (m_sort_box) {
		const int sort = v.value("sort", -1).toInt();
		if (sort >= 0 && sort < m_sort_box->count())
			m_sort_box->setCurrentIndex(sort);
	}

	// Collapse first, then open what was open. Without the collapse this would
	// only ever add folders: a run that starts from an expanded tree and is
	// told to open three would leave every other folder open too, and the
	// setting would look like it did nothing.
	m_tree->collapseAll();
	const QStringList open =
	  v.value("open_folders").toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (const QString &id : open) {
		node *n = m_model->node_by_id(id);
		if (!n)
			continue;   // a folder deleted since; not an error
		const QModelIndex idx = m_proxy->mapFromSource(m_model->index_for_node(n));
		if (idx.isValid())
			m_tree->setExpanded(idx, true);
	}

	// And the tab that was in front. Activated rather than merely selected: a
	// selection highlights a row, and what was asked for is the page back.
	const QString current = v.value("current").toString();
	if (node *n = current.isEmpty() ? nullptr : m_model->node_by_id(current)) {
		const QModelIndex idx = m_proxy->mapFromSource(m_model->index_for_node(n));
		if (idx.isValid()) {
			m_tree->setCurrentIndex(idx);
			m_tree->scrollTo(idx);
			if (!n->is_folder())
				on_tree_activated(idx);
		}
	}
}

// Everything that has to be on disk before this process stops existing.
//
// Split out of `closeEvent` because closing the window is no longer the only
// way the browser ends. A signalled exit -- a logout, a shutdown, a Ctrl-C --
// never delivers a close event, so for as long as this was the body of one it
// was also the list of things a signalled exit threw away: not just the tree,
// which the debounce timer had usually just written, but the view state and
// every live tab's suspended blob, neither of which anything else writes.
//
// It suspends the live views, so it is the *end*, not a checkpoint. Nothing
// that means to carry on running may call it.
void main_window::save_everything() {
	// Leave kiosk first, so the presented view is back in the stack and can be
	// suspended with the rest.
	if (m_kiosk && m_kiosk->active())
		m_kiosk->exit();

	// Persist live tabs as suspended blobs so they restore next launch,
	// then write the tree structure.
	const QList<QString> live_ids = m_views_by_id.keys();
	for (const QString &id : live_ids)
		if (node *n = m_model->node_by_id(id))
			suspend_node(n);
	if (!m_tree_path.isEmpty())
		m_model->save(m_tree_path);
	if (!m_policy_path.isEmpty())
		m_policy->save(m_policy_path);
	save_view_state();
}

void main_window::closeEvent(QCloseEvent *event) {
	save_everything();
	QWidget::closeEvent(event);
}
