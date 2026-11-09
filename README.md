# Plasma-login-manager — LPM Linux edition

Experimental **Wayland-only Plasma 6 display-manager prototype**.

This is an independent LPM-specific implementation, not KDE's official Plasma Login Manager (https://invent.kde.org/plasma/plasma-login-manager).

## Architecture

- plmd: root supervisor, seat0, local credential transport and logind D-Bus
- plm-session: isolated PAM session helper, starts KWin greeter or Plasma user session
- plm-greeter: unprivileged Qt 6 Quick UI, KScreenLocker-inspired visual design
- KWin Wayland DRM/KMS manages monitors, hotplug, input and compositing
- meson/ninja builds; systemd service; sysusers, tmpfiles, PAM profiles

## Warning

**DO NOT REPLACE SDDM ON BARE METAL WITH THIS CODE YET.** Only seat0, VT1
greeter and VT2 user sessions are implemented. This has not undergone real
boot testing, secure authentication audit, fast-user-switch testing,
hotplug testing, MFA/PAM conversation support, watchdog recovery,
display teardown checks or crash recovery.

Use a VM with a known-good console fallback. Installing does not enable
the service or claim the display-manager.service systemd alias.

## Build prerequisites

Qt 6: Core, Network, DBus, Gui, Qml, Quick, QuickControls2.
Also PAM dev headers, libsystemd, KWin Wayland, Plasma 6, Meson and Ninja.

    meson setup build --prefix=/usr --libdir=lib
    meson compile -C build
    sudo meson install -C build
    sudo systemd-sysusers
    sudo systemd-tmpfiles --create

PAM files must be adapted to your distro's system-login PAM stack.
The plm user and KWin/Wayland session must be checked before launching.

## Development VM smoke test

From a separate rescue TTY or serial console, with SDDM stopped:

    sudo systemctl start plmd.service
    journalctl -fu plmd.service
    loginctl list-sessions

To recover: stop plmd and start sddm.service. Do not enable both.
Never remove getty on a separate recovery TTY.

## Security and current limits

Greeter <-> plmd uses a Unix socket in /run/plm, accessible only to the
dedicated plm UID. plmd verifies SO_PEERCRED. Passwords are supplied to
a PAM helper via anonymous stdin pipe, never passed through D-Bus, argv,
environment variables or a shell. Authentication requests are bounded,
per seat and minimally rate-limited.

MFA, password changes and multi-message PAM challenges are not supported
yet. Input-method support, HDR, accessibility, dynamic multi-seat setup,
remote access, comprehensive display configuration and systemd crash
handover are future work. Do not treat this as production ready.

SPDX-License-Identifier: GPL-2.0-or-later.

A session worker launches desktop processes with ordinary user privileges; the
systemd service intentionally avoids NoNewPrivileges, PrivateTmp and broad
mount/network restrictions inherited by its children. Production hardening
requires moving the session worker into separate scoped units.
