#!/bin/sh
# Rapid-fire captures of one window, raw xwd (no encoding) so frames are cheap.
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
