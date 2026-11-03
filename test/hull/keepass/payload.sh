#!/bin/sh
# Inside the machine: a throwaway vault, a KeePassXC serving it, and
# try_keepass against that. What stops it: every wait below is bounded, and the
# exit trap kills the two background programs.
set -eu

# **A session bus and a Secret Service of the machine's own**, which is where
# the bridge keeps a pairing. Re-entered under dbus-run-session once, so
# everything below shares the bus; gnome-keyring unlocks with a throwaway
# password. D-Bus wants a machine id, and the overlay is writable.
if [ -z "${HYDRA_IN_SESSION:-}" ]; then
	dbus-uuidgen --ensure=/etc/machine-id
	export HYDRA_IN_SESSION=1
	exec dbus-run-session -- sh "$0" "$@"
fi

export HOME=/tmp/home XDG_RUNTIME_DIR=/tmp/run DISPLAY=:99
mkdir -p "$HOME/.config/keepassxc" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
eval "$(printf '%s' throwaway-keyring | gnome-keyring-daemon --unlock --components=secrets)"
export GNOME_KEYRING_CONTROL

Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
xvfb=$!
kp=
gen=
trap 'echo "--- keepassxc log:"; cat /tmp/keepassxc.log 2>/dev/null; kill $kp $xvfb $gen 2>/dev/null || true' EXIT

# A fixture, not a secret: the database exists for this run and dies with it.
pw=throwaway-hull-vault
db=/tmp/vault.kdbx
printf '%s\n%s\n' "$pw" "$pw" | keepassxc-cli db-create -q -p "$db"
printf '%s\n%s\n%s\n' "$pw" "fixture-password" "fixture-password" |
	keepassxc-cli add -q -u alice --url http://127.0.0.1:9931 -p "$db" fixture
echo "vault: $(printf '%s\n' "$pw" | keepassxc-cli ls -q "$db" | tr '\n' ' ')"

# Browser integration on, and access to entries allowed once paired: the
# pairing is the consent this test exercises; a second dialog per lookup is
# a setting, and the vault is ours.
cat >"$HOME/.config/keepassxc/keepassxc.ini" <<INI
[General]
ConfigVersion=2
UpdateCheckMessageShown=true

[GUI]
CheckForUpdates=false

[Browser]
Enabled=true
AlwaysAllowAccess=true
AlwaysAllowUpdate=true
INI

printf '%s\n' "$pw" | keepassxc --pw-stdin "$db" >/tmp/keepassxc.log 2>&1 &
kp=$!
sock="$XDG_RUNTIME_DIR/org.keepassxc.KeePassXC.BrowserServer"
i=0
while [ ! -S "$sock" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ -S "$sock" ] || { echo "KeePassXC never opened $sock"; cat /tmp/keepassxc.log; exit 1; }
echo "keepassxc: $(keepassxc --version 2>/dev/null | head -1), socket up"
sleep 3
echo "windows on the display:"
for w in $(xdotool search --name '.' 2>/dev/null); do
	n=$(xdotool getwindowname "$w" 2>/dev/null || true)
	[ -n "$n" ] && echo "  - $n"
done

# The one dialog a pairing raises: name the connection, and accept.
(
	i=0
	while [ $i -lt 150 ]; do
		w=$(xdotool search --name 'association request' 2>/dev/null | head -1 || true)
		if [ -n "$w" ]; then
			xdotool windowactivate --sync "$w" 2>/dev/null || true
			xdotool type --delay 20 "hydra-hull-test"
			xdotool key Return
			echo "association dialog answered"
			exit 0
		fi
		if [ $((i % 10)) -eq 5 ]; then
			echo "  waiting for the dialog; windows now:"
			for w in $(xdotool search --name '.' 2>/dev/null); do
				n=$(xdotool getwindowname "$w" 2>/dev/null || true)
				[ -n "$n" ] && echo "    - $n"
			done
		fi
		sleep 1
		i=$((i + 1))
	done
	echo "no association dialog appeared"
) &

HYDRA_KEEPASS_INTERACTIVE=1 HYDRA_KEEPASS_WRITE=1 QT_QPA_PLATFORM=offscreen \
	timeout 300 /mnt/repo/test/build-make/try_keepass

# **And again, with nobody to answer a dialog**: the pairing the first run
# stored must be accepted as it is, which is the claim "the next run needs no
# dialog" made by a run rather than read off a keyring.
echo
echo "=== second run: the stored pairing, no dialog ==="
QT_QPA_PLATFORM=offscreen timeout 120 /mnt/repo/test/build-make/try_keepass >/tmp/second.log 2>&1 || true
cat /tmp/second.log
grep -q 'the stored pairing was accepted' /tmp/second.log || {
	echo "FAIL  the second run did not take the stored pairing"; exit 1; }
grep -q '^0 failed\|, 0 failed' /tmp/second.log || {
	echo "FAIL  the second run failed checks"; exit 1; }
echo "ok    the second run took the stored pairing, and nobody was asked"

# **And the browser itself against the vault**: the shell, a real page, the
# key, a generated password and a save from a submitted form. Same keyring
# item as try_keepass, so it inherits that pairing and asks nobody.
echo
echo "=== the shell against the vault ==="
# **KeePassXC's generator is a window on its side**, and from 2.7 it replies
# to generate-password only when somebody presses Apply Password there
# (Ctrl+Return is its shortcut). So a second hand, for that window, for as
# long as the shell runs: bounded by its own count, and killed by the trap.
(
	i=0
	while [ $i -lt 300 ]; do
		w=$(xdotool search --name '^Generate Password$' 2>/dev/null | head -1 || true)
		if [ -n "$w" ]; then
			xdotool windowactivate --sync "$w" 2>/dev/null || true
			xdotool key ctrl+Return
			echo "  (rig) applied KeePassXC's generated password"
			sleep 2
		fi
		sleep 1
		i=$((i + 1))
	done
) &
gen=$!
HYDRA_VAULT=1 HYDRA_SECRET_KIND=keepassxc-association-try-keepass \
	QT_QPA_PLATFORM=offscreen QTWEBENGINE_DISABLE_SANDBOX=1 \
	HYDRA_TEST_OUT=/tmp/autofill \
	timeout 300 /mnt/repo/test/build-make/try_autofill || shell=$?
kill $gen 2>/dev/null || true
[ "${shell:-0}" -eq 0 ] || { echo "FAIL  try_autofill exited ${shell}"; exit 1; }
# Read back by the vault's own tool, not by the bridge that wrote it.
printf '%s\n' "$pw" | keepassxc-cli search -q "$db" 127.0.0.1:9934 | grep -q . || {
	echo "FAIL  keepassxc-cli does not find the saved login"; exit 1; }
echo "ok    keepassxc-cli finds the login the shell saved"
