# LiteKern X — Monitor and Fan Control

A lightweight system monitor with fan control for the EeePC 1000HE, built in stages. The plan and its checkboxes are in [`05-LiteKernX-Expansion.md` §9](../LiteKernX-Roadmap/05-LiteKernX-Expansion.md) (brought forward before v1.0, on purpose); this file is the design and how to build, run and test each stage.

## The target
- **EeePC 1000HE:** Intel Atom N280 (one core plus Hyper-Threading, 32-bit only, 1.66 GHz, SpeedStep), 945GSE chipset, GMA 950, 1-2 GB RAM, one fan run by the embedded controller (EC), a 1024×600 screen.
- **LiteKern X is not Linux.** There's no `/proc`, `/sys` or hwmon. Apps are ring 3 KERN86 programs (C, no `malloc`, no floating point, at most 1 MiB) that can't touch I/O ports or MSRs. ACPI's tables are read, but AML is never run.

## Design
```
 ring 3   apps/probe (stage a)        apps/monitor (stages b-g)
             |  KERN86 calls (SYS_HW_PROBE now; sensors, history, fan, power later)
 ───────────────────────────────────────────────────────────────────────────
 ring 0   kernel/hwprobe.c   what's there (stage a)
          kernel/sensors.c   one table of backends, each saying what it found (stage b):
             msr   CPU temperature, clock, throttling       (kernel/cpu.h)
             ec    fan, EC temperatures, battery             (drivers/ec.c + a register map)
             ata   drive temperature                         (drivers/ata.c, SMART)
             kern  CPU %, memory, disk sectors, per-app use
          kernel/thermal.c   fan policy and safety, on the RTC's 1 Hz tick, app or no app (stage e)
          kernel/history.c   tiered history in RAM (stage c)
```
- **The kernel is the privileged helper.** Hardware access, the fan's safety floors, the emergency override and handing the fan back to the firmware all live in the kernel; the app only asks. A crashed, stopped or closed app can't leave the fan in manual mode.
- **The EC is read directly.** The EeePC's ACPI methods (`_TMP`, `_BST`, the fan's) are AML, which LiteKern X doesn't run. Instead the probe lists the fields the tables name in EC RAM, and comparing EC RAM over time (idle, busy, cooling, on battery) shows which bytes are which. That becomes a register map, keyed to the BIOS version, since EC layouts can change between BIOS versions.
- **RAM only for now.** Apps can't write file contents yet (§8's SYS_FS_WRITE), so settings, profiles and history last until a restart. CSV export and profile import/export wait for it.

## Safety rules (stage e, and everything before it)
- **Monitoring only by default.** No EC write happens until the register map is marked verified for this BIOS version **and** the user has confirmed a warning. Without saved settings, that's asked again after each restart.
- **Floors that rise with temperature.** Whatever a profile or the user asks, the fan never runs below the floor for the current CPU temperature. Proposed (to confirm with the real fan steps): below 55 °C any, from 55 °C at least low, from 65 °C at least medium, from 75 °C at least high, with 3 °C of hysteresis.
- **Emergency override:** at 85 °C or more, full speed and an on-screen warning (and a beep), whatever the profile. The N280 throttles itself at about 90 °C.
- **Hand back to the firmware** when the app exits, crashes or is stopped, when a sensor gives impossible values (≤ 5 °C, ≥ 125 °C, a stuck 0x00 or 0xff) for 3 s, or when an EC transaction times out.
- **Fan steps as the EC has them:** a few levels or 0-255, detected and used as they are; curves map onto them.

## Stage (a): the hardware probe (done 2026-10-03)
Lists which sensors and controls this machine has, and which LiteKern X can reach. **It only reads.**

