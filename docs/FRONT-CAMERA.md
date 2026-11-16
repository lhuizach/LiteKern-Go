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

## Still to find before a driver

- Wiring: which CCI bus and address the sensor is on (the earlier note says it answers; where), its MCLK
  (probably MCLK1), reset/enable GPIOs, and which regulators power it (the rear uses RT5033 LDO/BUCK and
  PM8916 L10/L16). The Samsung msm8916 dts (camera sensor dtsi in the Lineage kernel on GitHub) has them.
- CSI: 2 lanes into a different CSIPHY than the rear (csiphy1, probably), with its own CSID/VFE path.
- Then: `drivers/misc/s5k5e3yx.c` (modelled on `imx135.c`), the dts node, and a front/back switch in the
  Camera app (picture mirrored for the selfie view).
