# Switching from SDDM to Plasma-login-manager (experimental)

**Do this in a VM first.** A passing CI build does not prove PAM, KWin DRM,
logind seat handling, and VT handoff on your device. This service can leave
you with a blank login screen. Do not uninstall SDDM until the new manager
has survived multiple login/logout and reboot cycles.

## Requirements

- System uses systemd, systemd-logind, Linux-PAM and a working seat0.
- KWin supports Wayland DRM and --exit-with-session; Plasma 6 supports
  startplasma-wayland.
- The installed PAM stack in /etc/pam.d/system-login opens pam_systemd.
- plm account created by systemd-sysusers and runtime directory populated.
- Qt 6 Wayland QPA plugin and QtQuick.Controls QML modules available.
- A recovery login on tty3 (Ctrl+Alt+F3) and console credentials.

## Install without enabling

    meson setup build --prefix=/usr --libdir=lib --libexecdir=libexec --buildtype=release
    meson compile -C build
    sudo meson install -C build
    sudo systemd-sysusers /usr/lib/sysusers.d/plm.conf
    sudo systemd-tmpfiles --create /usr/lib/tmpfiles.d/plm.conf
    sudo systemctl daemon-reload
    sudo sh tools/plm-check.sh

Before first test, verify /etc/pam.d/plm includes the correct distro PAM
files. Never overwrite a working PAM policy blindly.

## Test in a VM, before replacing anything permanently

Keep tty3 or a serial console active. From that recovery console:

    sudo systemctl stop sddm.service
    sudo systemctl start plmd.service
    sudo systemctl status plmd.service
    loginctl list-sessions

Perform correct and incorrect password attempts, logout, GPU/VT switching,
resume and multi-monitor tests. If the greeter fails or screen remains blank:

    sudo systemctl stop plmd.service
    sudo systemctl start sddm.service

## Opt-in systemd replacement after successful testing

The service declares Alias=display-manager.service, which systemd creates
on **enable**, not on package installation. Do not enable both managers:

    sudo systemctl disable --now sddm.service
    sudo systemctl enable --now plmd.service

Confirm:

    readlink -f /etc/systemd/system/display-manager.service
    systemctl status display-manager.service
    journalctl -b -u plmd.service --no-pager
    loginctl list-sessions

If a display-manager.service alias conflict is reported, inspect and
disable the old display-manager using systemctl. Do not forcibly delete
random systemd unit links.

## Rollback (from tty3 or remote console)

    sudo systemctl disable --now plmd.service
    sudo systemctl enable --now sddm.service
    sudo systemctl set-default graphical.target
    sudo reboot

When testing on unstable hardware, start in multi-user.target and bring
up the display manager manually. To recover from repeated failed boots,
temporarily boot with systemd.unit=multi-user.target on the kernel command
line.

## Still required for production readiness

The implementation only manages seat0 with VT1 greeter and VT2 Plasma.
Real multi-seat support, live smartcard/fingerprint/OTP PAM integration,
fail-safe boot recovery, screenreader/virtual keyboard support and a broad
device regression matrix remain unfinished. **Do not remove SDDM yet.**
