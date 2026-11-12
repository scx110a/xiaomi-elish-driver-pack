# Xiaomi Mi Pad 5 Pro (elish) — Linux Driver Package<br>Sensors · Fingerprint · GNSS · Camera

[中文说明](README.md)

A repeatable **driver / system-integration package** for the **Xiaomi Mi Pad 5 Pro**
(Qualcomm **SM8250 / elish**, CSOT panel, Wi-Fi model) running Armbian Linux.
It bundles four subsystems on one shared kernel / DTB / boot image:

- **Sensors (SLPI/SSC)** — ambient light, accelerometer, rotation-vector orientation, compass heading;
- **Fingerprint (Goodix GF3626)** — fprintd enroll/verify, GDM lock/login, sudo fingerprint;
- **GNSS (Airoha AG3335)** — QUP5 UART + `airoha-gnssd` + gpsd + GeoClue2;
- **Camera (OV8856 / OV13B10)** — full-resolution photo/record, GPU debayer, Venus H.264/HEVC encode,
  two V4L2 virtual cameras, PipeWire/portal, rear autofocus.

- **Version**: 2026.11.13
- **Device**: Xiaomi Mi Pad 5 Pro (elish, CSOT panel, Wi-Fi)
- **System**: Armbian 26.8.14 (Ubuntu 24.04 noble) + kernel `6.18.44-current-sm8250`
- **License**: original parts of this package are **MIT** (see `LICENSE`); upstream components keep their own licenses.

> **Packaging policy**: this package contains **code, patches, system configuration, scripts and docs only**.
> It does **not** ship firmware, device data, logs, fingerprint templates, keys/serials, PAM backups,
> or prebuilt userspace binaries. The only bundled binary is the **model-level combined DTB**
> (no per-unit private information; rationale in `kernel/dtb/README.md`).
> Required vendor data/firmware is extracted from **your own device / official distributions** —
> see `docs/DEVICE-DATA-EXTRACTION.md`.

---

## Contents

