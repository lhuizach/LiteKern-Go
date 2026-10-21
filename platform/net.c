/* LiteKern X on Linux — networking (kernel/net.h), phase L5. Linux has the
 * drivers (the 1000HE's Atheros Ethernet and Wi-Fi); this drives them from
 * lkx-shell's own loop, with no daemon of its own:
 *
 *   Ethernet  brought up; while a cable is in, udhcpc (busybox) asks for an
 *             address (its default script sets it, the route and the DNS
 *             servers in /etc/resolv.conf); cable out, it's stopped
 *   Wi-Fi     the radio unblocked (rfkill), wpa_supplicant started and driven
 *             through its control socket: scanning, joining, its events (a
 *             wrong password); once joined, udhcpc on it as well
 *
 * Remembered networks are on the stick, /LiteKern/wifi.txt (one line each:
 * "wifi <name in hex> <password in hex>"), given to wpa_supplicant at start,
 * which joins whichever is in range. The passwords are there as typed (WPA3
 * needs them so): the stick is as private as the EeePC.
 *
 * Only on the EeePC (LKX_TARGET, set by lkx-start): on the PC, the PC's
 * network isn't ours to change. The tests' simulated Wi-Fi (mac80211_hwsim)
 * runs an access point on one radio; LKX_NET_SKIP names it. */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"   /* names are cut to fit, on purpose */
#include "kernel/net.h"
#include "kernel/printk.h"
#include "kernel/settings.h"
#include "kernel/timing.h"
#include "platform/platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RUN_DIR     "/run/lkx-net"
#define CTRL_DIR    "/run/wpa_supplicant"
#define KNOWN_FILE  "wifi.txt"
#define KNOWN_MAX   16
#define SCAN_MAX    64

static int enabled;
static char eth[IFNAMSIZ], wlan[IFNAMSIZ];
static pid_t dhcp[2];               /* udhcpc: [0] Ethernet, [1] Wi-Fi */
static int ctrl = -1;               /* wpa_supplicant's control socket */
static struct { char ssid[K86_SSID_MAX], pass[K86_PASSWORD]; } known[KNOWN_MAX];
static int nknown;
static struct k86_wifi found[SCAN_MAX];
static int nfound;
static char wpa_state[32], cur_ssid[K86_SSID_MAX];
static int wrong_key, signal_pct;
static int drop_pending;
static char joining[K86_SSID_MAX];  /* the network last asked for: named when its password fails */
static struct { char ssid[K86_SSID_MAX], pass[K86_PASSWORD]; } pending;    /* joined, not yet remembered */

static void save_known(void);

/* The network just joined, if it was being asked for: remembered now. */
static void remember(void)
{
    if (!pending.ssid[0] || strcmp(pending.ssid, cur_ssid))
        return;
    int k = 0;
    while (k < nknown && strcmp(known[k].ssid, pending.ssid))
        k++;
    if (k == nknown && nknown < KNOWN_MAX)
        nknown++;
    if (k < nknown) {
        known[k] = (typeof(known[k])){ { 0 }, { 0 } };
        snprintf(known[k].ssid, sizeof(known[k].ssid), "%s", pending.ssid);
        snprintf(known[k].pass, sizeof(known[k].pass), "%s", pending.pass);
        save_known();
        kprintf("net: Wi-Fi: remembered %s\n", pending.ssid);
    }
    pending.ssid[0] = '\0';
}
static uint32_t next_poll;
static struct k86_net shown;        /* what was logged last */

/* --- small things -------------------------------------------------------------------------- */

static int read_line(const char *path, char *out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, max, f))
        out[0] = '\0';
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return 0;
}

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    ssize_t k = write(fd, text, strlen(text));
    (void)k;
    close(fd);
}

/* Run a program and wait for it (quietly). */
static int run(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 1), dup2(null, 2);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void set_up(const char *ifname)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq r = { 0 };
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
    if (s >= 0 && !ioctl(s, SIOCGIFFLAGS, &r)) {
        r.ifr_flags |= IFF_UP;
        ioctl(s, SIOCSIFFLAGS, &r);
    }
    if (s >= 0)
        close(s);
}

