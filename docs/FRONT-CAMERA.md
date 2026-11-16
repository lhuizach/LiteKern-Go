# Front camera (Samsung S5K5E3YX): what's known

Written 16 Nov 2026. Nothing here is built or tried on the phone yet.

## Where the register values came from

Not from any kernel source: Qualcomm's camera stack keeps each sensor's start-up values in a userspace
library, here `/vendor/lib/libmmcamera_s5k5e3yx.so` (32-bit ARM, 18 KB) in the stock BTU firmware's system
image (the firmware zip already on the PC, `AP_...tar.md5` -> `system.img.ext4`, read on the PC only; the
unpacked copy is in WSL `~/fw-a5`, not in git). `tools/unsparse.py` turns the sparse image into one `debugfs`
can read. The values are in the library's `.data`: records of 12 bytes (u16 address, u16 data, 8 bytes of
delay/zero). Only the numbers are used here, for this one phone.

## The values (S5K5E3YX, 2 MIPI lanes)

Start-up ("init", 99 registers, written once after reset; the last one is 0x0104 = 1, group hold on):

```
3000=04 3002=03 3003=04 3004=05 3005=00 3006=10 3007=0a 3008=55 3039=00 303a=00 303b=00 3009=05 300a=55
300b=38 300c=10 3012=14 3013=00 3014=22 300e=79 3010=68 3019=03 301a=00 301b=06 301c=00 301d=22 301e=00
301f=10 3020=00 3021=00 3022=0a 3023=1e 3024=00 3025=00 3026=00 3027=00 3028=1a 3015=00 3016=84 3017=00
3018=a0 302b=10 302c=0a 302d=06 302e=05 302f=0e 3030=2f 3031=08 3032=05 3033=09 3034=05 3035=00 3036=00
3037=00 3038=00 3088=06 308a=08 308c=05 308e=07 3090=06 3092=08 3094=05 3096=21 3055=9e 3099=06 3070=10
3085=31 3086=01 3064=00 3062=08 3061=15 307b=20 3068=01 3074=00 307d=05 3045=01 3046=05 3047=78 307f=b1
3098=01 305c=f6 3063=2f 3400=01 3235=49 3233=00 3234=00 3300=0c 3203=45 3205=4d 320b=40 320c=06 320d=c0
3244=00 3245=00 3246=01 3247=00 3268=88 3269=01 3320=02 0104=01
```

Mode 2576x1932 (the full picture, about 5 MP; the library has it as the preview and snapshot mode):

```
0305=06 0306=00 0307=cf 3c1f=00 0820=03 0821=82 3c1c=58 0114=01      (PLL; 0114=01: 2 lanes; link 0x382)
0340=07 0341=e9 0342=0b 0343=86                                       (frame 2025 lines, line 2950)
0344=00 0345=00 0346=00 0347=02 0348=0a 0349=0f 034a=07 034b=8d        (window 0..2575, 2..1933)
034c=0a 034d=10 034e=07 034f=8c                                       (out 2576 x 1932)
0900=00 0901=00 0383=01 0387=01 0204=00 0205=20 0202=02 0203=00 0200=04 0201=98 0136=1a 0137=05
```

Mode 1920x1080 (the video crop): same PLL; 0340=05 0341=b7, 0342=0b 0343=86, window x 0x140..0x8bf
(0344=01 0345=40, 0348=08 0349=bf), y 0x1ac..0x5e3 (0346=01 0347=ac, 034a=05 034b=e3), out 0x780 x 0x438
(034c=07 034d=80, 034e=04 034f=38); 0387=01 and the same ending (0204=0020 gain, 0202=0200 exposure,
0200=0498, 0136=1a05 clock 26.02 MHz).

Streaming: 0100=01 on, 0100=00 off. 0101=03 (mirror and flip, as the library sets for this mounting).
0104 is group hold (0 = apply).

## The wiring (from the stock device tree: Samsung's boot.img, `qcom,camera@1`)

Read on the PC with `tools/fdt_dump.py` (a small device tree dumper) from the QCDT inside `boot.img`.

| | front (S5K5E3YX) | rear (IMX135, for comparison) |
|---|---|---|
| CSI PHY / CSID | 1 / 1 | 0 / 0 |
| camera bus | CCI master 0, same as the rear | CCI master 0 |
| address | 0x20 (8-bit), 0x10 as 7-bit | |
| clock | MCLK1, gpio27 | MCLK0, gpio26 |
| reset | gpio28 | gpio35 |
| supply enable | gpio120 | gpio33 |
| supplies | the same: vio 1.8 V (PM8916 L16), analog 2.8 V and digital 1.5 V (the RT5033's LDO and BUCK) | |
| mount angle | 270 | 90 |

Power-up order (stock): analog, the enable pin, digital, I/O, reset low (2 ms) then high (30 ms), clock (1 ms),
then 5 ms. The sensor wants 26 MHz; the phone's clock controller gives 23.88, so the driver sets the clock
registers and the PLL multiplier (226) for that.

## What's built (16 Nov 2026: it streams)

- `phone/kernel/0005-a5-front-camera.patch`: `drivers/misc/s5k5e3yx.c` (a module, modelled on `imx135.c`; the
  register header is made by `tools/front-regs.py`), and the device tree: `camera@10` on the camera bus, CSI PHY 1
  with two lanes (`data-lanes = <0 2>`, clock lane 1: a guess from the rear's pattern), MCLK1 and the pins.
- Module parameters for what isn't known yet: `order` (Bayer order, 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR) and `orient`
  (register 0x0101: 1 mirror, 2 flip; the stock firmware writes 3).
- Still to do: see whether it probes (`dmesg | grep S5K5E3YX`), then find the Bayer order and orientation from a
  picture, then a front/back switch in the Camera app (the pipeline needs csiphy1, csid1 instead of 0, 0).

## Result so far (16 Nov 2026)

- Probe: `s5k5e3yx 9-0010: S5K5E3YX: model id 0x5e30`, powered with the stock order.
- Opening the video node failed with -22: mainline camss powers the VFE with the CSID's own number, and the
  8916 has one VFE. `0006-camss-csid1-uses-vfe0.patch` makes CSID 1 use VFE 0.
- `CAM_FRONT=1 CAM_SIZE=1920x1080 camgrab NAME` (phone/tools/camgrab.c) gets frames: a real picture, sideways
  (the sensor is mounted rotated, mount angle 270). The two lanes with `data-lanes = <0 2>` work.
- Not yet: the Bayer order and colours (grey-world gains came out 0.77 / 0.76, so check `order`), the full
  2576x1932 mode, the front/back switch in the Camera app, and the picture's orientation and mirroring.