- [Support status at a glance](#support-status-at-a-glance)
- [Tested system versions](#tested-system-versions)
- [Measured results](#measured-results-2026-11-13-same-device)
- [Package layout](#package-layout)
- [Installation](#installation)
- [Building](#building)
- [Verification](#verification)
- [Known limitations](#known-limitations)
- [Credits / upstream](#credits--upstream)
- [License](#license-mit)

---

## Support status at a glance

> ✅ works (tested) · ⚠️ works with known limitations · 🚫 limited / incomplete · — not present on this device

| Subsystem | Component | Status | Notes |
|---|---|---|---|
| **Sensors** | SLPI/SSC stack: accelerometer, ambient light, rotation vector, compass heading | ✅ | Auto-brightness / auto-rotate work; sensors ready ~22 s after boot (after M2) |
| Sensors | Boot-time acceleration (L2/M1/M2) | ✅ | Early service start + writable hexrpc registry: PD init 35 s → 8 s |
| Sensors | Suspend/resume recovery | ✅ | Sleep hook + worker; iio re-claims sensors ~10 s after resume (after M2) |
| Sensors | Raw gyroscope / magnetometer axes / proximity (SAR) | 🚫 | Not exposed by userspace (libssc/udev layer); not a hardware-driver gap |
| **Fingerprint** | Goodix GF3626: fprintd enroll/verify, GDM lock/login, sudo | ✅ | TA must be extracted from your own device; TA-refusal watchdog included |
| **GNSS** | Airoha AG3335: QUP5 UART + PAIR + gpsd + GeoClue2 | ✅ | NMEA @ 1 Hz; GeoClue location + compass heading; Pure Maps validated |
| **Camera** | Front OV8856 3280×2464 / rear OV13B10 4208×3120 full-res photo/record | ✅ | GPU debayer + Venus HEVC/H.264 at ~30 fps; two V4L2 virtual cameras |
| Camera | Rear AF/AE/AWB, CN3927V VCM, EEPROM 24c64 | ✅ | Software AF sweep writes the focus sysfs |
| Camera | PipeWire / xdg-desktop-portal / Snapshot | ✅ | Two `media.role=Camera` nodes; portal frame capture validated |
| Camera | Hi556 depth sensor | 🚫 | Blocked on PM8150A L1/L2 power sequencing (work in progress) |
| Camera | Front and rear streaming simultaneously | 🚫 | CAMSS/DTB resource limit; one camera at a time |
| **Kernel/System** | Combined DTB (sensor vmids + fingerprint node + GNSS QUP5 UART + camera nodes) | ✅ | Model-level, no per-unit private data; see `kernel/dtb/README.md` |
| Kernel | fastrpc SDSP VMID / sensors-PD, QSEECOM/TEE, mdt_loader, qcom_scm, goodix_fp_spi | ✅ | Fingerprint/sensor kernel layer |
| Kernel | LPASS pinctrl clock-timeout defer (pogo keyboard/USB random failures) | ✅ | Stand-alone patch `kernel/patches/0002-…` |
| Kernel | SMB5 Type-C legacy/100 mA resume hang | ✅ | Verified with 3 suspend/resume cycles with charger attached |
| System | Display/touch/pen/hall/battery/audio/Wi-Fi/BT/USB HID/GPU/UFS/PCIe/Venus | ✅ | Platform baseline; see `docs/HARDWARE-STATUS.md` |
| System | Screen orientation / tablet mode / auto-rotate configuration | ⚠️ | `tablet_mode=on/off/auto`, `rebuild_mode`; depends on the cover hall switch |
| System | Pogo keyboard after suspend/resume | ⚠️ | Known kernel USB PHY resume issue; reboot/re-plug recovers; userspace deliberately does not touch it |
| System | Charging | ⚠️ | Baseline ~1.95 A; quick-charge pump / 33 W+ not validated (no original charger) |
| — | 5G variant | — | This unit is Wi-Fi only (5G variant is outside this package's test scope; other misc hardware is absent on all variants) |

The authoritative hardware baseline is [`docs/HARDWARE-STATUS.md`](docs/HARDWARE-STATUS.md).

---

## Tested system versions

| Item | Value |
|---|---|
| Device | Xiaomi Mi Pad 5 Pro (codename **elish**, CSOT panel, Wi-Fi) |
| SoC | Qualcomm Snapdragon 870 (**SM8250**) |
| Distribution | **Armbian 26.8.14 noble** (Ubuntu 24.04 LTS userspace) |
| Kernel | **`6.18.44-current-sm8250`** (Armbian sm8250 baseline + this package's patches; see `kernel/README.md` for BTF/ABI requirements) |
| Boot | The Linux slot is read from `abl_boot_partition_label` in `/boot/armbianEnv.txt` (`boot_b` on this unit; the scripts follow it and **never hardcode** it); only that slot is written, the **Android slot is not touched** (Android is `boot_a` here; your layout may differ) |
| Panel | CSOT (combined DTB is `sm8250-xiaomi-elish-csot.dtb`; BOE variant untested) |
| Kernel source (to build) | Matching Armbian sm8250 tree (e.g. `/var/tmp/kbuild-src`); not included |
| Dependencies | Ubuntu 24.04 packages; the installer's `--deps` installs them |

> Sensor data/firmware, fingerprint TA, GNSS flash images and camera calibration dumps are **not**
> included; extract them from your own device / official distributions —
> see [`docs/DEVICE-DATA-EXTRACTION.md`](docs/DEVICE-DATA-EXTRACTION.md).

---

## Measured results (2026-11-13, same device)

One elish unit (CSOT), Armbian noble + `6.18.44-current-sm8250`, one clean boot + one lid-close resume.

### Boot / sensors / resume

| Metric | Measured |
|---|---|
| Desktop ready (systemd `Startup finished`) | **38.3 s** (kernel 3.2 s + userspace 35.0 s; graphical at 34.8 s userspace) |
| Boot → sensors usable | **21.65 s** (`hexrpc-boot.log [start] up=21.65 sensors OK`; was ~79 s, −73%) |
| PD registry initialisation (SLPI start → sensors OK) | **8.2 s** (~36–42 s before the M2 writable registry) |
| iio state | active, single instance, **NRestarts=0** |
| Lid close → resume → sensors re-claimed | **≈10.2 s** (s2idle; was ~35–40 s) |
| Auto-rotate after resume | works (tested) |
| Sensor self-check | `13 OK`; 1 PDM-crash item is a **known post-resume** artifact (0 after a clean reboot) |

### Fingerprint

| Metric | Measured |
|---|---|
| `fprintd-list` | Goodix QSEE match-on-chip, enrolled `left-index-finger` |
| Enroll / verify | ✅ (`fprintd-enroll` / `fprintd-verify`) |
| GDM lock / login | ✅ (`enable-fingerprint-authentication=true`) |
| sudo fingerprint | ✅ (`pam_fprintd` 8 s timeout → password fallback; verified on 2026-11-13) |
| TA-refusal recovery | ✅ `fp-ta-watchdog` reloads the loader automatically |
| Fingerprint self-check | **OK=20 / WARN=0 / FAIL=0** |

### GNSS

| Metric | Measured |
|---|---|
| Chain | `/dev/ttyHS1` (921600) → `airoha_gps.ko` → `airoha-gnssd` → `/dev/ttyGNSS0` → gpsd |
| NMEA | 1 Hz, 10491+ sentences forwarded; gpsd TPV OK (indoor `mode=1` is normal) |
| GeoClue2 | `gps-nmea-share` → `/run/gps-share.sock`; GeoClue location available |
| Compass | `CompassHeading` ≈176–177°; GeoClue heading available |
| Map apps | GNOME Maps (location); Pure Maps 3.5.1 (location + compass) validated |

### Camera

| Metric | Measured |
|---|---|
| Front | 3280×2464 PNG; HEVC 20 Mbps recording **30.05 fps** |
| Rear | 3120×4208 PNG; HEVC 20 Mbps recording **29.96 fps** |
| GPU debayer | front 11.0–12.1 ms/frame; rear 18.7–19.8 ms/frame (p95 ≤14.7 / ≤22.1) |
| Venus encode throughput (unthrottled) | 3280×2464 HEVC **135 fps**; 4208×3120 HEVC **90.6 fps** |
| Virtual cameras | `/dev/video2` (front, NV12 3280×2464), `/dev/video20` (rear, NV12 3120×4208) |
| PipeWire / portal | two `media.role=Camera` nodes; portal `OpenPipeWireRemote` + Snapshot validated |
| Rear software AF | best indoor focus = 640 (range 0–1023) |
| Camera self-check | **OK=23 / WARN=0 / FAIL=0** |

### Combined self-check (`install/check-all.sh`)

| Item | Result |
|---|---|
| Combined DTB | ✅ pass |
| Sensor stack | 13 OK / 1 FAIL (known post-resume PDM crash) |
| Fingerprint stack | 20 OK / 0 FAIL |
| GNSS stack | normal (polkit rule only WARNs when run as non-root) |
| initramfs firmware invariant | ✅ no SLPI firmware inside |
| Camera stack | 23 OK / 0 FAIL |
| apt state | recommended: hold `libfprint-2-2` (upgrades overwrite the self-built library) |

> For complete results run `sudo ./install/check-all.sh`.

---

## Package layout

```
README.md / README.en.md / QUICKSTART.md / INSTALL.md / LICENSE
kernel/        unified kernel: driver sources/patches, config, initramfs hook,
               model-level combined DTB, one-shot kernel build+install script,
               Linux boot-slot rebuild script (auto-detected)
sensors/       sensor subpackage: systemd/udev/D-Bus integration, boot/resume/diag
               scripts, hexrpc/libssc/iio-sensor-proxy patches + build notes,
               data extraction scripts, docs
fingerprint/   fingerprint subpackage: qsee-supplicant/libfprint sources + patches,
               udev/systemd/PAM config, TA extraction, install/verify/uninstall, docs
gnss/          GNSS subpackage: airoha_gps driver source, airoha-gnssd/gps-nmea-share,
               systemd/gpsd/GeoClue integration, protocol docs, flash-repair extraction
camera/        camera subpackage: elish-camera GPU pipeline (full-res front/rear,
               GPU debayer, Venus encode, virtual cameras, autoserve, autofocus),
               helper scripts, udev/autostart/resume hooks, install.sh + verify.sh, docs/
install/       installer: turnkey.sh, extract-all.sh, build-userspace.sh,
               check-all.sh, file map
docs/          hardware baseline, TODO, upgrade impact, hardening, data extraction guide
```

---

## Installation

> See [`QUICKSTART.md`](QUICKSTART.md) for the one-command flow and [`INSTALL.md`](INSTALL.md)
> for manual, per-step instructions. All installers support `--help` and `--dry-run`;
> already-installed components are skipped automatically (`--force` rebuilds).

### 0. Prerequisites

- Xiaomi Pad 5 Pro (elish, CSOT) running Armbian noble;
- root (sudo). If passwordless sudo was removed (recommended hardening), authorize with
  **fingerprint or password**;
- two device files (not shipped): an Android vendor image (e.g. `vendor_a.img`, read-only dump)
  and the pMOS SLPI firmware package `firmware-xiaomi-elish-slpi-*.apk`;
- fingerprint TA (optional; reused from `/lib/firmware/gfenu.*` if already installed);
- network (userspace components are cloned from upstream and built; kernel needs your own matching source tree).

### 1. One-command full install (recommended)

```bash
cd ~/xiaomi-elish-linux-v1-final

# preview (writes nothing)
./install.sh --dry-run --android /path/vendor_a.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk

# install: deps → extract → data root/firmware invariant → build userspace
#          → system integration → self-check
sudo ./install.sh --android /path/vendor_a.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk --yes

# optional: build and install the kernel too (writes the Linux boot slot; auto-detected; explicit opt-in; auto-backup)
sudo ./install.sh --with-kernel --kernel-src /var/tmp/kbuild-src --write-boot --yes

sudo reboot
sudo ./install/check-all.sh
```

### 2. Per-component installation

#### 2.1 Kernel / DTB / initramfs (optional; requires kernel source)

```bash
sudo ./kernel/build-and-install.sh --kernel-src /var/tmp/kbuild-src --write-boot
# dry run: --dry-run ; reuse existing build: --skip-build
```
**The boot slot is not hardcoded**: it is read from `abl_boot_partition_label` in
`/boot/armbianEnv.txt` (`boot_b` on this unit; installs on `boot_a` are followed too), and
`--slot <label>` is only an assertion.

It applies kernel patches, integrates QSEECOM/mdt_loader/qcom_scm, enforces
`q6v5_pas auto_boot=true`, builds Image/modules, installs fastrpc/goodix_fp_spi/airoha_gps,
installs the combined DTB and initramfs hook, backs up and rebuilds the Linux boot slot
(auto-detected via armbianEnv; **never touches the Android slot**). Manual steps: `kernel/README.md`.

#### 2.2 Sensors

```bash
# one-shot (extract + data root + system integration + self-check)
sudo ./install.sh --android <vendor.img> --pmos <slpi.apk> --skip-build --yes

# or step by step
sudo ./install/extract-all.sh --android <vendor.img> --pmos <slpi.apk>
sudo ./sensors/install/install-data.sh --android /var/tmp/elish-build/extracted/android \
     --armbian /var/tmp/elish-build/extracted/armbian --firmware /var/tmp/elish-build/extracted/pmos
sudo ./sensors/install/install-system-files.sh
sudo ./sensors/install/verify.sh
```
Userspace binaries (hexagonrpcd + M1/M2 patches, libssc, iio-sensor-proxy) are patched and built by
`install/build-userspace.sh`; manual commands in `sensors/userspace/*/README.md`.

#### 2.3 Fingerprint

```bash
# TA (from your own device / adb / firmware image)
sudo ./install/extract-all.sh --fingerprint-adb --skip-sensors
sudo install -m0644 /var/tmp/elish-build/extracted/fingerprint-ta/gfenu.* /lib/firmware/

# userspace (qsee-supplicant, libfprint, fprintd, integration)
sudo ./install/build-userspace.sh --skip-hexrpc --skip-libssc --skip-iio
sudo ./fingerprint/install/install-fingerprint-files.sh
sudo ./fingerprint/install/verify-fingerprint.sh "$USER"
```
Details (PAM/sudo fingerprint, watchdog, rollback): `fingerprint/INSTALL.md`.

#### 2.4 GNSS

```bash
sudo ./gnss/install/install.sh     # builds + installs airoha-gnssd and gpsd/GeoClue integration
sudo ./gnss/install/verify.sh
```
`airoha_gps.ko` is built by the kernel step (or `--with-kernel`).

#### 2.5 Camera

```bash
sudo ./camera/install.sh           # builds elish-camera + scripts/udev/autostart/resume + orientation perms
bash ./camera/elish-camera/verify.sh
```

#### 2.6 Security hardening (optional, recommended)

Disable GDM auto-login and passwordless sudo, keep fingerprint authorization
(both sudo and login): see `docs/HARDENING.md`.

### 3. Device data / firmware extraction (not shipped)

```bash
sudo ./install/extract-all.sh --android <vendor.img> --pmos <slpi.apk> [--fingerprint-adb] [--out DIR]
```
It validates registry=155 / config=40 / proto=79 / slpi.mbn md5. Full list and manual methods:
[`docs/DEVICE-DATA-EXTRACTION.md`](docs/DEVICE-DATA-EXTRACTION.md).

---

## Building

- The one-command installer clones upstream sources, applies this package's patches, builds and installs
  them (source cache: `/var/tmp/elish-build`).
- Manual build notes:
  - kernel: `kernel/README.md` (steps A/B, config checks, ABI triplet)
  - hexrpc: `sensors/userspace/hexrpc/README.md` (v0.3.2 + 0001/0003/0004/0005)
  - libssc / iio: `sensors/userspace/*/README.md`
  - fingerprint: `fingerprint/INSTALL.md` (qsee-supplicant, libfprint goodix-qsee)
  - camera: `make -C camera/elish-camera`
- Verified source versions/commits and post-install sha256 references: `VERSIONS.md` and each subpackage.

---

## Verification

```bash
sudo ./install/check-all.sh         # combined self-check (DTB/sensors/fingerprint/GNSS/camera/initramfs)
sensor-status                       # sensor overview (services/firmware path/PDM/iio/values)
bash camera/elish-camera/verify.sh  # camera stack
./kernel/compat-check.sh --src <new kernel source tree>   # read-only patch/copy compatibility dry-run
sudo ./fingerprint/install/verify-fingerprint.sh "$USER"
sudo ./gnss/install/verify.sh
sudo ./kernel/dtb/verify-combined-dtb.sh
```

---

## Known limitations

- Sensor PD occasionally hangs silently (~**1/14** clean boots): firmware-side; a clean reboot recovers.
- One PDM crash after suspend/resume is a known artifact (sensor data still works; 0 after a clean reboot).
- Raw gyroscope / magnetometer axes / proximity are not exposed (userspace libssc/udev work).
- Camera: Hi556 depth sensor not working; front and rear cannot stream simultaneously; front focus is fixed;
  Flatpak/COSMIC portal-backend limitations.
- Pogo keyboard may stop working after resume (kernel USB PHY resume); **do not** use
  `authorized`/`USBDEVFS_RESET`/controller rebind.
- Fingerprint TA must be extracted yourself; upgrading `libfprint-2-2` overwrites the self-built library
  (hold recommended); kernel/firmware upgrades require redoing the package per `docs/UPGRADE-IMPACT-ALL.md`.
- sudo-fingerprint PAM is security-sensitive; ensure password fallback works before enabling.

---

## Credits / upstream

This package is an integration of many upstream projects plus substantial reverse-engineering,
adaptation and validation. Thanks to:

### Upstream code

| Project | Role | License |
|---|---|---|
| [Linux kernel](https://git.kernel.org/) / [mainline](https://github.com/torvalds/linux) | kernel base and driver frameworks | GPL-2.0 |
| [Armbian build](https://github.com/armbian/build) / armbian-firmware | sm8250 baseline, config, sensor firmware data | GPL-2.0 / respective |
| [linux-msm/hexagonrpc](https://github.com/linux-msm/hexagonrpc) | SLPI/SSC FastRPC file service (hexagonrpcd 0.3.2) | GPL-2.0 |
| [mmcallister/libssc](https://codeberg.org/mmcallister/libssc) | SSC sensor discovery/transport (0.4.4) | LGPL-2.1 |
| [hadess/iio-sensor-proxy](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy) | IIO → D-Bus SensorProxy | GPL-2.0-or-later |
| [wrobelda/libfprint](https://github.com/wrobelda/libfprint) (goodix-qsee) | Goodix QSEE fingerprint driver | LGPL-2.1 |
| [wrobelda/qsee-supplicant](https://github.com/wrobelda/qsee-supplicant) | QSEE supplicant / app-loader | GPL-2.0 |
| [postmarketOS](https://gitlab.com/postmarketOS) (device-xiaomi-elish / firmware packages) | SLPI single-file firmware source | respective (pMOS packaging) |
| PipeWire / WirePlumber | camera/audio media pipeline | MIT / MIT |
| [v4l2loopback](https://github.com/v4l2loopback/v4l2loopback) | virtual cameras | GPL-2.0 |
| Mesa / EGL / GLES / libdrm / GBM | GPU debayer rendering | MIT / respective |
| GeoClue / gpsd / Pure Maps | location stack and map apps | LGPL / BSD / GPL |
| quectel-open-source/GNSSTool and other GNSS references | AG3335 protocol reference | respective |
| Community hexrpc writable-registry patches (suckbluefrog / gennro / Armada OS packaging) | M2 writable sensor registry | with upstream patches |

### People / AI

- **caitming** — project owner, device experiments, kernel/driver integration, testing and maintenance;
- **DeepSeek Harness AI Agent (deepseek-flash)** — read-only reverse-engineering analyses,
  implementation of the L2/M1/M2 boot acceleration and R1/R2 resume work, adaptation of the
  hexrpc writable-registry patches, camera/fingerprint/GNSS integration validation,
  installer and documentation (including this README).

---

## License (MIT)

Original parts of this package (installer, system configuration, scripts, documentation) are released
under the **MIT License** — see [`LICENSE`](LICENSE).

> Upstream code/patches (kernel and driver patches, hexagonrpc, libssc, iio-sensor-proxy, libfprint,
> qsee-supplicant, PipeWire, v4l2loopback, etc.) **retain their original licenses**
> (mainly GPL-2.0 / LGPL-2.1 / MIT). Please comply with the respective upstream licenses
> when redistributing or modifying.

---

## Maintenance

- Hardware baseline: `docs/HARDWARE-STATUS.md` (authoritative)
- TODO / known issues: `docs/TODO.md`
- Upgrade impact: `docs/UPGRADE-IMPACT-ALL.md`, `sensors/docs/UPGRADE-IMPACT.md`
- Kernel upgrade risk assessment and checklist: `docs/KERNEL-UPGRADE-RISK.md`
- Rollback/recovery: `docs/`, `kernel/README.md`; kernel/system installers back up the Linux boot slot and key configs automatically.
