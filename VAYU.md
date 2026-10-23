# iOS 16 on the Poco X3 Pro (vayu)

Port of this repository (topaz, itself a port of sweet) to the Xiaomi **Poco X3 Pro** (`vayu`, Qualcomm **SM8150-AC** / Snapdragon 860, 6 GB). Branch `vayu`.

**Status (2026-10-21): iOS 16.7.16 boots to the lock screen** on all eight cores with 5 GiB of RAM, power and volume keys, **touch** (Novatek NT36672C over GPI DMA, since 0168) and a live kernel log over USB (`sweetusb=2`, `tools/sweet-usbnet.sh`). Battery (PM8150B FG gen4), power button / WLED backlight and a load governor work too. GPU (Adreno 640, `bsd/dev/vayu_gpu.c`, `vayugpu=4`): GMU boot, HFI, GX power, zap shader via TZ, command processor and a GPU memory write work (2026-10-23); not yet wired to the renderer. Not yet: Wi-Fi, audio, camera. The first boot of a new root waits several minutes on the Apple logo while LaunchServices builds its database.

Kernel patch numbers: the vayu patches were written as 0140-0175 and renumbered to 0960-0995 when upstream's HackiOS release (topaz 0140-0152, 0900-0950) was merged; text below uses the original numbers.

What it took on vayu (kernel patches 0140-0150): the SM8150 platform switch (0140), CPU copy display (0141), USB device mode with an identity SMMU context for stream 0x140 (0142-0144; USB DMA faults had stalled UFS), the shim giving xnu the largest DTB /memory block (ABL: 0x80000000+0x3cc00000, 0xc0000000..0x200000000; was 192 MiB), SCTLR_EL1.SPAN on ARMv8.1+ cores (0149), and diagnostics (sampler kernel backtraces, running-thread xcall sampler, fast-fault loop detector).

Touch (0151-0169, `bsd/dev/vayu_touch.c`): SE17's firmware disables the FIFO interface, so SPI goes through the GPI DMA engine at `0xc00000` (GPII1). It needs an RPMh BCM `QUP0` vote, its own stage-1 SMMU context (TCR2 SEP_UPSTREAM) that maps only the DMA buffer for streams `0x7b6` (GPI) and `0x7a3` (QUP SE): the hypervisor turns S2CR bypass into fault, and an identity mapping lets a stray GPI write reset the phone. GPI interrupts are wired (INTSET 1). The chip (trim id `0c 00 00 72 66 03`) uses Novatek's NT36675 map: event buffer `0x22D00`, hw CRC with 3-byte lengths. Firmware `j20s_novatek_ts_fw0{1,2}.bin` (by panel 36/42 from the ABL cmdline) goes into the ADT (`tools/mkadt.py --touch-fw`), never into git.

## Why vayu is closer to sweet than to topaz

SM8150 and sweet's SM7150 are the same generation (Kryo 4xx = Cortex-A76 + A55 DynamIQ, ARMv8.2) and share their MMIO map: GIC, UFS, PS_HOLD, SPMI, splash framebuffer, MDSS, USB. So vayu needs none of topaz's SM6225 workarounds:

| | topaz (SM6225) | vayu (SM8150-AC) |
| --- | --- | --- |
| Boot CPU | Silver A53, **no 16K granule** -> shim hands off to Gold | CPU 0 (A55) **has** the 16K granule -> no hand-off |
| Cores for xnu | 4 Gold | all 8 (4x A55, 3x A76, 1x A76 prime), MPIDR `0x000`-`0x700` |
| Boot image | v4, payload appended to the Image | v2 + stock DTB, payload as ramdisk (as on sweet) |
| GIC | `0xf200000` / `0xf300000` | `0x17a00000` / `0x17a60000` |
| UFS | `0x4804000` | `0x1d84000` |
| Splash | `0x5c000000` | `0x9c000000` (region `0x2400000`) |
| MDSS | `0x5e00000` | `0xae00000` (same block offsets) |
| PS_HOLD / restart reason | `0x440b000` / `0xc12565c` | `0xc264000` / `0x146bf65c` |
| SPMI arbiter v5 | `0x1c40000` | `0xc440000` (obsrvr `0xe600000`, cnfg `0xc40a000`) |
| cpufreq-hw | gold `0xf523000` | `0x18323000`, `0x18325800`, `0x18327800` |

ARMv8.0 code from patch 0081 (no LSE, `-mcpu=cortex-a53`) still runs on the A55/A76, so one kernel serves both phones; patch 0140 selects the hardware at runtime from the ADT root compatible (`xiaomi,vayu`).

## Hardware facts (from the MIUI V14.0.3.0 dump)