/* The IPv4 address of ifname ("" none). */
static void address_of(const char *ifname, char *out, int max)
{
    out[0] = '\0';
    if (!ifname[0])
        return;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct ifreq r = { 0 };
    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", ifname);
    if (s >= 0 && !ioctl(s, SIOCGIFADDR, &r))
        inet_ntop(AF_INET, &((struct sockaddr_in *)&r.ifr_addr)->sin_addr, out, (socklen_t)max);
    if (s >= 0)
        close(s);
}

static void hex(char *out, const char *s)
{
    for (; *s; s++, out += 2)
        sprintf(out, "%02x", (unsigned char)*s);
    *out = '\0';
}

static int unhex(char *out, int max, const char *h)
{
    int n = 0;
    for (; h[0] && h[1] && n < max - 1; h += 2) {
        unsigned v;
        if (sscanf(h, "%2x", &v) != 1)
            return -1;
        out[n++] = (char)v;
    }
    out[n] = '\0';
    return 0;
}

/* --- DHCP ---------------------------------------------------------------------------------- */

static void dhcp_start(int i, const char *ifname)
{
    if (dhcp[i])
        return;
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(null, 1), dup2(null, 2);
        /* -f: stays, renewing; its default script sets the address, the
         * route and /etc/resolv.conf. */
        execl("/sbin/udhcpc", "udhcpc", "-f", "-i", ifname, "-t", "10", "-T", "3", "-A", "10", (char *)0);
        _exit(127);
    }
    dhcp[i] = pid;
}

static void dhcp_stop(int i, const char *ifname)
{
    if (!dhcp[i])
        return;
    kill(dhcp[i], SIGTERM);
    waitpid(dhcp[i], 0, 0);
    dhcp[i] = 0;
    const char *argv[] = { "/sbin/ip", "addr", "flush", "dev", ifname, 0 };
    run(argv);
}

/* --- the clock ----------------------------------------------------------------------------- */

/* Once there's an address: the time from the internet (busybox ntpd, once),
 * then written back to the EeePC's own clock (as local time, as its BIOS
 * keeps it) so the next start is right even offline. HTTPS needs it: a
 * certificate is refused when the date is wrong. Tried each minute until it
 * works. */
static pid_t ntp;
static int clock_set;
static uint32_t next_ntp;

static void clock_poll(int online)
{
    if (ntp) {
        int st;
        if (waitpid(ntp, &st, WNOHANG) != ntp)
            return;
        ntp = 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            clock_set = 1;
            const char *argv[] = { "/sbin/hwclock", "-w", "-l", 0 };
            run(argv);
            time_t now = time(0);
            struct tm tm;
            localtime_r(&now, &tm);
            kprintf("net: clock set from the internet: %04d-%02d-%02d %02d:%02d\n", tm.tm_year + 1900,
                    tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        } else {
            kprintf("net: clock: no answer from the time servers (trying again in a minute)\n");
        }
    }
    if (clock_set || !online || (int32_t)(uptime_ms() - next_ntp) < 0)
        return;
    next_ntp = uptime_ms() + 60000;
    pid_t pid = fork();
    if (pid < 0)
        return;
    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(null, 1), dup2(null, 2);
        /* -q: set the clock once and leave; -n: in the foreground. */
        execl("/usr/sbin/ntpd", "ntpd", "-n", "-q", "-p", "pool.ntp.org", "-p", "time.google.com", (char *)0);
        _exit(127);
    }
    ntp = pid;
}

/* --- wpa_supplicant ------------------------------------------------------------------------ */

static void event(const char *msg)
{
    if (strstr(msg, "CTRL-EVENT-SSID-TEMP-DISABLED") && strstr(msg, "reason=WRONG_KEY")) {
        if (!wrong_key)
            kprintf("net: Wi-Fi: the password for %s was refused\n", joining[0] ? joining : "the network");
        wrong_key = 1;
        drop_pending |= pending.ssid[0] != 0;   /* net_poll lets it go (not from in here) */
    } else if (strstr(msg, "CTRL-EVENT-CONNECTED")) {
        wrong_key = 0;
    }
}