| Piece | What it does |
|---|---|
| `kernel/cpu.c` | `rdmsr_safe()`: a register the CPU doesn't have returns -1 instead of a kernel panic (the #GP handler resumes it) |
| `kernel/smbios.c` | The BIOS vendor, version and date, and the system and board names |
| `kernel/acpi.c` | Keeps every table it checked at boot, for later reading |
| `kernel/aml.c` | Finds declarations in AML without running it: devices and their `_HID`s, operation regions, the fields over EC RAM, thermal zones and their constant trip points, constant P-state tables, `_CRS` I/O ports |
| `drivers/ec.c` | The ACPI embedded controller (`ec0`): ports from the ECDT or the PNP0C09 device's `_CRS`; **reads only** (the ACPI read command, bounded waits) |
| `drivers/ata.c` | `BLK_SMART`: SMART READ DATA from the internal disk. Never changes a SMART setting |
| `kernel/hwprobe.c` | Builds the report; `SYS_HW_PROBE` hands it to apps |
| `apps/probe/` | **Hardware Probe**: a summary (one verdict per planned feature) and the details by section. **Read EC RAM** asks once, then reads all 256 bytes; each later read highlights what changed |
| `kernel/lkx.c` timer | `SYS_TIMER_SET` / `K86_EVENT_TIMER`: an app gets a tick every N ms. A due tick makes the app yield to the kernel's loop first, so input and the desktop keep running; a busy app still ticks every pass |
| `SYS_EC_READ` | The EC's 256 bytes in one call, for apps (read-only) |
| **Watch** | Reads the EC every 2 s; keeps, per byte that ever changed, its lowest and highest value and how often it moved; logs charger and battery-state changes by themselves |
| **Stress** | Runs the CPU flat out for up to 4 minutes (with Watch on) so the fan has to react. Stops at 85 °C on the EC's CPU temperature byte (0x51), on nonsense, or if the EC stops answering |

Status dots: accent = there and read; amber = there but not usable yet (the note says why); grey = missing; red = there but failing or giving nonsense.

### Build, run, test
```
wsl make                  # the image, with the Hardware Probe app
wsl make run              # QEMU: Show Apps -> Hardware Probe
wsl make test-kernel      # includes "Hardware Probe: ..." and the rdmsr_safe self-test
wsl make usb              # build/usb/litekernx.img for the EeePC
```
QEMU has no EC, thermal zone, battery or P-states of its own. The test boots with a test SSDT that declares them as an Eee PC's tables do (`tests/kernel/make-probe-ssdt.py`) and a blank second disk for SMART. To see it by hand:
```
wsl python3 tests/kernel/make-probe-ssdt.py build/test-probe/ssdt.aml
wsl truncate -s 64M build/test-probe/disk.img
wsl bash vm/qemu.sh -- -acpitable file=build/test-probe/ssdt.aml -drive file=build/test-probe/disk.img,format=raw,if=ide,index=1
```
On the first run the probe logs its summary (`probe: ...`, in the Log app too) and every detail line to serial only (`probe: <section>: ...`).

### Verify before stage (b)
On the EeePC, `docs/HARDWARE-TEST.md` §4G. What decides stage (b):
1. **CPU temperature** reads (an accent dot, a plausible value), and whether TjMax came from MSR 0x1A2 or was assumed (90 °C).
2. **The EC** is found and answers, and where its ports came from.
3. **EC fields:** how many, and their names. Fan-like names show up in the summary.
4. **EC RAM over time** (idle, busy, cooling, unplugged): which bytes move. Together with the field names, that's the register map for the fan's speed, the temperatures and the battery.
5. **P-states** found or not, and the **BIOS version**.

### What the first EeePC run found (2026-10-04, BIOS 0501)
EC at ports 0x62/0x66 (from its `_CRS`), 161 named fields, ACPI mode off (the firmware still runs thermal events). The CPU's own digital thermal sensor gives no valid reading (IA32_THERM_STATUS bit 31 clear), so the EC's byte is the CPU temperature source. From comparing reads (idle, load, charger in/out):

| EC byte | Meaning | Confirmed by |
|---|---|---|
| 0x51 | CPU temperature, °C | rose 48 → 55 under load and charging |
| 0x57 | second temperature, °C | rose with it |
| 0x2C-0x2D | battery voltage, mV (LE) | 7200, 7456 while charging |
| 0x2E-0x2F | battery current, mA, signed (LE) | -1384 discharging, +2412 charging |
| 0x30-0x31 | remaining capacity, mAh (LE) | fell on battery, rose on the charger |
| 0x3F | charge state | 1 discharging, 2 charging |
| 0xD0 bit 0 | AC adapter present | 0x06 unplugged, 0x07 plugged in |
| 0x58, 0x59, 0x5A | probably firmware thresholds (82, 85, 90) | unchanged |
| 0x63, 0x66-0x67 | **fan candidates** (rose 35→50 and 850→1240 as it warmed) | not yet: the screen saver barely warms the CPU, so Watch and Stress exist to settle it |
| 0xF3 | backlight level (0x3B, the GMA register's value) | matches |
