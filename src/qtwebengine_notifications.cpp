#include "qtwebengine_notifications.h"

#include <QWebEngineNotification>
#include <QWebEngineProfile>

/// @pkg_optional Qt6DBus defines HYDRA_HAVE_DBUS
#ifdef HYDRA_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QStringList>
#include <QVariantMap>
#endif

namespace {

#ifdef HYDRA_HAVE_DBUS
const char *k_service = "org.freedesktop.Notifications";
const char *k_path    = "/org/freedesktop/Notifications";
const char *k_iface   = "org.freedesktop.Notifications";
#endif

}  // namespace

qtwebengine_notifications::qtwebengine_notifications(QObject *parent)
    : QObject(parent) {}

qtwebengine_notifications *
qtwebengine_notifications::install(QWebEngineProfile *profile) {
	if (!profile)
		return nullptr;
#ifndef HYDRA_HAVE_DBUS
	// Built without QtDBus. Nothing to present with, and saying so by returning
	// null is the whole contract -- the caller leaves notifications blocked.
	return nullptr;
#else
	QDBusConnection bus = QDBusConnection::sessionBus();
	if (!bus.isConnected())
		return nullptr;

	// **The bus daemon is asked whether the name exists, and the service itself
	// is not called.**
	//
	// The first version called `GetServerInformation` on the service, reasoning
	// that a registered name is not the same as something that will answer.
	// True, and the wrong trade twice over. It is a *blocking* call on the
	// startup path, so a slow or wedged notification daemon would hold the
	// browser's launch for the timeout; and a blocking call cannot be answered
	// by a service living in this same thread, which is exactly how a test
	// stands one up -- `try_notify` failed at this line before it could test
	// anything, and the failure was the probe rather than the presenter.
	//
	// What makes the weaker check sufficient is that the strong one already
	// happens later and cannot be skipped: `present()` checks the `Notify`
	// reply, and a name that nothing answers on fails there and closes the
	// notification. So the cost of trusting the name is one notification lost
	// on a broken desktop, against a browser that will not start on a slow one.
	//
	// Activatable names count. A notification daemon that is started on demand
	// is not registered until something sends to it, and refusing to install a
	// presenter for one would leave notifications dead on exactly the desktops
	// that do it properly.
	QDBusConnectionInterface *iface = bus.interface();
	if (!iface)
		return nullptr;
	const bool present_now = iface->isServiceRegistered(QString::fromLatin1(k_service));
	const bool activatable =
	  iface->activatableServiceNames().value().contains(QString::fromLatin1(k_service));
	if (!present_now && !activatable)
		return nullptr;

	auto *self = new qtwebengine_notifications(profile);
	bus.connect(k_service, k_path, k_iface, QStringLiteral("ActionInvoked"),
	             self, SLOT(on_action_invoked(uint, QString)));
	bus.connect(k_service, k_path, k_iface, QStringLiteral("NotificationClosed"),
	             self, SLOT(on_closed(uint, uint)));

	profile->setNotificationPresenter(
	  [self](std::unique_ptr<QWebEngineNotification> n) {
		self->present(std::move(n));
	});
	return self;
#endif
}

void qtwebengine_notifications::present(
  std::unique_ptr<QWebEngineNotification> n) {
	if (!n)
		return;
#ifndef HYDRA_HAVE_DBUS
	// Unreachable: nothing installs a presenter without the bus. Closed rather
	// than dropped anyway, so the page is told, because a notification that is
	// neither shown nor closed leaves the page believing it is still up.
	n->close();
#else
	// **Shared, because two things outlive this function and one of them is
	// asynchronous.** The lambda that presents is done here; the object has to
	// survive until the service reports a click or a close, which arrives on a
	// signal much later.
	std::shared_ptr<QWebEngineNotification> held(std::move(n));

	// Named so the notification is attributed to this application by servers
	// that group or theme by desktop entry.
	//
	// **The site's own icon is deliberately not sent.** `icon()` gives a QImage
	// and the hint that carries one is a marshalled `(iiibiiay)` struct; that is
	// a page-supplied image travelling into a system service, and it is not
	// worth writing that path for decoration. The title already names the site.
	//
	// Built here rather than braced into the argument list below, where the
	// nested initialiser made the whole call read as three levels deep to
	// anything counting braces -- the style gate among them.
	QVariantMap hints;
	hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("hydra"));

	QDBusMessage call = QDBusMessage::createMethodCall(
	  k_service, k_path, k_iface, QStringLiteral("Notify"));
	call << QStringLiteral("Hydra")     // app_name
	     << uint(0)                     // replaces_id: each is its own
	     << QStringLiteral("hydra")     // app_icon, by name
	     << held->title()
	     << held->message()
	     // "default" is the freedesktop convention for "the body was clicked",
	     // which is what a web notification means by activation. The visible
	     // label beside it is only shown by servers that draw buttons.
	     << QStringList{QStringLiteral("default"), QStringLiteral("Open")}
	     << hints
	     << int(-1);                    // expire_timeout: the server's default

	// **Asynchronous, and that is not an optimisation.** This runs on the GUI
	// thread, called from the engine while a page is doing something; a blocking
	// call here hands the browser's responsiveness to whatever notification
	// daemon the desktop happens to run, for as long as it takes to answer.
	// Every notification, not just the slow ones.
	//
	// It also makes the thing testable. A blocking call cannot be answered by a
	// service in the same thread, so a stub daemon stood up inside a driver
	// times out rather than replying -- which is exactly what happened, and it
	// looked like the presenter silently not working, because that is what it
	// is indistinguishable from.
	auto *watcher = new QDBusPendingCallWatcher(
	  QDBusConnection::sessionBus().asyncCall(call, 5000), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this,
	         [this, held](QDBusPendingCallWatcher *w) {
		w->deleteLater();
		const QDBusPendingReply<uint> reply = *w;
		// What the service said, on request. Without it a failed notification and
		// a notification nobody looked at are the same observation: the page is
		// told `close` either way. Off by default because the arguments carry a
		// page's title and body, and those are somebody's business.
		if (reply.isError() && qEnvironmentVariableIsSet("HYDRA_NOTIFY_DEBUG"))
			qWarning("notify: %s: %s", qPrintable(reply.error().name()),
			          qPrintable(reply.error().message()));
		if (reply.isError()) {
			// The service was there when the presenter was installed and is not
			// answering now. The page is told it is closed rather than left
			// waiting -- the same principle the permission decider follows, that
			// an unanswered request is worse than a refused one.
			held->close();
			return;
		}
		m_live.insert(reply.value(), held);
		// Only after the service has taken it. `show()` is the page's
		// confirmation that its notification is on screen, and saying so before
		// the call could have failed would be a lie in the one direction that
		// matters.
		held->show();
	});
#endif
}

void qtwebengine_notifications::on_action_invoked(uint id,
                                                   const QString &action) {
	Q_UNUSED(action)
	const auto it = m_live.constFind(id);
	if (it == m_live.constEnd())
		return;   // another application's notification, on a broadcast signal
	(*it)->click();
}

void qtwebengine_notifications::on_closed(uint id, uint reason) {
	Q_UNUSED(reason)
	const auto it = m_live.find(id);
	if (it == m_live.end())
		return;
	// Told before it is dropped: `close()` is what emits the page's `onclose`,
	// and the object has to still exist to emit it.
	(*it)->close();
	m_live.erase(it);
}