Source: `boot.img` (header v2, kernel offset `0x8000`, ramdisk `0x1000000`, tags `0x100`, DTB `0x1f00000`, page 4096, os 13.0.0 / 2023-05), base DTB `SM8150P v2` (msm-id `0x169 0x20000`), `dtbo.img` entry `VAYU` (board-id `0x2f`).

- **RAM**: DDR from `0x80000000`. Fixed carve-outs: hyp `0x85700000`, xbl/aop/smem/removed `0x85d00000`-`0x8b700000`, camera/wlan/npu/adsp `0x8b700000`-`0x8e000000`, modem `0x8e000000`+`0x9600000`, video/slpi/ipa/gpu/spss/cdsp `0x97600000`-`0x9a500000`, cont_splash `0x9c000000`+`0x2400000`, qseecom `0x9e400000`+`0x1400000`, cdsp_sec `0xa4c00000`+`0x3c00000`, ramdump_fb (dtbo) `0xaf000000`+`0x1000000`. Kernel RAM starts at `0xb0000000`.
- **Timer**: 19.2 MHz, virtual timer PPI 3 = INTID 19 (as topaz).
- **Display**: panels `j20s 36 02 0a` (Tianma) and `j20s 42 02 0b` (CSOT), 1080x2400 DSC video mode, DSI0 `0xae94000`. SSPPs at MDSS `+0x5000/7000/9000/b000` (VIG0-3) and `+0x25000/27000/29000/2b000` (DMA0-3), CTL `+0x2000` step `0x200`, INTF_1 `+0x6b800`.
- **Touch**: Novatek NT36672C (`novatek,NVT-ts-spi`) on QUPv3 **SE17 SPI** `0xc80000`, reset GPIO 12, IRQ GPIO 122; firmware `j20s_novatek_ts_fw01.bin` (Tianma) / `fw02.bin` (CSOT) in vendor.
- **Keys**: power = PM8150 PON KPDPWR, volume down = PON RESIN, volume up = **PM8150 GPIO 6** (SID 0, `0xc500`), active low.
- **Battery**: PM8150B FG gen4 + SMB5, BQ25970 charge pump (not ported).
- **Wi-Fi**: WCN3998 (`qca,wcn3990` family, ath10k snoc) behind the modem; not ported.
- **USB**: DWC3 `0xa600000` (same as sweet), but the HS PHY is SNPS femto, not sweet's QUSB2.

## Build

Kernel (GitHub Actions on the fork):

```sh
gh workflow run xnu.yml -R grepfox/ios-vayu --ref vayu
gh run download RUN_ID -R grepfox/ios-vayu -n xnu-kernel -D xnu/out
```

Boot image. `VAYU_DTB` is the DTB section of the stock `boot.img` (in the dump: `boot/dtb.img`); it is Xiaomi's, keep it out of git. `MKBOOTIMG` points at AOSP `mkbootimg.py`. Without `aarch64-linux-gnu-gcc` the shim builds with clang + lld.

```sh
python3 tools/mkadt.py shim/out/vayu.adt --target vayu
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/vayu.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1'
TARGET=vayu VAYU_DTB=/path/to/boot/dtb.img MKBOOTIMG="python3 /path/to/mkbootimg.py" shim/build.sh ../shim/out/payload.bin
```

Without a payload argument, `build.sh` makes the **diagnostic shim**: red screen with the splash pipe, CPU and DTB facts, a read-only UFS GPT listing, then a reset back to fastboot after about 3 minutes.

## Bring-up order

1. **Diagnostic shim** (`fastboot boot`, RAM only, writes nothing). Photograph the red screen: `SPLASH pipe/src/stride/fmt`, `MMFR0 tg16`, `MIDR`, the `MEM0..` ranges, the UFS GPT lines (`userdata` LUN and size).
2. **Kernel with `cpus=1`, read-only, no root**: expect the xnu console on the splash and a panic at root mount (no iOS root yet). Confirms the ADT, GIC, timer and RAM.
3. **iOS root in `userdata`** (erases Android user data; ask before flashing), then `ufsrw=1 rootrw=1`, then `cpus=8`.

## What 0140 changes on vayu

- `SweetCPU`: sweet's 8-core layout and boot MPIDR 0.
- `AppleCLCD`: MDSS `0xae00000`; picks the SSPP whose source is the boot framebuffer and the CTL that drives INTF_1, and flushes that pipe's bit.
- Keys: same read-only SPMI poller, SM8150 arbiter and PM8150 GPIO 6.
- Off on vayu: topaz FT5452 touch, I2C battery gauge (static battery instead), topaz cpufreq (the shim does it), TopazModem.

## Next

- Battery (PM8150B FG gen4 over SPMI), brightness (DCS 0x51 on DSI0).
- Wi-Fi: the topaz modem/WLAN path with SM8150 MSS/WCN3998 addresses.
