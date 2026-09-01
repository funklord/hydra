#include "site_policy_dialog.h"
#include "policy_engine.h"
#include "policy.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QComboBox>
#include <QStandardItemModel>
#include <QLabel>
#include <QFrame>
#include <QFont>
#include <QtGlobal>
#include <QList>

#include <cstring>
#include <iterator>

using policy::feature;
using policy::setting;

namespace {

// **`ask` was missing from both of these and that was not a display bug.**
// The combo had three entries, `ask` fell into the `default:` arm and drew as
// "Default", and the moment anybody touched that row it was written back as
// `unset`. So a site the person had chosen to be asked about silently became a
// site on the global default, and nothing said so.
int setting_to_index(setting s) {
	switch (s) {
		case setting::allow: return 1;
		case setting::block: return 2;
		case setting::ask:   return 3;
		default:             return 0;  // unset
	}
}

setting index_to_setting(int i) {
	switch (i) {
		case 1:  return setting::allow;
		case 2:  return setting::block;
		case 3:  return setting::ask;
		default: return setting::unset;
	}
}

}  // namespace

namespace {

struct shield_row { policy::feature f; const char *group; };

// **The order this panel is read in, which the enum was never meant to
// supply.** It used to draw one row per feature in declaration order, so
// fifteen controls arrived as a flat wall with the camera three rows from the
// referer header and no indication that any of them were related.
//
// The last group is a request rather than a taxonomy, and it is worth being
// accurate about why it exists. These three are *rule-driven*: what they reach
// depends on data that may be thin or absent, where everything above them is
// enforced directly by the engine or by this program. Measured rather than
// assumed -- `ads` consults a built-in host check and then a filter list that
// is empty until somebody imports one, and `cookie_notices` runs from
// `site_rules::defaults()` plus whatever has been added since. `popups` is the
// exception in the group: it is enforced outright in `main_window`, needs no
// list, and sits here because it belongs with the other two in a reader's mind
// rather than in the implementation's.
const shield_row k_shield_layout[] = {
	{ policy::feature::javascript,          "Content" },
	{ policy::feature::images,              "Content" },
	{ policy::feature::autoplay,            "Content" },
	{ policy::feature::media_detect,        "Content" },

	{ policy::feature::camera,              "Camera, microphone and screen" },
	{ policy::feature::microphone,          "Camera, microphone and screen" },
	{ policy::feature::geolocation,         "Camera, microphone and screen" },
	{ policy::feature::notifications,       "Camera, microphone and screen" },
	{ policy::feature::clipboard_read,      "Camera, microphone and screen" },
	{ policy::feature::pointer_lock,        "Camera, microphone and screen" },

	{ policy::feature::cookies,             "Cookies and site data" },
	{ policy::feature::third_party_cookies, "Cookies and site data" },

	{ policy::feature::referer,             "Sent with requests" },
	{ policy::feature::autofill,            "Sent with requests" },
	{ policy::feature::extractor_fetch,     "Sent with requests" },

	{ policy::feature::ads,                 "Blocking that needs rules" },
	{ policy::feature::popups,              "Blocking that needs rules" },
	{ policy::feature::cookie_notices,      "Blocking that needs rules" },
};

// **A hand-written list under a name that quantifies is a claim that ages.**
// `settings_dialog`'s equivalent table is missing four features today, and
// nothing reports it -- a feature added to the enum simply stops appearing in
// that dialog, silently, because no assertion can fail about a row nobody
// wrote. This one is checked against the enum at construction: every feature
// exactly once, or the panel is wrong and says so where somebody will see it.
bool layout_covers_every_feature() {
	const int n = policy::feature_count();
	QList<int> seen(n, 0);
	for (const shield_row &r : k_shield_layout) {
		const int i = static_cast<int>(r.f);
		if (i < 0 || i >= n)
			return false;
		++seen[i];
	}
	for (int i = 0; i < n; ++i)
		if (seen[i] != 1)
			return false;
	return static_cast<int>(std::size(k_shield_layout)) == n;
}

}  // namespace

