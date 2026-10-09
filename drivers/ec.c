/* LiteKern X — the ACPI embedded controller, read-only (kernel/ec.h).
 *
 * Found through ACPI: the ECDT table if the firmware has one, otherwise the
 * PNP0C09 device (DSDT or SSDT) and the two I/O ports of its _CRS (data, then
 * command/status). If the device has no readable _CRS, the standard ports
 * 0x62/0x66 are assumed and reported as assumed. A status register reading
 * 0xff means nothing answers there: no EC.
 *
 * Every read is the ACPI EC protocol (spec 12.3): wait for the input
 * buffer to empty, send READ (0x80), send the address, wait for the output
 * buffer, take the byte. Each wait is bounded (60 ms); a timed-out try is
 * repeated up to 4 times with a pause between, then it's -EIO. The EC can be
 * busy with the battery gauge, and in ACPI legacy mode (SCI_EN off, as on the
 * EeePC) the firmware also uses it from SMM, which no interrupt mask stops: a
 * try can fail or lose a byte to it. The first EeePC run showed a 20 ms wait
 * with no retry gave up within a few samples. */
#include "drivers/builtin.h"
#include "kernel/acpi.h"
#include "kernel/aml.h"
#include "kernel/ec.h"
#include "kernel/errno.h"
#include "kernel/io.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "kernel/timing.h"

#define EC_OBF      0x01    /* output buffer full: a byte for us */
#define EC_IBF      0x02    /* input buffer full: the EC hasn't taken ours yet */
#define EC_CMD_READ 0x80
#define EC_WAIT_MS  60      /* each wait for the EC: it can be busy with the battery gauge */
#define EC_TRIES    4

#define MAX_SCOPES  192

static struct ec_info ec;
static uint32_t ec_retried, ec_failed;       /* reads that needed a retry / gave up */
static struct aml_scope scopes[MAX_SCOPES];

/* The ECDT: EC_CONTROL and EC_DATA as Generic Address Structures (12 bytes
 * each: space, width, offset, access size, 64-bit address), then the path. */
static int from_ecdt(void)
{
    const struct acpi_sdt *t = acpi_find("ECDT");
    if (!t || t->length < 66)
        return -ENOENT;
    const uint8_t *b = (const uint8_t *)t;
    if (b[36] != 1 || b[48] != 1)               /* both must be in I/O space */
        return -ENOENT;
    ec.cmd_port = (uint16_t)(b[40] | b[41] << 8);
    ec.data_port = (uint16_t)(b[52] | b[53] << 8);
    ec.ports_from = EC_PORTS_ECDT;
    int o = 0;
    for (uint32_t i = 65; i < t->length && b[i] && o < (int)sizeof(ec.path) - 1; i++)
        ec.path[o++] = (char)b[i];
    ec.path[o] = '\0';
    return 0;
}

