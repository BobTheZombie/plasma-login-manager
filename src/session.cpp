// SPDX-License-Identifier: GPL-2.0-or-later
// Root-only PAM worker. Each worker supervises exactly one logind session.
#include <security/pam_appl.h>
#include <systemd/sd-login.h>
#include <pwd.h>
#include <grp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <algorithm>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Credentials {
    std::vector<char> password;
};

static void clean(Credentials &c)
{
    if (!c.password.empty()) {
        explicit_bzero(c.password.data(), c.password.size());
        c.password.clear();
    }
}

static volatile sig_atomic_t interrupted = 0;

static void terminate(int) { interrupted = 1; }

static bool allowedPamEnvironment(const std::string &key)
{
    if (key == "LANG" || key == "LANGUAGE" || key == "TZ" ||
        key == "KRB5CCNAME" || key == "XAUTHORITY" ||
        key == "XDG_CONFIG_HOME" || key == "XDG_DATA_HOME" ||
        key == "XDG_CACHE_HOME" || key == "XDG_STATE_HOME") return true;
    return key.rfind("LC_", 0) == 0;
}

static std::vector<std::string> sessionEnvironment(pam_handle_t *pam)
{
    std::vector<std::string> result;
    char **entries = pam_getenvlist(pam);
    if (!entries) return result;
    for (size_t i = 0; entries[i]; ++i) {
        const std::string env = entries[i];
        const size_t equals = env.find('=');
        if (equals != std::string::npos &&
            allowedPamEnvironment(env.substr(0, equals)) &&
            env.size() <= 4096 && result.size() < 64)
            result.push_back(env);
        free(entries[i]);
    }
    free(entries);
    return result;
}

