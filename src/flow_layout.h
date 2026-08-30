//
// A row of widgets that becomes several rows when one will not fit.
//
// **Written for the phone, and it costs the desktop nothing.** The downloads
// dialog puts five action buttons and a Close in one `QHBoxLayout`. That layout
// has exactly one strategy when the space is short: squeeze every child. At 360
// logical pixels -- a small phone in portrait -- it produced six buttons at 51
// pixels against an 80-pixel label, so "Open Folder" was on screen reading "pen
// Folde". Technically reachable, which is why a check that only asked whether
// the buttons were inside the dialog passed it.
//
// `android_dialogs` cannot fix that. It hands the dialog the screen rectangle,
// which is the right instruction, but a widget cannot go below its layout's
// minimum -- and a horizontal row's minimum is the sum of its children. The
// downloads dialog would not go under 532 pixels wide whatever it was told.
//
// So the layout has to be one that can use a second line. At any width where
// the items already fit, this lays out exactly as a `QHBoxLayout` would, so no
// desktop window changes shape.
//
// This is the shape of Qt's own flow-layout example, which is the canonical
// answer to this and has been for twenty years; there is nothing to gain by
// inventing a different one. What is ours: the spacing comes from the style
// rather than being a hardcoded number, and `heightForWidth` is implemented,
// without which a dialog sizes itself as though the wrapping never happened.
#pragma once

#include <QLayout>
#include <QList>

class flow_layout : public QLayout {
	Q_OBJECT
public:
	explicit flow_layout(QWidget *parent = nullptr, int margin = -1,
	                      int h_spacing = -1, int v_spacing = -1);
	~flow_layout() override;

	// Horizontal and vertical gaps are asked separately because a style gives
	// different answers for them, and a single number looks wrong in one axis.
	int horizontal_spacing() const;
	int vertical_spacing() const;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;

	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	QSize sizeHint() const override;
	// **The widest single item, not the sum.** This is the whole point: a
	// horizontal row reports the sum here, which is what kept the downloads
	// dialog 532 pixels wide.
	QSize minimumSize() const override;
	void setGeometry(const QRect &rect) override;

private:
	// One pass over the items. With `apply` false it measures and moves
	// nothing, which is what `heightForWidth` needs; the two must agree, so
	// they are the same code rather than two that look alike.
	int lay_out(const QRect &rect, bool apply) const;
	int spacing_from_style(QSizePolicy::ControlType type, bool horizontal) const;

	QList<QLayoutItem *> m_items;
	int m_h_spacing = -1;
	int m_v_spacing = -1;
};
