#!/bin/sh
# Rapid-fire captures of one window, raw xwd (no encoding) so frames are cheap.
#
# **xwd, and never ImageMagick's `import`.** `import` grabs the X pointer, and
# when it cannot do what it was asked -- a window id it will not accept, say --
# it falls back to interactive selection and *keeps the grab* while it waits for
# a click. The whole desktop stops responding: no window manager, no keyboard,
# nothing but a crosshair cursor that moves. Every other X client is blocked
# until the grab is released, which includes whatever was meant to break it.
#
# That is not a hypothetical. It was done to this machine from a one-off command
# while capturing a Hydra window, and it froze the session.
#
# `xwd -id` reads the window's contents and grabs nothing. Inside a Qt driver,
# `QWidget::grab()` is better still: it renders the widget in-process and never
# touches the server. Neither can take a desktop down.
out=$1; n=$2; delay=$3
id=$(xwininfo -root -tree 2>/dev/null | grep -m1 '"Hydra"' | awk '{print $1}')
[ -z "$id" ] && { echo "no Hydra window"; exit 1; }
echo "window $id"
i=0
while [ $i -lt "$n" ]; do
  xwd -id "$id" -silent > "$out/w$(printf %03d $i).xwd" 2>/dev/null
  i=$((i+1))
  sleep "$delay"
done
echo "captured $n"
