// Moving between pages: what the toolbar offers, and whether it is true.
//
// Back, Forward and Reload were permanently enabled, including on an empty tab
// where none of them did anything at all -- the same defect the Media button
// had, with a better cure available: a navigation button does not need a status
// message explaining why it did nothing, it needs to look unavailable.
//
// **Driven with real pages, from local files.** History is Chromium's, not
// ours, and the only way to find out whether `canGoBack` says what the toolbar
// claims is to navigate. Two `file://` documents build exactly the history a
// person builds by following a link.
//
// What the window *says* about a page lives in `try_chrome`; the tools that act
// on one live in `try_pagetools`.
#include "shell_fixture.h"

#include "node.h"
#include "policy.h"
#include "tab_tree_model.h"
#include <QAbstractButton>
#include <QAction>
#include "web_view_backend.h"

#include <QFile>
#include <QProgressBar>
#include <QSortFilterProxyModel>
#include <QApplication>
#include <QStatusBar>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-navigate");
	main_window &w   = f.window;
	QAction   *back  = f.back, *fwd = f.fwd, *reload = f.reload;
	QLineEdit *address = f.address;
	QTreeView *tv    = f.tv;
	policy_engine &policy = f.policy;
	const QString out = f.out, one = f.one, two = f.two;
	(void)fwd; (void)back; (void)reload; (void)tv; (void)policy;
	(void)one; (void)two;

	if (!back || !fwd || !reload || !address || !tv) {
		std::printf("the window did not come up as expected\n");
		return 1;
	}

	section("the three buttons exist and can be read");
	check(back && fwd && reload && address,
	      "Back, Forward, Reload and the address bar are all there");
	if (!back || !fwd || !reload || !address) {
		std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
		return 1;
	}

	section("with no page open");
	{
		// The case that started this. All three were enabled on an empty tab,
		// and pressing any of them did nothing whatsoever.
		check(!back->isEnabled(),   "Back is greyed");
		check(!fwd->isEnabled(),    "Forward is greyed");
		check(!reload->isEnabled(), "Reload is greyed -- there is nothing to reload");
	}

	check(f.open_tab(0, "one.html"), "the first page loads");
	section("on the first page of a tab");
	{
		settle(reload, true);
		check(reload->isEnabled(), "Reload works now that there is a page");
		check(!back->isEnabled(),  "Back is still greyed -- nothing precedes it");
		check(!fwd->isEnabled(),   "and so is Forward");
	}

	section("after going somewhere else");
	{
		address->setText(QUrl::fromLocalFile(two).toString());
		emit address->returnPressed();
		check(wait_for(address, "two.html"), "the second page loads");
		settle(back, true);
		check(back->isEnabled(), "Back comes alive");
		check(!fwd->isEnabled(), "Forward stays greyed -- nothing was undone yet");
	}

	section("after going back");
	{
		back->trigger();
		check(wait_for(address, "one.html"), "the first page returns");
		settle(fwd, true);
		check(fwd->isEnabled(),   "Forward comes alive");
		check(!back->isEnabled(), "and Back goes grey at the start of history");
	}

	section("the Reload button becomes Stop while a page is arriving");
	{
		// **Watched rather than sampled.** Even a local file goes through
		// loadStarted and loadFinished, so the button is Stop for a moment
		// too short to catch by looking; recording every change to the action
		// catches it however fast the page arrives.
		check(reload->text().contains("Reload"),
		      QString("it is Reload while nothing is loading (%1)").arg(reload->text()));

		QStringList seen;
		auto conn = QObject::connect(reload, &QAction::changed, reload,
		                              [&seen, reload] { seen << reload->text(); });
		address->setText(QUrl::fromLocalFile(two).toString());
		emit address->returnPressed();
		check(wait_for(address, "two.html"), "a page loads");
		spin(600);
		QObject::disconnect(conn);

		check(seen.filter("Stop").size() > 0,
		      QString("it offered Stop while the page was on its way (%1)")
		          .arg(seen.join(", ").left(48)));
		check(reload->text().contains("Reload"),
		      QString("and it is Reload again once the page arrived (%1)")
		          .arg(reload->text()));

		// Back to the first page, so the sections after this one still find
		// what they expect.
		back->trigger();
		wait_for(address, "one.html");
		spin(300);
	}

	section("a page asking for another window");
	{
		// **Unhandled is not the same as refused.** With nothing implementing
		// this, a target="_blank" link did nothing whatsoever -- and a blocked
		// popup looked exactly the same, which is how the gap survived beside
		// a popup setting that appeared to work.
		//
		// Driven directly: what is worth checking is the decision and where
		// the tab lands, and Qt's own signal is one line of wiring.
		auto *model = w.findChild<tab_tree_model *>();
		node *folder = model->root()->children.first();
		// The tab the driver opened at the start, which is the one that will
		// be asking: `open_new_window` parents onto whatever view is current.
		node *asking = folder->children.first();
		const int before = folder->children.size();
		const int under_asking = asking->children.size();

		// A click. Chromium's rule and the right one: the popup setting exists
		// to stop pages opening windows nobody asked for, not to break links.
		node *made = w.open_new_window(QUrl("https://example.test/clicked"), true);
		spin(400);
		check(made != nullptr, "a clicked link opens even with popups blocked");
		// **This used to assert the opposite, and its own message said so.**
		// It checked that the new tab arrived beside the asking one, under the
		// folder, while the message read "under the tab that asked, where the
		// tree shows the relationship" -- the same contradiction the shell
		// carried, where the comment described the design and the code
		// described a restriction. A tab can hold children now (sec 5.5), so the
		// relationship is recorded where both always said it should be.
		check(folder->children.size() == before,
		      QString("not beside the tab that asked (%1 -> %2)")
		          .arg(before).arg(folder->children.size()));
		check(asking->children.size() == under_asking + 1,
		      QString("but under it, as a sub-tab (%1 -> %2)")
		          .arg(under_asking).arg(asking->children.size()));
		if (made)
			check(made->parent == asking,
			      "under the tab that asked, where the tree shows the relation");

		// A script, with the default policy, which blocks popups.
		// Counted where the tabs now land, which is under the asking tab
		// rather than beside it.
		const int after_click = asking->children.size();
		QStatusBar *sb = w.findChild<QStatusBar *>();
		node *blocked = w.open_new_window(QUrl("https://example.test/popup"), false);
		spin(300);
		check(blocked == nullptr, "a script-opened window is refused");
		check(asking->children.size() == after_click,
		      "and leaves no tab behind");
		check(sb && sb->currentMessage().contains("Blocked"),
		      QString("out loud, not silently (%1)")
		          .arg(sb ? sb->currentMessage() : QString()));

		// The same request once the site is allowed popups.
		policy.set_setting("*", policy::feature::popups, policy::setting::allow);
		node *allowed = w.open_new_window(QUrl("https://example.test/allowed"), false);
		spin(300);
		check(allowed != nullptr, "allowing popups lets one through");
		policy.set_setting("*", policy::feature::popups, policy::setting::block);
	}

	section("a new tab is the tab the address bar is talking to");
	{
		// The reported defect, and both of its symptoms from one cause.
		// `add_tab` makes a row with no address -- that is what a new tab *is*
		// -- and `open_node` refused any node whose url was empty, so nothing
		// was ever shown for it and the stack went on showing the tab before.
		//
		// From there the tree gained a row that never loaded and so never
		// changed the title `add_tab` gave it, while `current_view()` -- which
		// reads the stack, not the tree -- still answered with the previous
		// tab. `new_tab` then focuses the address bar, inviting an address, and
		// `navigate_to_address` loaded it into whatever was showing. One defect,
		// wearing "the tab doesn't update its text" and "it opens the wrong
		// page" at the same time.
		auto *model = w.findChild<tab_tree_model *>();
		node *folder = model->root()->children.first();

		check(f.open_tab(0, "one.html"), "the first tab is showing page one");
		const int before = folder->children.size();

		// Through the menu entry, which is the way a person reaches it.
		QAction *new_tab_action = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (QString(a->text()).remove('&') == "New Tab")
				new_tab_action = a;
		check(new_tab_action != nullptr, "the menu offers New Tab");
		if (new_tab_action) {
			new_tab_action->trigger();
			spin(1500);
			check(folder->children.size() == before + 1,
			      QString("which adds a row beside the tab that was showing "
			               "(%1 -> %2)").arg(before)
			          .arg(folder->children.size()));
			node *fresh = folder->children.last();

			// The tree has to agree about which row is current, because that is
			// what the shell reads to decide where the *next* tab is filed.
			node *current = nullptr;
			if (tv->currentIndex().isValid()) {
				auto *proxy = qobject_cast<QSortFilterProxyModel *>(tv->model());
				const QModelIndex src = proxy
				  ? proxy->mapToSource(tv->currentIndex()) : tv->currentIndex();
				current = model->node_for_index(src);
			}
			check(current == fresh,
			      QString("and highlights it in the tree (%1)")
			          .arg(current ? current->title : QString("nothing")));

			// Now the gesture the address bar was just focused for.
			address->setText(QUrl::fromLocalFile(two).toString());
			emit address->returnPressed();
			check(wait_for(address, "two.html"),
			      "typing an address after New Tab loads it");
			f.wait_idle();

			// Loaded into the *new* tab. The old one is still on page one, and
			// under the defect it was the old one that moved.
			check(f.open_tab(0, "one.html"),
			      "and the tab that was showing before is still on page one");

			// The new row wears the page's title rather than the placeholder
			// `add_tab` gave it -- which is only possible because a page loaded
			// into it at all.
			check(fresh->title != "New tab",
			      QString("and the new row took the page's title (%1)")
			          .arg(fresh->title));
		}
	}

	section("switching back to a loading tab still says it is loading");
	{
		// `page_changed` resets the chrome for the page in front of you, and
		// it resets `m_loading` to false unconditionally -- then never asks
		// the tab it has just switched to whether it is, in fact, still
		// loading. Its comment records the direction that was fixed, "the bar
		// would otherwise report the last tab's load against this one", and
		// this is the other one: a tab that IS loading reports as idle, so
		// the progress bar is gone and the button offers Reload where the
		// only useful thing to do is Stop.
		//
		// The slow page needs no server and no DNS. 192.0.2.1 is TEST-NET-1,
		// reserved by RFC 5737 and routed nowhere, so the connection hangs
		// rather than being refused -- measured here at ten seconds with no
		// answer, against a `curl -m 10` that timed out rather than failing.
		//
		// **The main frame, not a subresource.** The first attempt was a
		// local page holding `<img src="http://192.0.2.1/x.png">`, and it
		// finished loading immediately: Chromium blocks an http subresource
		// of a `file://` document outright, so the image errored at once and
		// the load completed. Hanging the navigation itself is the version
		// that cannot be short-circuited by a policy.
		w.open_url(QUrl("http://192.0.2.1/slow.html"));
		check(wait_for(address, "192.0.2.1"),
		       "a page that will not finish is open");
		spin(800);
		// The premise. If this fails the rest says nothing: the page finished,
		// or never started, and there is no loading tab to switch away from.
		const bool still_loading = reload->text().contains("Stop");
		check(still_loading,
		       QString("and it is still loading (%1)").arg(reload->text()));

		if (still_loading) {
			check(f.open_tab(0, "one.html"), "switching away to another tab");
			check(reload->text().contains("Reload"),
			       QString("which is not loading, so the button is Reload (%1)")
			           .arg(reload->text()));

			// Back to it. The load has not finished -- nothing has been given
			// a chance to -- so the chrome has to say so again.
			auto *nav_model = w.findChild<tab_tree_model *>();
			node *slow_row = nav_model->root()->children.last();
			auto *proxy = qobject_cast<QSortFilterProxyModel *>(tv->model());
			emit tv->activated(proxy->mapFromSource(
			  nav_model->index_for_node(slow_row)));
			spin(800);
			check(reload->text().contains("Stop"),
			       QString("and coming back to the loading tab offers Stop "
			                "again (%1)").arg(reload->text()));
			QProgressBar *bar = w.findChild<QProgressBar *>("load_progress");
			check(bar && bar->isVisible(),
			       "with the progress bar back beside it");
			// Stop it, so the sections after this are not racing a load.
			reload->trigger();
			spin(200);
		}
	}

	section("a redirect does not eat what somebody is typing");
	{
		// A page that moves on its own -- a meta refresh, a JavaScript
		// redirect, a slow load that commits late -- emits `url_changed`
		// whenever it gets there, and `update_address` wrote that straight
		// into the field. If the person is in the middle of typing an address
		// when it lands, their text is gone mid-keystroke.
		//
		// Written by hand rather than through `write_page`, which builds the
		// whole document from its argument and uses it as the title too.
		const QString slow = f.out + "/slow.html";
		QFile sf(slow);
		if (sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			sf.write(QString("<!doctype html><html><head><title>slow</title>"
			                    "<meta http-equiv=\"refresh\" "
			                    "content=\"2;url=%1\"></head>"
			                    "<body><p>slow</p></body></html>\n")
			               .arg(QUrl::fromLocalFile(two).toString()).toUtf8());
			sf.close();
		}

		w.open_url(QUrl::fromLocalFile(slow));
		check(wait_for(address, "slow.html"), "a self-redirecting page is open");
		// The row `open_url` filed under the root, so the redirect can be
		// observed on the view itself rather than on the field under test.
		auto *nav_model = w.findChild<tab_tree_model *>();
		node *redirecting = nav_model->root()->children.last();

		// Typed, not assigned: `setText` clears the modified flag, and what
		// separates "somebody is editing this" from "the browser filled it in"
		// is exactly that flag.
		address->setFocus();
		address->clear();
		address->insert("example.or");
		check(address->text() == "example.or",
		       "with a half-typed address in the bar");

		// Long enough for the refresh to fire and commit.
		for (int waited = 0; waited < 6000 && !address->text().isEmpty() &&
		                      address->text() == "example.or"; waited += 200)
			spin(200);
		// It arrived, so the check below is about what the field kept rather
		// than about a redirect that never happened.
		web_view_backend *v =
		  w.m_views_by_id.value(redirecting->id, nullptr);
		const QString now = v ? v->url().toString() : QString();
		check(now.contains("two.html"),
		       QString("the redirect arrived (%1)").arg(now));
		check(address->text() == "example.or",
		       QString("and the half-typed address survived it (%1)")
		           .arg(address->text()));

		// **The other half, and it is what keeps the guard from stranding the
		// bar.** Leaving a half-typed address alone is only right while the
		// person is still on that page: moving to another tab is a different
		// address and a question they just asked, so the field has to catch
		// up. Without this the guard would be a hang -- the typed text would
		// sit there through every tab switch, describing nothing.
		check(f.open_tab(0, "one.html"),
		       "switching to another tab loads it");
		check(address->text().contains("one.html"),
		       QString("and the bar follows rather than keeping the typing "
		                "(%1)").arg(address->text()));
	}

	return report();
}