/* The PNP0C09 device, in the DSDT or an SSDT. */
static int from_aml(void)
{
    for (int ti = 0; acpi_table(ti); ti++) {
        const struct acpi_sdt *t = acpi_table(ti);
        if (memcmp(t->sig, "DSDT", 4) && memcmp(t->sig, "SSDT", 4))
            continue;
        const uint8_t *p, *end;
        acpi_body(t, &p, &end);
        int n = aml_scopes(p, end, scopes, MAX_SCOPES);
        if (n > MAX_SCOPES)
            n = MAX_SCOPES;
        for (int i = 0; i < n; i++) {
            if (scopes[i].kind != AML_DEVICE || strcmp(scopes[i].hid, "PNP0C09"))
                continue;
            aml_path(scopes, n, i, ec.path, sizeof(ec.path));
            uint16_t ports[2];
            if (aml_crs_io(scopes[i].start, scopes[i].end, ports, 2) >= 2) {
                ec.data_port = ports[0];
                ec.cmd_port = ports[1];
                ec.ports_from = EC_PORTS_CRS;
            } else {
                ec.data_port = 0x62;
                ec.cmd_port = 0x66;
                ec.ports_from = EC_PORTS_DEFAULT;
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int wait_status(uint8_t mask, uint8_t want)
{
    uint32_t t0 = uptime_ms();
    do {
        for (int i = 0; i < 2000; i++)          /* a burst of cheap status reads... */
            if ((inb(ec.cmd_port) & mask) == want)
                return 0;
    } while (uptime_ms() - t0 < EC_WAIT_MS);    /* ...then check the clock */
    return -EIO;
}

/* One try at the ACPI read command. Interrupts stay on: nothing of ours
 * touches the EC from an interrupt, and holding them off for a whole wait
 * would stall the keyboard and the clock. (In ACPI legacy mode the firmware
 * also uses the EC from SMM, which interrupts can't mask anyway: that is why
 * a try can fail, and why read_byte tries again.) */
static int read_once(uint8_t addr, uint8_t *v)
{
    if (inb(ec.cmd_port) & EC_OBF)
        (void)inb(ec.data_port);                /* a stale byte: not ours */
    int err = wait_status(EC_IBF, 0);
    if (!err) {
        outb(ec.cmd_port, EC_CMD_READ);
        err = wait_status(EC_IBF, 0);
    }
    if (!err) {
        outb(ec.data_port, addr);
        err = wait_status(EC_OBF, EC_OBF);
    }
    if (!err)
        *v = inb(ec.data_port);
    return err;
}

static int read_byte(uint8_t addr, uint8_t *v)
{
    static int logged;
    for (int t = 0; t < EC_TRIES; t++) {
        int err = read_once(addr, v);
        if (!err) {
            if (t)
                ec_retried++;
            return 0;
        }
        if (logged < 6) {
            logged++;
            kprintf("ec0: read of 0x%02x timed out (try %d, status 0x%02x)\n", addr, t + 1,
                    inb(ec.cmd_port));
        }
        uint32_t t0 = uptime_ms();              /* let the EC finish what it was doing */
        while (uptime_ms() - t0 < 3)
            __asm__ volatile("pause");
    }
    ec_failed++;
    return -EIO;
}
static int ec_init(device_t *dev)
{
    (void)dev;
    if (from_ecdt() && from_aml()) {
        kprintf("ec0: no embedded controller in the ACPI tables\n");
        return -ENODEV;
    }
#ifdef LKX_FAKE_EC
    /* Test builds only: an EC that answers in a VM, with values that move
     * the way the EeePC's did (tests/kernel/test-kernel.sh). */
    kprintf("ec0: %s, FAKE (test build); read-only\n", ec.path);
    return 0;
#endif
    ec.status = inb(ec.cmd_port);
    if (ec.status == 0xff) {
        kprintf("ec0: %s declared at 0x%x/0x%x, but nothing answers there\n", ec.path,
                ec.data_port, ec.cmd_port);
        return -ENODEV;
    }
    static const char *const from[] = { "ECDT", "_CRS", "assumed" };
    kprintf("ec0: %s, ports 0x%x/0x%x (%s), status 0x%02x; read-only\n", ec.path, ec.data_port,
            ec.cmd_port, from[ec.ports_from], ec.status);
    return 0;
}

static int ec_ioctl(device_t *dev, unsigned cmd, void *arg)
{
    (void)dev;
    if (!arg)
        return -EINVAL;
    switch (cmd) {
    case EC_GET_INFO:
        *(struct ec_info *)arg = ec;
        return 0;
    case EC_READ: {
        struct ec_io *io = arg;
        return read_byte(io->addr, &io->value);
    }
    case EC_READ_ALL: {
        uint8_t *buf = arg;
#ifdef LKX_FAKE_EC
        /* The CPU temperature climbs 40 -> 59 C and wraps every 40 s; the
         * charger toggles every 10 s, with the battery current and state. */
        uint32_t s = uptime_ms() / 1000;
        memset(buf, 0, 256);
        int ac = s / 10 & 1;
        buf[0x51] = (uint8_t)(40 + s % 20);
        buf[0x57] = (uint8_t)(30 + s % 7);
        buf[0x63] = (uint8_t)(35 + (s % 20 > 10 ? 15 : 0));
        buf[0x66] = 3 + (s % 20 > 10);
        buf[0x67] = (uint8_t)(s * 7);
        buf[0xd0] = (uint8_t)(6 | ac);
        buf[0x3f] = (uint8_t)(1 + ac);
        int cur = ac ? 2412 : -1384;
        buf[0x2e] = (uint8_t)cur;
        buf[0x2f] = (uint8_t)(cur >> 8);
        buf[0x2c] = 0x20;
        buf[0x2d] = 0x1c;
        return 0;
#endif

        for (int a = 0; a < 256; a++) {
            int err = read_byte((uint8_t)a, &buf[a]);
            if (err)
                return err;
        }
        return 0;
    }
    default:
        return -ENOSYS;                         /* no writes: see kernel/ec.h */
    }
}

const driver_t ec_driver = {
    .name = "acpi-ec",
    .pci_ids = NULL,
    .init = ec_init,
    .read = driver_nosys_read,
    .write = driver_nosys_write,
    .ioctl = ec_ioctl,
    .shutdown = driver_noop_shutdown,
};
