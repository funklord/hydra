#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QWebEngineNotification;
class QWebEngineProfile;

// Web notifications, actually shown.
//
// **Nothing presented them, and the failure was silent in the worst way.**
// Chromium hands a granted notification to whatever the profile's presenter is,
// and an absent presenter is not an error: the page's `new Notification(...)`
// succeeds, its promise resolves, and the notification goes nowhere. So a site
// could be granted the permission, believe it had it, and put nothing on the
// screen -- while the settings page showed the capability as allowed.
//
// That is why `notifications` was the one capability the audit left on `block`
// when camera, microphone and location moved to `ask`: prompting somebody to
// grant a power the browser cannot deliver is worse than refusing it, because
// the refusal is at least true. This is the other half of that finding.
//
// Freedesktop's `org.freedesktop.Notifications` is the mechanism, which is the
// same bus this project already talks to for the colour scheme. **Its absence
// is a supported outcome, not a failure**: `install` returns null and the caller
// leaves the default at block, so a machine with no notification service ends up
// exactly where it was rather than with a prompt that leads nowhere.
class qtwebengine_notifications : public QObject {
	Q_OBJECT
public:
	// Installs itself as the profile's presenter and returns it, parented to the
	// profile. Returns null -- having installed nothing -- when there is no
	// working notification service on the session bus.
	//
	// The check is a real call to the service rather than a name lookup: a name
	// can be registered by something that will not answer, and the question here
	// is whether a notification will actually be shown.
	static qtwebengine_notifications *install(QWebEngineProfile *profile);

private slots:
	// The service's own two signals. They are broadcast to every listener with
	// an id that means nothing to us unless we sent it, so both begin by asking
	// whether the id is one of ours; a browser that treated another
	// application's dismissal as its own would tell a page its notification had
	// been closed when it had not.
	void on_action_invoked(uint id, const QString &action);
	void on_closed(uint id, uint reason);

private:
	explicit qtwebengine_notifications(QObject *parent);
	void present(std::unique_ptr<QWebEngineNotification> n);

	// Takes a notification off the screen because the *page* withdrew it.
	//
	// **This was missing and the notification would have stayed up for ever.**
	// A page closing its own notification -- a chat marking a message read, a
	// countdown that has finished -- emits `closed()` and nothing was listening,
	// so the service was never told and the thing sat on the desktop after the
	// page had taken it back. The opposite direction, the service telling us,
	// was handled from the start; this is the same fact travelling the other way.
	void withdraw(uint id);

	// Held until the service says the notification is gone. **Not a cache**: the
	// object owns the page's end of the notification, and destroying it early is
	// what would make a click or a dismissal unreportable. The service always
	// sends `NotificationClosed` eventually, including when it expires on its
	// own, so nothing accumulates here for the life of the process.
	QHash<uint, std::shared_ptr<QWebEngineNotification>> m_live;
};