static bool readExact(int fd, void *where, size_t length)
{
    auto *out = static_cast<unsigned char *>(where);
    while (length) {
        if (interrupted) return false;
        const ssize_t n = read(fd, out, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        out += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

static void sendPamText(const char *type, int echo, const char *message)
{
    // Hex framing prevents embedded newline / control sequences escaping
    // into the worker-to-supervisor line protocol.
    std::string hex;
    const size_t n = strnlen(message ? message : "", 512);
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char b = static_cast<unsigned char>(message[i]);
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    fprintf(stdout, "%s=%d:%s\n", type, echo, hex.c_str());
    fflush(stdout);
}

static bool receivePamReply(std::vector<char> &secret)
{
    uint32_t wireSize = 0;
    if (!readExact(STDIN_FILENO, &wireSize, 4)) return false;
    const uint32_t n = ntohl(wireSize);
    if (n == 0 || n > 1024) return false;
    secret.resize(n + 1, '\0');
    if (!readExact(STDIN_FILENO, secret.data(), n)) {
        explicit_bzero(secret.data(), secret.size());
        secret.clear();
        return false;
    }
    if (memchr(secret.data(), '\0', n)) {
        explicit_bzero(secret.data(), secret.size());
        secret.clear();
        return false;
    }
    return true;
}

static int conversation(int count, const pam_message **msgs,
                        pam_response **responses, void *ptr)
{
    if (count < 1 || count > 16 || !msgs || !responses) return PAM_CONV_ERR;
    auto *credentials = static_cast<Credentials *>(ptr);
    auto *answer = static_cast<pam_response *>(calloc(count, sizeof(pam_response)));
    if (!answer) return PAM_BUF_ERR;
    for (int i = 0; i < count; ++i) {
        if (!msgs[i]) goto invalid;
        const auto style = msgs[i]->msg_style;
        if (style == PAM_PROMPT_ECHO_OFF || style == PAM_PROMPT_ECHO_ON) {
            std::vector<char> reply;
            if (style == PAM_PROMPT_ECHO_OFF && !credentials->password.empty()) {
                reply.swap(credentials->password);
            } else {
                sendPamText("PROMPT", style == PAM_PROMPT_ECHO_ON ? 1 : 0,
                            msgs[i]->msg);
                if (!receivePamReply(reply)) goto invalid;
            }
            answer[i].resp = strdup(reply.data());
            explicit_bzero(reply.data(), reply.size());
            reply.clear();
            if (!answer[i].resp) goto invalid;
        } else if (style == PAM_TEXT_INFO || style == PAM_ERROR_MSG) {
            sendPamText("NOTICE", style == PAM_ERROR_MSG ? 1 : 0, msgs[i]->msg);
        } else {
            goto invalid;
        }
    }
    *responses = answer;
    return PAM_SUCCESS;
invalid:
    for (int i = 0; i < count; ++i) {
        if (answer[i].resp) {
            explicit_bzero(answer[i].resp, strlen(answer[i].resp));
            free(answer[i].resp);
        }
    }
    free(answer);
    return PAM_CONV_ERR;
}

static bool isName(const std::string &name)
{
    if (name.empty() || name.size() > 32) return false;
    if (!((name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
               || c == '-' || c == '_')) return false;
    return true;
}

static bool putenvPam(pam_handle_t *pam, const char *key, const std::string &value)
{
    return pam_putenv(pam, (std::string(key) + "=" + value).c_str()) == PAM_SUCCESS;
}

static void writeSession(const std::string &id)
{
    // This is a private stdout pipe to root plmd, never untrusted input.
    fprintf(stdout, "SESSION=%s\n", id.c_str());
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (geteuid() != 0 || argc != 7
        || std::string(argv[1]) != "--mode"
        || std::string(argv[3]) != "--user"
        || std::string(argv[5]) != "--vt") return 2;

    const std::string mode = argv[2];
    const std::string name = argv[4];
    const std::string vtnr = argv[6];
    if ((mode != "user" && mode != "greeter") || !isName(name)
        || (mode == "greeter" && name != "plm")
        || (mode == "user" && (name == "root" || name == "plm"))
        || (vtnr != (mode == "greeter" ? "1" : "2"))) return 2;

    const passwd *found = getpwnam(name.c_str());
    if (!found || found->pw_uid == 0) return 2;
    const uid_t uid = found->pw_uid;
    const gid_t gid = found->pw_gid;
    const std::string home = found->pw_dir ? found->pw_dir : "/";
    const std::string shell = found->pw_shell ? found->pw_shell : "/bin/sh";
    const std::string tty = "/dev/tty" + vtnr;

    struct sigaction action{};
    action.sa_handler = terminate;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    Credentials credentials;
    if (mode == "user") {
        uint32_t wireSize = 0;
        if (!readExact(STDIN_FILENO, &wireSize, sizeof(wireSize))) return 2;
        const uint32_t n = ntohl(wireSize);
        if (n == 0 || n > 1024) return 2;
        credentials.password.resize(n + 1, '\0');
        if (!readExact(STDIN_FILENO, credentials.password.data(), n)) {
            clean(credentials);
            return 2;
        }
        if (memchr(credentials.password.data(), '\0', n)) {
            clean(credentials);
            return 2;
        }
    }
    close(STDIN_FILENO);

    // A distinct controlling VT is required for pam_systemd/logind.
    if (setsid() < 0) { clean(credentials); return 2; }
    const int ttyfd = open(tty.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (ttyfd < 0 || ioctl(ttyfd, TIOCSCTTY, 0) != 0) {
        clean(credentials);
        if (ttyfd >= 0) close(ttyfd);
        return 2;
    }

    pam_handle_t *pam = nullptr;
    const pam_conv conv{conversation, &credentials};
    const char *service = mode == "greeter" ? "plm-greeter" : "plm";
    int result = pam_start(service, name.c_str(), &conv, &pam);
    if (result != PAM_SUCCESS || !pam) { clean(credentials); close(ttyfd); return 3; }
    const auto finish = [&](int status, bool opened, bool cred) -> int {
        if (opened) pam_close_session(pam, 0);
        if (cred) pam_setcred(pam, PAM_DELETE_CRED);
        pam_end(pam, status);
        clean(credentials);
        close(ttyfd);
        return status == PAM_SUCCESS ? 0 : 3;
    };

    if (pam_set_item(pam, PAM_TTY, ("tty" + vtnr).c_str()) != PAM_SUCCESS
        || !putenvPam(pam, "XDG_SESSION_TYPE", "wayland")
        || !putenvPam(pam, "XDG_SESSION_CLASS",
                      mode == "greeter" ? "greeter" : "user")
        || !putenvPam(pam, "XDG_SESSION_DESKTOP",
                      mode == "greeter" ? "plm" : "KDE")
        || !putenvPam(pam, "XDG_SEAT", "seat0")
        || !putenvPam(pam, "XDG_VTNR", vtnr)) {
        return finish(PAM_SYSTEM_ERR, false, false);
    }

    if (mode == "user") {
        result = pam_authenticate(pam, 0);
        if (result == PAM_SUCCESS) result = pam_acct_mgmt(pam, 0);
        if (result == PAM_NEW_AUTHTOK_REQD) {
            // Expired credentials: PAM prompts for a password change using
            // the same private challenge-response pipe.
            result = pam_chauthtok(pam, PAM_CHANGE_EXPIRED_AUTHTOK);
            if (result == PAM_SUCCESS) result = pam_acct_mgmt(pam, 0);
        }
        if (result != PAM_SUCCESS) {
            fprintf(stderr, "PAM authentication/account check failed\n");
            return finish(result, false, false);
        }
    } else {
        result = pam_acct_mgmt(pam, 0);
        if (result != PAM_SUCCESS) return finish(result, false, false);
    }
    clean(credentials);

    result = pam_setcred(pam, PAM_ESTABLISH_CRED);
    if (result != PAM_SUCCESS) return finish(result, false, false);
    result = pam_open_session(pam, 0);
    if (result != PAM_SUCCESS) return finish(result, false, true);

    const char *sid = pam_getenv(pam, "XDG_SESSION_ID");
    const char *runtime = pam_getenv(pam, "XDG_RUNTIME_DIR");
    if (!sid || !*sid || !runtime || !*runtime) {
        fprintf(stderr, "pam_systemd did not register a runtime session\n");
        return finish(PAM_SESSION_ERR, true, true);
    }
    const std::string session = sid;
    const std::string runtimeDir = runtime;
    const std::string expectedRuntime = "/run/user/" + std::to_string(uid);
    struct stat runtimeStat{};
    if (runtimeDir != expectedRuntime ||
        stat(runtimeDir.c_str(), &runtimeStat) != 0 ||
        !S_ISDIR(runtimeStat.st_mode) || runtimeStat.st_uid != uid) {
        fprintf(stderr, "pam_systemd returned an invalid runtime directory\n");
        return finish(PAM_SESSION_ERR, true, true);
    }
    const auto pamEnvironment = sessionEnvironment(pam);

    // The supervisor must activate this logind session *before* KWin
    // acquires DRM master and input devices. This avoids a VT handoff race.
    writeSession(session);
    bool foreground = false;
    for (int i = 0; i < 250 && !interrupted; ++i) {
        if (sd_session_is_active(session.c_str()) > 0) {
            foreground = true;
            break;
        }
        usleep(20000);
    }
    if (!foreground) {
        fprintf(stderr, "logind did not activate the session's VT\n");
        return finish(PAM_SESSION_ERR, true, true);
    }

    const pid_t child = fork();
    if (child < 0) return finish(PAM_SYSTEM_ERR, true, true);
    if (child == 0) {
        if (dup2(ttyfd, STDIN_FILENO) < 0
            || dup2(ttyfd, STDOUT_FILENO) < 0
            || dup2(ttyfd, STDERR_FILENO) < 0) _exit(125);
        close(ttyfd);
        if (initgroups(name.c_str(), gid) != 0
            || setgid(gid) != 0 || setuid(uid) != 0) _exit(125);
        umask(022);
        clearenv();
        for (const auto &entry : pamEnvironment) {
            const auto equal = entry.find('=');
            const std::string key = entry.substr(0, equal);
            const std::string val = entry.substr(equal + 1);
            setenv(key.c_str(), val.c_str(), 1);
        }
        setenv("HOME", home.c_str(), 1);
        setenv("USER", name.c_str(), 1);
        setenv("LOGNAME", name.c_str(), 1);
        setenv("SHELL", shell.c_str(), 1);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        setenv("XDG_RUNTIME_DIR", runtimeDir.c_str(), 1);
        setenv("XDG_SESSION_ID", session.c_str(), 1);
        setenv("XDG_SESSION_CLASS", mode == "greeter" ? "greeter" : "user", 1);
        setenv("XDG_SESSION_DESKTOP", mode == "greeter" ? "plm" : "KDE", 1);
        setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("XDG_SEAT", "seat0", 1);
        setenv("XDG_VTNR", vtnr.c_str(), 1);
        setenv("DBUS_SESSION_BUS_ADDRESS",
               ("unix:path=" + runtimeDir + "/bus").c_str(), 1);
        if (!getenv("LANG")) setenv("LANG", "C.UTF-8", 1);
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        if (chdir(home.c_str()) != 0 && chdir("/") != 0) _exit(125);

        if (mode == "greeter") {
            execl("/usr/bin/kwin_wayland", "kwin_wayland", "--drm",
                  "--no-global-shortcuts", "--no-lockscreen", "--locale1",
                  "--exit-with-session=/usr/libexec/plm-greeter",
                  static_cast<char *>(nullptr));
        } else {
            execl("/usr/bin/startplasma-wayland", "startplasma-wayland",
                  static_cast<char *>(nullptr));
        }
        _exit(127);
    }
    // KWin/Plasma starts as the unprivileged child; PAM lives only in worker.
    int childStatus = 0;
    bool signaled = false;
    while (true) {
        const pid_t waited = waitpid(child, &childStatus, 0);
        if (waited == child) break;
        if (waited < 0 && errno == EINTR) {
            if (interrupted && !signaled) {
                kill(child, SIGTERM);
                signaled = true;
            }
            continue;
        }
        if (waited < 0 && errno == ECHILD) break;
        if (waited < 0) {
            kill(child, SIGKILL);
            while (waitpid(child, &childStatus, 0) < 0 && errno == EINTR) {}
            break;
        }
    }
    if (WIFSIGNALED(childStatus) ||
        (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) != 0))
        return finish(PAM_SESSION_ERR, true, true);
    return finish(PAM_SUCCESS, true, true);
}
