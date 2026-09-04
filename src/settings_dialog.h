#pragma once

#include "ai_provider.h"

#include "kiosk_controller.h"
#include "theme.h"

#include <QDialog>
#include <QSet>
#include <QList>

class QCheckBox;
class QComboBox;
class QListWidget;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QVBoxLayout;
class claude_provider;
class download_manager;
class ollama_provider;
class player_launcher;
class torrent_download_source;

// Settings (architecture doc sec 11.3, sec 11.4).
//
// Everything here already existed as API and could only be reached by editing
// code -- the external-player choice sec 11.3 specifies in detail, and the
// BitTorrent knobs sec 11.4's whole connection-scaling argument rests on. A
// setting nobody can change is a setting that does not exist, which is why the
// caps in particular belong here: the argument for rasterbar was that the
// ceiling can be raised, and that claim is only true if someone can raise it.
//
// Values are read and written through `settings_store` below, so the dialog
// owns presentation and the store owns persistence and application.
class policy_engine;
class filter_list;
class consent_blocker;
class web_view_factory;
class QTreeWidget;
class QPushButton;
class QStackedWidget;

class settings_dialog : public QDialog {
	Q_OBJECT
public:
	settings_dialog(player_launcher *players, download_manager *downloads,
	                 torrent_download_source *torrents, ollama_provider *local_ai,
	                 claude_provider *external_ai, policy_engine *policy = nullptr,
	                 filter_list *filters = nullptr,
	                 const QString &filters_path = QString(),
	                 consent_blocker *consent = nullptr,
	                 const QString &rules_path = QString(),
	                 QWidget *parent = nullptr,
	                 // **After `parent`, which is not where Qt usually puts
	                 // the last argument**, and deliberately: `parent` is the
	                 // one argument every existing caller passes positionally,
	                 // so anywhere else this would have been a shuffle through
	                 // eleven call sites for one new value. Appended, it costs
	                 // each of them nothing until they want it.
	                 //
	                 // Only ever used to clear browsing data, and taken as the
	                 // engine-neutral seam type so that this dialog still does
	                 // not know what engine is behind it (architecture doc
	                 // sec 19.2). Null is a supported state: the clear controls
	                 // say there is nothing to clear with rather than
	                 // disappearing, because a control that vanishes tells
	                 // nobody why.
	                 web_view_factory *views = nullptr);

	// Saving belongs to *accepting*, not to a particular button. It hung off the
	// OK button's signal, so any other route to acceptance -- code calling
	// accept(), a future default-button change, a shortcut -- would have closed
	// the window and silently discarded everything typed into it. Found by a
	// test that accepted the dialog the obvious programmatic way.
	void accept() override;
	void reject() override;

protected:
	void resizeEvent(QResizeEvent *event) override;

signals:
	// **Emitted when the button was actually pressed and a clear went out.**
	// The shell keeps caches of its own that no backend knows about -- the
	// per-session permission answers most of all -- and the clear goes
	// straight from here to the view factory, so without this nothing in the
	// window ever learns that somebody asked to be forgotten.
	void browsing_data_cleared();

private:
	// Below this the category list stops being a sidebar and becomes a
	// dropdown. Named for the same reason `main_window::k_drawer_threshold`
	// is: a bare number in a comparison is a decision nobody can find later.
	static constexpr int k_narrow_threshold = 520;   // logical px
	void update_layout_mode();

	void build_privacy_page(QWidget *page);
	// The one destructive control on this dialog, so it is built apart from
	// the rest of the privacy page and reads that way on screen.
	void build_clear_section(QVBoxLayout *v, QWidget *page);
	void clear_browsing_data();
	void build_appearance_page(QWidget *page);
	void build_kiosk_page(QWidget *page);
	void build_filter_page(QWidget *page);
	void rebuild_filter_list();
	// The per-site exceptions list on the privacy page: what the shield has been
	// used to say, gathered in one place so it can be reviewed and undone
	// without visiting each site to find out.
	void rebuild_exceptions();
	// Put one page's controls back to what a fresh install would show. Not
	// written anywhere until OK, which is why it needs no confirmation dialog:
	// Cancel is the undo.
	void restore_page_defaults(int page);
	void export_settings();
	void import_settings();
	void update_restore_button();
	void rebuild_site_rules();
	void index_pages(QWidget *page, int page_index);
	void run_search(const QString &text);
	void load_kiosk();
	void apply_kiosk();
	void build_player_page(QWidget *page);
	void build_download_page(QWidget *page);
	void build_ai_page(QWidget *page);
	void update_ai_state();
	// Probing in a settings window is an explicit act, never something that
	// happens because the window opened. Both of these are wired to a button.
	void check_local_model();
	void rescan_players();
	void populate_players();
	void load();
	void apply();
	void update_custom_state();

	policy_engine           *m_policy   = nullptr;
	filter_list             *m_filters  = nullptr;
	QString                  m_filters_path;
	QTreeWidget             *m_filter_view   = nullptr;
	QPushButton             *m_filter_remove = nullptr;
	QLabel                  *m_filter_note   = nullptr;
	consent_blocker         *m_consent       = nullptr;
	web_view_factory        *m_views         = nullptr;   // injected, not owned
	QCheckBox               *m_clear_cookies = nullptr;
	QCheckBox               *m_clear_cache   = nullptr;
	QCheckBox               *m_clear_links   = nullptr;
	QPushButton             *m_clear_go      = nullptr;
	QLabel                  *m_clear_note    = nullptr;
	QString                  m_rules_path;
	QTreeWidget             *m_rules_view    = nullptr;
	QPushButton             *m_rules_remove  = nullptr;
	QPushButton             *m_rules_copy    = nullptr;
	QLabel                  *m_rules_note    = nullptr;
	player_launcher         *m_players  = nullptr;
	download_manager        *m_downloads = nullptr;
	torrent_download_source *m_torrents = nullptr;
	ollama_provider         *m_local_ai = nullptr;
	claude_provider         *m_external_ai = nullptr;