/* Send a command, return its reply (events that arrive meanwhile are taken). */
static int wpa(const char *cmd, char *reply, int max)
{
    if (ctrl < 0 || send(ctrl, cmd, strlen(cmd), 0) < 0)
        return -1;
    for (;;) {
        struct pollfd p = { ctrl, POLLIN, 0 };
        if (poll(&p, 1, 3000) <= 0)
            return -1;
        ssize_t n = recv(ctrl, reply, (size_t)max - 1, 0);
        if (n < 0)
            return -1;
        reply[n] = '\0';
        if (reply[0] == '<') {              /* "<3>CTRL-EVENT-...": not our answer */
            event(reply);
            continue;
        }
        return (int)n;
    }
}

static int wpa_ok(const char *cmd)
{
    char r[64];
    return wpa(cmd, r, sizeof(r)) > 0 && !strncmp(r, "OK", 2) ? 0 : -1;
}

/* Give wpa_supplicant a network (and select it, to join now). */
static int wpa_add(const char *ssid, const char *pass, int select)
{
    char r[64], cmd[256], h[2 * K86_SSID_MAX + 1];
    if (wpa("ADD_NETWORK", r, sizeof(r)) <= 0)
        return -1;
    int id = atoi(r);
    hex(h, ssid);
    snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid %s", id, h);
    int err = wpa_ok(cmd);
    if (pass[0]) {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", id, pass);
        err |= wpa_ok(cmd);
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt WPA-PSK WPA-PSK-SHA256 SAE", id);
        err |= wpa_ok(cmd);
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ieee80211w 1", id);
        wpa_ok(cmd);                        /* WPA3 wants it; older ones don't mind */
    } else {
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
        err |= wpa_ok(cmd);
    }
    snprintf(cmd, sizeof(cmd), "%s %d", select ? "SELECT_NETWORK" : "ENABLE_NETWORK", id);
    err |= wpa_ok(cmd);
    return err ? -1 : id;
}

/* Remove every network wpa_supplicant has by this name. */
static void wpa_remove(const char *ssid)
{
    char list[4096];
    if (wpa("LIST_NETWORKS", list, sizeof(list)) <= 0)
        return;
    /* "network id / ssid / bssid / flags", then "<id>\t<ssid>\t..." */
    for (char *l = strchr(list, '\n'); l && *++l;) {
        char *end = strchr(l, '\n');
        if (end)
            *end = '\0';
        char *tab = strchr(l, '\t');
        if (tab) {
            char name[K86_SSID_MAX];
            int n = (int)strcspn(tab + 1, "\t");
            snprintf(name, sizeof(name), "%.*s", n, tab + 1);
            if (!strcmp(name, ssid)) {
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", atoi(l));
                wpa_ok(cmd);
            }
        }
        l = end;
    }
}

static int wpa_connect(void)
{
    char local[64];
    snprintf(local, sizeof(local), RUN_DIR "/ctrl-%d", (int)getpid());
    unlink(local);
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX }, b = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", local);
    snprintf(b.sun_path, sizeof(b.sun_path), CTRL_DIR "/%s", wlan);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || connect(s, (struct sockaddr *)&b, sizeof(b))) {
        if (s >= 0)
            close(s);
        return -1;
    }
    ctrl = s;
    wpa_ok("ATTACH");                       /* its events too (a wrong password) */
    return 0;
}

static void load_known(void)
{
    char text[4096];
    int n = settings_read_file(KNOWN_FILE, text, sizeof(text) - 1);
    nknown = 0;
    if (n <= 0)
        return;
    text[n] = '\0';
    for (char *l = strtok(text, "\n"); l && nknown < KNOWN_MAX; l = strtok(0, "\n")) {
        char hs[2 * K86_SSID_MAX + 2], hp[2 * K86_PASSWORD + 2] = "";
        if (sscanf(l, "wifi %67s %129s", hs, hp) < 1)
            continue;
        if (!unhex(known[nknown].ssid, K86_SSID_MAX, hs) && !unhex(known[nknown].pass, K86_PASSWORD, hp) &&
            known[nknown].ssid[0])
            nknown++;
    }
}

