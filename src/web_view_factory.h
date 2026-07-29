// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class QWidget;
class web_view_backend;

// Makes views, and owns whatever profile-wide machinery an engine needs behind
// them — on desktop that is the shared QWebEngineProfile with the request
// interceptor and cookie filter installed on it (architecture doc §6/§7.3).
//
// The shell holds only this interface, so the concrete backend is named in
// exactly one place: main(). That is what keeps §19.2's rule enforceable rather
// than merely intended.
class web_view_factory {
public:
	virtual ~web_view_factory() = default;

	virtual web_view_backend *create_view(QWidget *parent) = 0;
};
