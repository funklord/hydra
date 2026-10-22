#pragma once

#include <QString>
#include <QStringList>

class QWidget;

// The app's half of `make android-screenshots` (`tool/android.mk`).
//
// The target starts the app once per view with a `store_view` extra, and
// what it captures is published in the Play listing. So a store view is
// never the person's own tree, history or downloads: `prepare_profile`
// points every app-data location at Qt's test-mode directory, empties it,
// and seeds it with the demo set in `android/assets/store/` -- a tree of
// tabs on `https://hydra.example/`, which `HydraWebView` serves from the
// package, and a download history. The same files every run, so two runs
// give the same pictures.
//
// Each view logs `store-view-ready <name>` once it is drawn; the target
// waits for that line rather than sleeping. `main_window::show_store_view`
// opens them.
namespace store_view {

// The views there are: the listing's four, in listing order, which the
// Makefile's `STORE_VIEWS` names, and `settings`, for looking at that
// dialog on a phone.
QStringList names();

// The demo site's host, served from the package by `HydraWebView`.
// `.example` is reserved and resolves nowhere, so nothing real is shadowed.
QString host();

// Move app data, settings and cache to the test-mode directories, empty
// them, and seed them from `seed_dir` (`assets:/store` in the package).
// Before anything reads a path: the settings and the tree are read early.
// False, with `why`, when the seed could not be laid down; the caller must
// then not show anything, since what it would show is not the demo.
bool prepare_profile(const QString &seed_dir, QString *why);

// Log `store-view-ready <name>` once `w` is on screen and has been painted.
void announce_when_drawn(QWidget *w, const QString &name);

}  // namespace store_view