	// One combo per policy feature, indexed by the enum, so a feature added to
	// the model turns up here without anyone remembering to add a row.
	QListWidget          *m_categories = nullptr;
	// Stands in for the list when the window is too narrow to afford a
	// permanent sidebar. See `update_layout_mode` for the threshold and why.
	QComboBox            *m_category_pick = nullptr;
	QLineEdit            *m_search      = nullptr;
	QListWidget          *m_results     = nullptr;
	// Everything findable, gathered by walking the built pages rather than
	// declared alongside each control: a registry someone has to remember to
	// add to is a registry that goes stale, and the point of a search box is
	// that it finds the setting nobody could remember the name of.
	struct searchable { QString text; int page; QWidget *widget; };
	QList<searchable>     m_index;
	QLineEdit            *m_kiosk_home   = nullptr;
	QSpinBox             *m_kiosk_w      = nullptr;
	QSpinBox             *m_kiosk_h      = nullptr;
	QComboBox            *m_kiosk_scale  = nullptr;
	QComboBox            *m_kiosk_fit    = nullptr;
	QCheckBox            *m_kiosk_cursor = nullptr;
	QSpinBox             *m_kiosk_idle   = nullptr;
	QCheckBox            *m_kiosk_dog    = nullptr;
	QCheckBox            *m_kiosk_escape = nullptr;
	QCheckBox            *m_kiosk_clear  = nullptr;
	QList<QComboBox *>    m_feature_combos;
	QTreeWidget          *m_exceptions      = nullptr;
	QPushButton          *m_exception_drop  = nullptr;
	QLabel               *m_exception_note  = nullptr;
	QLabel               *m_bundle_note     = nullptr;
	// Removals are pending until OK, like every other control on this dialog.
	// The shield applies immediately because it is a menu on a live page; a
	// dialog with a Cancel button that had already discarded rules would be
	// lying about what Cancel does.
	QSet<QString>         m_dropped_patterns;
	QComboBox            *m_appearance     = nullptr;
	QPushButton          *m_restore        = nullptr;
	QStackedWidget       *m_stack          = nullptr;
	QList<QRadioButton *> m_player_buttons;
	QVBoxLayout    *m_player_group_layout = nullptr;
	QLineEdit      *m_custom_cmd   = nullptr;
	QLabel         *m_player_note  = nullptr;
	QLineEdit      *m_search_engine = nullptr;
	QLabel         *m_search_note   = nullptr;

	QLineEdit      *m_dir          = nullptr;
	QSpinBox       *m_conn_global  = nullptr;
	QSpinBox       *m_conn_torrent = nullptr;
	QDoubleSpinBox *m_seed_ratio   = nullptr;
	QLineEdit      *m_interfaces   = nullptr;
	QCheckBox      *m_sequential   = nullptr;

	QRadioButton *m_ai_auto     = nullptr;
	QRadioButton *m_ai_local    = nullptr;
	QRadioButton *m_ai_external = nullptr;
	QLineEdit    *m_ollama_url   = nullptr;
	QLineEdit    *m_ollama_model = nullptr;
	QSpinBox     *m_probe_timeout = nullptr;
	QPushButton  *m_check_local   = nullptr;
	// Nothing is claimed about the local model until it has been checked.
	enum class probe_state { unknown, checking, reachable, unreachable };
	probe_state   m_probe_state = probe_state::unknown;
	QLineEdit    *m_claude_model = nullptr;
	QLineEdit    *m_claude_key   = nullptr;
	QLabel       *m_ai_status    = nullptr;
};

// Where settings live between runs, and the one place that pushes them into the
// objects that use them.
//
// Split out from the dialog so startup can apply saved values without building
// any UI -- otherwise "settings persist" would quietly mean "settings persist if
// you open the dialog", which is the usual way this goes wrong.
namespace settings_store {

void load_into(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai);

void save_from(player_launcher *players, download_manager *downloads,
                torrent_download_source *torrents, ollama_provider *local_ai,
                claude_provider *external_ai);

// Which backend the user picked. Read straight from storage rather than held
// on any one object, because it is a preference *about* providers rather than
// a property of either of them.
// Kiosk (sec 8). Kept here rather than on the controller for the same reason the
// AI mode is: it is a preference *about* a session that may not exist yet, and
// until now it was code-level defaults with no way to reach it from the app at
// all -- a presentation mode nobody could configure.
kiosk_config kiosk();
void set_kiosk(const kiosk_config &c);

ai_choice ai_mode();
void set_ai_mode(ai_choice mode);

// Light, dark, or follow the desktop. Default is to follow -- see `theme.h` for
// why that is harder than it sounds.
theme::choice appearance();
void          set_appearance(theme::choice c);

// The search template the address bar uses when what was typed is not an
// address, with `%1` standing for the terms. Stored rather than compiled in
// because the engine somebody searches through is theirs to choose, and it is
// the one setting here that sends what they typed to a third party.
QString search_engine();
void    set_search_engine(const QString &tmpl);

}  // namespace settings_store