site_policy_dialog::site_policy_dialog(policy_engine *engine, QWidget *parent)
  : QDialog(parent), m_engine(engine) {
	setWindowFlags(Qt::Popup);
	setWindowTitle("Site controls");

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(12, 12, 12, 12);
	outer->setSpacing(8);

	m_host_label = new QLabel(this);
	QFont hf = m_host_label->font();
	hf.setBold(true);
	m_host_label->setFont(hf);
	outer->addWidget(m_host_label);

	m_scope = new QComboBox(this);
	m_scope->setObjectName("scope");
	m_scope->addItem("This host");
	m_scope->addItem("This domain");
	m_scope->addItem("Global default");
	connect(m_scope, &QComboBox::currentIndexChanged,
	         this, &site_policy_dialog::on_scope_changed);
	outer->addWidget(m_scope);

	auto *line = new QFrame(this);
	line->setFrameShape(QFrame::HLine);
	outer->addWidget(line);

	auto *grid = new QGridLayout;
	grid->setHorizontalSpacing(12);
	grid->setVerticalSpacing(4);
	m_combos.resize(policy::feature_count());

	// Checked here rather than trusted: the cost of the table being short by one
	// is a control that silently cannot be reached from the shield, which is
	// exactly the failure that is invisible from inside the dialog.
	if (!layout_covers_every_feature())
		qWarning("site_policy_dialog: the shield layout does not cover every "
		          "policy feature exactly once -- a control is missing from "
		          "this panel");

	int row = 0;
	const char *open_group = nullptr;
	for (const shield_row &r : k_shield_layout) {
		if (!open_group || std::strcmp(open_group, r.group) != 0) {
			auto *head = new QLabel(r.group, this);
			QFont f = head->font();
			f.setBold(true);
			head->setFont(f);
			// Air above every heading but the first, so the groups read as
			// groups rather than as a list with some bold entries in it.
			if (open_group)
				grid->setRowMinimumHeight(row++, 10);
			grid->addWidget(head, row++, 0, 1, 2);
			open_group = r.group;
		}
		const int i = static_cast<int>(r.f);
		grid->addWidget(new QLabel(policy::feature_label(r.f), this), row, 0);

		auto *combo = new QComboBox(this);
		// Text set by `refresh_default_labels()`, because what this entry
		// means depends on the global default and that changes under us.
		combo->addItem("Default");
		combo->addItem("Allow");
		combo->addItem("Block");
		// **Present on every row, enabled on six.** Greyed rather than absent,
		// the way "Default" is greyed in the global scope: a combo whose entry
		// count depends on the row makes every index mean something different
		// per row, and `setting_to_index` above would have to know which. The
		// list that decides is `policy::can_ask`, which answers from the same
		// table the prompt takes its words from -- so a feature is offerable
		// here exactly when there is a sentence to put in front of somebody.
		combo->addItem("Ask");
		if (!policy::can_ask(r.f))
			if (auto *m = qobject_cast<QStandardItemModel *>(combo->model()))
				if (QStandardItem *it = m->item(3))
					it->setEnabled(false);
		connect(combo, &QComboBox::currentIndexChanged,
		         this, [this, i](int) { on_feature_changed(i); });
		m_combos[i] = combo;
		grid->addWidget(combo, row, 1);
		++row;
	}
	outer->addLayout(grid);
}

void site_policy_dialog::set_host(const QString &host) {
	m_host = host;

	// **With no page there is no site to have a rule about**, and the two site
	// scopes then resolve to an empty pattern -- which `set_setting` will
	// happily store, leaving a rule keyed on nothing that matches nothing and
	// can only be found by reading policy.ini. So they are disabled and the
	// scope falls back to the global defaults, which are still meaningful and
	// still worth being able to edit from here.
	const bool have_site = !host.isEmpty();
	m_host_label->setText(have_site
	                          ? host
	                          : QStringLiteral("No page open \u2014 global defaults"));
	for (int i = 0; i < 2; ++i) {
		// Disabled through the model, which is how a QComboBox greys one entry
		// rather than removing it: the choice stays visible, so it is clear
		// that per-site rules exist and simply have nothing to apply to.
		if (auto *m = qobject_cast<QStandardItemModel *>(m_scope->model()))
			if (QStandardItem *it = m->item(i))
				it->setEnabled(have_site);
	}
	if (!have_site)
		m_scope->setCurrentIndex(2);

	m_scope->setItemText(1, have_site
	                            ? "This domain (*." +
	                                  policy_engine::etld_plus_one(host) + ")"
	                            : QStringLiteral("This domain"));
	repopulate();

	// **A popup does not hand focus to a child, and a dialog does.** With
	// `Qt::Popup` set, nothing in here had it -- so this panel of fifteen
	// controls opened with no focus ring anywhere and no entry point for
	// anybody not using a pointer. The scope selector is the right place to
	// land: it decides what everything below it applies to, so reading it
	// first is the order the panel is meant to be used in.
	m_scope->setFocus(Qt::PopupFocusReason);
}