static void save_known(void)
{
    char text[4096] = "# LiteKern X: Wi-Fi networks to join (name and password, in hex). Settings -> Network.\n";
    for (int i = 0; i < nknown; i++) {
        char hs[2 * K86_SSID_MAX + 1], hp[2 * K86_PASSWORD + 1];
        hex(hs, known[i].ssid);
        hex(hp, known[i].pass);
        size_t n = strlen(text);
        snprintf(text + n, sizeof(text) - n, "wifi %s %s\n", hs, *hp ? hp : "-");
    }
    if (settings_write_file(KNOWN_FILE, text, (uint32_t)strlen(text)) < 0)
        kprintf("net: couldn't save the Wi-Fi networks\n");
}

/* --- start ---------------------------------------------------------------------------------- */

void net_init(void)
{
    if (!getenv("LKX_TARGET")) {
        kprintf("net: the PC build leaves the PC's network alone\n");
        return;
    }
    enabled = 1;
    mkdir(RUN_DIR, 0700);
    const char *skip = getenv("LKX_NET_SKIP");
    DIR *d = opendir("/sys/class/net");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[300], type[16] = "";
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || (skip && !strcmp(e->d_name, skip)))
            continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", e->d_name);
        struct stat st;
        int wireless = !stat(path, &st);
        snprintf(path, sizeof(path), "/sys/class/net/%s/type", e->d_name);
        read_line(path, type, sizeof(type));
        if (atoi(type) != 1)                /* ARPHRD_ETHER: not a monitor or tunnel */
            continue;
        if (wireless && !wlan[0] && strncmp(e->d_name, "hwsim", 5))
            snprintf(wlan, sizeof(wlan), "%s", e->d_name);
        else if (!wireless && !eth[0])
            snprintf(eth, sizeof(eth), "%s", e->d_name);
    }
    if (d)
        closedir(d);
    /* The radio on (Fn+F2's switch may have left it blocked). */
    d = opendir("/sys/class/rfkill");
    while (d && (e = readdir(d))) {
        char path[300];
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", e->d_name);
        write_file(path, "0");
    }
    if (d)
        closedir(d);
    if (eth[0])
        set_up(eth);
    load_known();
    if (wlan[0]) {
        set_up(wlan);
        write_file(RUN_DIR "/wpa.conf", "ctrl_interface=" CTRL_DIR "\nupdate_config=0\n");
        const char *argv[] = { "/usr/sbin/wpa_supplicant", "-B", "-i", wlan, "-D", "nl80211",
                               "-c", RUN_DIR "/wpa.conf", 0 };
        if (run(argv))
            kprintf("net: Wi-Fi: wpa_supplicant didn't start\n");
        for (int i = 0; i < 40 && wpa_connect(); i++)
            usleep(50000);
        for (int i = 0; i < nknown; i++)
            wpa_add(known[i].ssid, known[i].pass, 0);
        wpa_ok("SCAN");
    }
    kprintf("net: Ethernet %s, Wi-Fi %s (%d remembered)%s\n", eth[0] ? eth : "none", wlan[0] ? wlan : "none",
            nknown, wlan[0] && ctrl < 0 ? "; Wi-Fi control failed" : "");
}

/* --- every so often, from the loop ------------------------------------------------------------ */

static int carrier(void)
{
    char path[64], v[8] = "";
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", eth);
    return eth[0] && !read_line(path, v, sizeof(v)) && v[0] == '1';
}

static int radio_blocked(void)
{
    DIR *d = opendir("/sys/class/rfkill");
    struct dirent *e;
    int blocked = 0;
    while (d && (e = readdir(d))) {
        char path[300], v[16] = "";
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", e->d_name);
        read_line(path, v, sizeof(v));
        if (strcmp(v, "wlan"))
            continue;
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/hard", e->d_name);
        read_line(path, v, sizeof(v));
        blocked |= v[0] == '1';
    }
    if (d)
        closedir(d);
    return blocked;
}

