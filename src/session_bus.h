#pragma once

#include <QtGlobal>

// Is there a session bus to talk to?
//
// **Read the address, not the connection.** `QDBusConnection::sessionBus()`
// autolaunches a private `dbus-daemon --session` through libdbus when
// `DBUS_SESSION_BUS_ADDRESS` is unset -- which is every test run, every CI
// run and every tty login without a session bus -- and nothing reaps it.
//
// Every caller in this tree already had an abstain path, and every one of them
// read the ANSWER rather than the question: `theme::portal_scheme` constructed
// a `QDBusInterface` and returned -1 when `isValid()` was false, and
// `qtwebengine_notifications::install` took `sessionBus()` and returned null
// when `isConnected()` was false. Both behaved correctly, and both had already
// paid for the launch by the time they looked, because the launch happens
// inside the call whose answer they were reading.
//
// **Empty counts as absent**, which is why this is `IsEmpty` and not `IsSet`:
// the build clears the variable rather than unsetting it, make having no way
// to unset one for a child.
//
// **A caller cannot report whether it asked by calling this again.** A second
// read of the environment would still say "did not ask" with the gate deleted,
// and that is precisely the case such a report exists to catch -- so callers
// that expose an `asked` flag set it where the connection is about to be made,
// positionally, and never recompute it from here. raidcfgd found the cheaper
// thing first: asserting on the connection's `name()` fails, because a
// `QDBusConnection` named but never opened has no private data and returns
// empty, so the object discards exactly the fact a test wants.
namespace session_bus {

inline bool present() {
	return !qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS");
}

}  // namespace session_bus