QString site_policy_dialog::current_pattern() const {
	switch (m_scope->currentIndex()) {
		case 0:  return m_host;
		case 1:  return "*." + policy_engine::etld_plus_one(m_host);
		default: return QString();  // global
	}
}

void site_policy_dialog::repopulate() {
	m_populating = true;
	const bool global = (m_scope->currentIndex() == 2);
	const QString pattern = current_pattern();
	for (int i = 0; i < policy::feature_count(); ++i) {
		const feature f = static_cast<feature>(i);
		setting s = global ? m_engine->global_default(f)
		                   : m_engine->setting_for(pattern, f);
		m_combos[i]->setCurrentIndex(setting_to_index(s));
	}
	refresh_default_labels();
	m_populating = false;
}

// **"Default" on its own does not tell anybody anything.** Every other entry
// in these boxes states an outcome -- Allow, Block -- and the one that is
// selected for most sites most of the time stated only that a choice had not
// been made. Whether that meant the scripts ran was a fact held one dialog
// away, on the settings page, per feature, and the shield is exactly where
// somebody is standing when they want to know.
//
// It cannot be written once at construction: it is the *global default* that
// decides, that is editable from this same dialog's third scope and from the
// settings page, and a label baked in at build time would go stale the moment
// either changed.
void site_policy_dialog::refresh_default_labels() {
	const bool global = (m_scope->currentIndex() == 2);
	for (int i = 0; i < policy::feature_count() && i < m_combos.size(); ++i) {
		const feature f = static_cast<feature>(i);
		const setting d = m_engine->global_default(f);
		// "allowed"/"blocked" rather than "enabled"/"disabled": the entries
		// below it say Allow and Block, and one concept wants one word.
		// Three defaults to name now, not two. It read `block ? blocked :
		// allowed`, which would have called an `ask` default "allowed" -- the
		// one word of the three that is actively wrong, since a site left on
		// that default gets a prompt and may well end up refused.
		m_combos[i]->setItemText(
		  0, d == setting::block ? QStringLiteral("Default (blocked)")
		     : d == setting::ask ? QStringLiteral("Default (ask each time)")
		                          : QStringLiteral("Default (allowed)"));

		// In the global scope this entry is what a *site* falls back to, so
		// offering it here is a setting pointing at itself -- the settings
		// page declines to offer it at all for that reason, and this dialog
		// used to accept it and quietly turn it into Allow. Greyed rather than
		// removed, the way the scope entries above are, so the row does not
		// change shape as the scope changes.
		if (auto *m = qobject_cast<QStandardItemModel *>(m_combos[i]->model()))
			if (QStandardItem *it = m->item(0))
				it->setEnabled(!global);
	}
}

void site_policy_dialog::on_scope_changed() {
	repopulate();
}

void site_policy_dialog::on_feature_changed(int feature_index) {
	if (m_populating || feature_index < 0 || feature_index >= m_combos.size())
		return;
	const feature f = static_cast<feature>(feature_index);
	const setting s = index_to_setting(m_combos[feature_index]->currentIndex());

	if (m_scope->currentIndex() == 2) {
		// Global default: unset is not meaningful, treat it as allow. The
		// entry is greyed in this scope so it cannot normally be chosen; this
		// stays as the belt to that braces.
		m_engine->set_global_default(f, s == setting::unset ? setting::allow : s);
		// What every site's "Default" resolves to has just moved.
		refresh_default_labels();
	} else {
		// Belt and braces against the case above: a site rule with no site is
		// not a rule, and storing one is worse than refusing it because it
		// persists and matches nothing.
		const QString pattern = current_pattern();
		if (pattern.isEmpty())
			return;
		m_engine->set_setting(pattern, f, s);
	}
	emit policy_changed();
}