static void refresh_scan(void)
{
    char r[16384];
    if (wpa("SCAN_RESULTS", r, sizeof(r)) <= 0)
        return;
    nfound = 0;
    /* "bssid / frequency / signal level / flags / ssid", then tab-separated lines */
    for (char *l = strtok(r, "\n"); l; l = strtok(0, "\n")) {
        char bssid[20], flags[160], ssid[K86_SSID_MAX] = "";
        int freq, dbm;
        if (sscanf(l, "%19s %d %d %159s %32[^\n]", bssid, &freq, &dbm, flags, ssid) < 4 || !ssid[0])
            continue;
        int pct = dbm >= -50 ? 100 : dbm <= -100 ? 0 : 2 * (dbm + 100);
        int k = 0;
        while (k < nfound && strcmp(found[k].ssid, ssid))
            k++;
        if (k < nfound) {                   /* the same network, another access point */
            if (pct > found[k].signal)
                found[k].signal = (uint8_t)pct;
            continue;
        }
        if (nfound == SCAN_MAX)
            continue;
        struct k86_wifi *w = &found[nfound++];
        memset(w, 0, sizeof(*w));
        snprintf(w->ssid, sizeof(w->ssid), "%s", ssid);
        w->signal = (uint8_t)pct;
        w->secure = strstr(flags, "WPA") || strstr(flags, "WEP") || strstr(flags, "SAE") || strstr(flags, "RSN");
    }
    /* Strongest first. */
    for (int i = 1; i < nfound; i++)
        for (int j = i; j > 0 && found[j].signal > found[j - 1].signal; j--) {
            struct k86_wifi t = found[j];
            found[j] = found[j - 1];
            found[j - 1] = t;
        }
}

static void log_changes(void)
{
    struct k86_net now;
    net_status(&now);
    if (now.wired != shown.wired) {
        static const char *const names[] = { "none", "no cable", "down", "asking for an address", "connected", "?" };
        char a[16];
        address_of(eth, a, sizeof(a));
        kprintf("net: Ethernet %s%s%s\n", names[now.wired], now.wired == K86_NET_UP ? ", " : "",
                now.wired == K86_NET_UP ? a : "");
    }
    if (now.wifi != shown.wifi || strcmp(now.ssid, shown.ssid)) {
        static const char *const names[] = { "none", "switched off", "not connected", "joining",
                                             "connected", "wrong password" };
        char a[16];
        address_of(wlan, a, sizeof(a));
        kprintf("net: Wi-Fi %s%s%s%s%s\n", names[now.wifi], now.ssid[0] ? " to " : "", now.ssid,
                now.wifi == K86_NET_UP ? ", " : "", now.wifi == K86_NET_UP ? a : "");
    }
    shown = now;
}

void net_poll(void)
{
    if (!enabled || (int32_t)(uptime_ms() - next_poll) < 0)
        return;
    next_poll = uptime_ms() + 500;
    /* Events that came while nobody asked. */
    char msg[2048];
    while (ctrl >= 0) {
        ssize_t n = recv(ctrl, msg, sizeof(msg) - 1, MSG_DONTWAIT);
        if (n <= 0)
            break;
        msg[n] = '\0';
        event(msg);
        if (strstr(msg, "CTRL-EVENT-SCAN-RESULTS"))
            refresh_scan();
    }
    /* A new network's password refused: it isn't kept, and Wi-Fi stops
     * trying it (Settings asks for the password again). */
    if (drop_pending) {
        wpa_remove(pending.ssid);
        pending.ssid[0] = '\0';
        drop_pending = 0;
    }
    /* udhcpc ended by itself (it shouldn't): free to start again. */
    for (int i = 0; i < 2; i++)
        if (dhcp[i] && waitpid(dhcp[i], 0, WNOHANG) == dhcp[i])
            dhcp[i] = 0;
    if (eth[0]) {
        if (carrier())
            dhcp_start(0, eth);
        else
            dhcp_stop(0, eth);
    }
    if (ctrl >= 0) {
        char r[2048];
        wpa_state[0] = cur_ssid[0] = '\0';
        if (wpa("STATUS", r, sizeof(r)) > 0)
            for (char *l = strtok(r, "\n"); l; l = strtok(0, "\n")) {
                if (!strncmp(l, "wpa_state=", 10))
                    snprintf(wpa_state, sizeof(wpa_state), "%s", l + 10);
                else if (!strncmp(l, "ssid=", 5))
                    snprintf(cur_ssid, sizeof(cur_ssid), "%s", l + 5);
            }
        if (!strcmp(wpa_state, "COMPLETED")) {
            remember();
            dhcp_start(1, wlan);
            if (wpa("SIGNAL_POLL", r, sizeof(r)) > 0) {
                char *rssi = strstr(r, "RSSI=");
                int dbm = rssi ? atoi(rssi + 5) : -100;
                signal_pct = dbm >= -50 ? 100 : dbm <= -100 ? 0 : 2 * (dbm + 100);
            }
        } else {
            dhcp_stop(1, wlan);
        }
    }
    log_changes();
    clock_poll(shown.wired == K86_NET_UP || shown.wifi == K86_NET_UP);
}

