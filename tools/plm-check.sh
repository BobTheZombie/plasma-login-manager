#!/bin/sh
# Read-only workstation preflight; never enables/stops display managers.
set -u
errors=0
check() {
    if "$@" >/dev/null 2>&1; then
        printf 'OK   %s\n' "$*"
    else
        printf 'FAIL %s\n' "$*" >&2
        errors=$((errors + 1))
    fi
}
check command -v kwin_wayland
check command -v startplasma-wayland
check command -v loginctl
check command -v systemd-sysusers
check test -e /dev/tty1
check test -e /dev/tty2
check getent passwd plm
check test -f /etc/pam.d/plm
check test -f /etc/pam.d/plm-greeter
check test -d /usr/lib/qt6/plugins/platforms
check loginctl show-seat seat0
printf 'Read-only checks complete. Failures: %s\n' "$errors"
test "$errors" -eq 0
