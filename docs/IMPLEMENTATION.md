# Architecture and threat model

## Trust boundaries

1. plm-greeter runs as the unprivileged service user plm.
2. The greeter connects to /run/plm/control.sock, an AF_UNIX socket that
   plmd creates with mode 0600 and ownership plm:plm. The containing
   directory belongs to root and cannot be replaced by the greeter.
3. plmd is the only privileged long-lived manager. Every connection is
   checked with SO_PEERCRED, and every JSON request is length-limited.
4. Credentials are passed to a single-use root PAM worker on an anonymous
   stdin pipe. No password is exported through system D-Bus.
5. The worker authenticates with Linux-PAM and opens a pam_systemd session
   with XDG_SESSION_TYPE=wayland, XDG_SEAT=seat0 and XDG_VTNR.
6. Session child drops supplementary groups, GID and UID before exec.
   The root worker stays around to close PAM after the session exits.
7. KWin owns DRM/KMS input and outputs, reacting to hotplug and scale;
   plm does not directly open /dev/dri in its root supervisor.

## Boot and lifecycle

- systemd -> plmd root supervisor
- plmd -> greeter worker (PAM, class=greeter) -> KWin Wayland on VT1
- KWin -> plm-greeter QML on all Qt-identified monitors
- plm-greeter -> private socket -> plmd -> user worker
- user worker -> PAM auth/account/cred/open -> Plasma Wayland on VT2
- plmd -> logind ActivateSession(user)
- user exits -> PAM close, credentials teardown -> ActivateSession(greeter)

Only a fixed Plasma Wayland executable is accepted; there is no
user-provided path or shell command executed by the privileged helper.

## Threat model and gaps

This v0.1 preview is not yet production-grade security software.
The JSON password travels over a private local socket and is briefly
copied by Qt and QML, with best-effort clearing of selected buffers.
Those copies need an audited, binary challenge-response mechanism before
production. PAM now supports multiple challenge-response prompts and expired-password
changes, but the security properties of each real PAM module need testing.
Seat0 and fixed VTs are not enough for real multi-seat hardware.
Other gaps: credential rotation; retry/lockout persistence; session
ownership after supervisor crash; screen-reader integration; keyboard
layouts and input methods; smartcards/FIDO2; user account discovery;
wake and suspend handling; GPU reset/recovery; screencast protection.

## Performance targets

Avoid a custom compositor; reuse Plasma's KWin.
Keep plmd event-driven and asleep between events.
Use a single-shot PAM worker for each login.
Do not block the greeter UI thread during authentication.
Keep QML gradients static, avoid frame-by-frame animations.
Measure on hardware instead of making unverified startup claims.

Suggested benchmarks:
- boot critical chain: systemd-analyze critical-chain plmd.service
- greeter startup: journalctl -b -u plmd.service -o short-monotonic
- memory/CPU: systemd-cgtop, ps -eo pid,cmd,rss
- KWin and output handoff: loginctl show-session and journalctl -b -t kwin_wayland

## Staged deployment gates

1. Meson build and unit tests pass under a disposable VM.
2. Security review of PAM and credential handling (including MFA).
3. Validate normal login/logout, failed login, suspend/resume,
   TTY switching, hotplug and multi-monitor scale.
4. Verify no file descriptor, privilege, DRM or session leaks.
5. Add watchdog and safe rescue fallback.
6. Only then consider systemd display-manager alias and SDDM replacement.