/* --- kernel/net.h --------------------------------------------------------------------------- */

void net_status(struct k86_net *out)
{
    memset(out, 0, sizeof(*out));
    if (!enabled)
        return;
    char ea[16], wa[16];
    address_of(eth, ea, sizeof(ea));
    address_of(wlan, wa, sizeof(wa));
    if (eth[0])
        out->wired = !carrier() ? K86_NET_OFF : ea[0] ? K86_NET_UP : K86_NET_CONNECTING;
    if (wlan[0]) {
        int in_progress = strstr("ASSOCIATING ASSOCIATED 4WAY_HANDSHAKE GROUP_HANDSHAKE AUTHENTICATING", wpa_state) &&
                      wpa_state[0];
        if (radio_blocked())
            out->wifi = K86_NET_OFF;
        else if (!strcmp(wpa_state, "COMPLETED"))
            out->wifi = wa[0] ? K86_NET_UP : K86_NET_CONNECTING;
        else if (wrong_key)
            out->wifi = K86_NET_WRONG_KEY;
        else
            out->wifi = in_progress ? K86_NET_CONNECTING : K86_NET_DOWN;
        /* refused: the network it was for (Wi-Fi itself no longer names it) */
        snprintf(out->ssid, sizeof(out->ssid), "%s", out->wifi == K86_NET_WRONG_KEY ? joining : cur_ssid);
        out->signal = (uint8_t)(out->wifi == K86_NET_UP ? signal_pct : 0);
    }
    snprintf(out->address, sizeof(out->address), "%s", wa[0] ? wa : ea);
}

int net_scan(struct k86_wifi *out, int max)
{
    if (!enabled || ctrl < 0)
        return -ENODEV;
    refresh_scan();
    wpa_ok("SCAN");                         /* fresher results for next time (FAIL-BUSY is fine) */
    for (int i = 0; i < nfound && i < max; i++) {
        out[i] = found[i];
        for (int k = 0; k < nknown; k++)
            out[i].known |= !strcmp(known[k].ssid, found[i].ssid);
        out[i].connected = !strcmp(wpa_state, "COMPLETED") && !strcmp(cur_ssid, found[i].ssid);
    }
    return nfound;
}

int net_join(const char *ssid, const char *password)
{
    if (!enabled || ctrl < 0)
        return -ENODEV;
    /* No password for a network it remembers: the one it has (Settings'
     * Connect on a saved network); for one it doesn't, an open network. */
    char pass[K86_PASSWORD];
    snprintf(pass, sizeof(pass), "%s", password);
    for (int k = 0; !pass[0] && k < nknown; k++)
        if (!strcmp(known[k].ssid, ssid))
            snprintf(pass, sizeof(pass), "%s", known[k].pass);
    password = pass;
    size_t n = strlen(password);
    if (n && (n < 8 || n > 63))
        return -EINVAL;                     /* WPA's rule: 8 to 63 characters */
    wpa_remove(ssid);
    wrong_key = 0;
    if (wpa_add(ssid, password, 1) < 0)
        return -EIO;
    /* Remembered once it works (net_poll: remember()), not before: a wrong
     * password isn't kept. */
    snprintf(pending.ssid, sizeof(pending.ssid), "%s", ssid);
    snprintf(pending.pass, sizeof(pending.pass), "%s", password);
    snprintf(joining, sizeof(joining), "%s", ssid);
    kprintf("net: Wi-Fi: joining %s\n", ssid);
    next_poll = 0;
    return 0;
}

int net_forget(const char *ssid)
{
    if (!enabled)
        return -ENODEV;
    wpa_remove(ssid);
    for (int k = 0; k < nknown; k++)
        if (!strcmp(known[k].ssid, ssid)) {
            known[k] = known[--nknown];
            save_known();
            kprintf("net: Wi-Fi: forgot %s\n", ssid);
            return 0;
        }
    return -ENOENT;
}
