# SVP7500 + Intel IPU7 Camera Fix Pack

Restore RGB camera functionality on Linux for laptops with the Synaptics SVP7500 CVS bridge (USB `06CB:0701`) and Intel IPU7 (Panther Lake / Lunar Lake).

> **About this fork** (of [jibsta210/svp7500-camera-fix-pack](https://github.com/jibsta210/svp7500-camera-fix-pack)) — adds two fixes on top of upstream v0.7, developed on a **Dell Pro 14 Plus PB14250** (Lunar Lake, OV05C10) running **Ubuntu 26.04** / kernel 7.0.0-1008-oem:
>
> 1. **ipu-bridge: OVTI05C1 advertises both 480MHz + 900MHz link frequencies.** The ov05c10 driver requires both or probe fails with -EINVAL — with the 480MHz-only entry the sensor never binds on the PB14250.
> 2. **ov05c10: survives i2c client re-creation (swnode self-heal).** Any SVP7500 USB re-enumeration (suspend/resume quirks, chip reset, flaky boot) destroys and re-creates the sensor's i2c client; an i2c-core/swnode kernel bug then severs — and after a couple of cycles outright frees — the ipu-bridge fwnode graph, leaving the camera dead until reboot. The probe now re-attaches the swnode and compensates the refcount (full analysis in the commit message).
>
> With these, the PB14250 status upgrades from "install confirmed, streaming TBD" to **streaming confirmed** (≤1080p, software ISP, 17–31 fps; native 2880×1808 hangs — always pass an explicit size, e.g. `cam --camera=1 -s width=1280,height=720`). Also confirms the pack works on **Ubuntu** (listed as untested below). Ubuntu note: the system uses **dracut**, so `ipu7_fw.bin` must be added to the initramfs (`install_items+=" /lib/firmware/intel/ipu/ipu7_fw.bin "` in `/etc/dracut.conf.d/`) or the IPU7 probe fails -ENOENT at early boot.
>
> Bonus for SVP7500 owners: the re-enumeration recovery enables turning the latched privacy LED off without a reboot — power-cycle the bridge's USB port, wait ~1–3 min of bridge fw warmup, reprobe the sensor. The latch is firmware-internal ("has streamed since last power-loss") and clears only on chip power-loss; ownership release and SET_HOST_IDENTIFIER don't move it on this hardware (protocol 1.0). Scripted as **[`scripts/camera-led-off.sh`](scripts/camera-led-off.sh)** (`sudo`, camera offline ~2–4 min, auto-detects the bridge's hub port). ⚠️ It deliberately polls fw readiness with a *raw* userspace i2c read and binds the driver exactly once — retry-binding during warmup hard-froze a PB14250 (regulator over-put on port-cycle teardown + failed-probe devres error path + re-probe → lockup). Don't hand-roll a bind-retry loop.

> ⚠️ **Testing scope:** All testing was done on **CachyOS** with a custom-built **linux-cachyos-susfix 7.0.5** kernel (CachyOS's `linux-cachyos` source plus our own kernel patch for the `ipu7_pci_remove` ordering bug). The DKMS modules themselves should be distro-agnostic (just C code against the kernel API), so Fedora / Arch / Debian / Ubuntu *should* work — **but we haven't tested those personally.** If you try it on another distro, please report back via the issue tracker.
>
> Specifically untested:
> - Fedora 43/44 (regular workstation) — should work, kernel version compatible
> - Fedora 44 Silverblue — DKMS on an immutable OS is finicky; you'll likely need `rpm-ostree install dkms kernel-devel` followed by a reboot before running our installer. Layering DKMS modules on Silverblue is known-awkward.
> - Ubuntu / Debian — should work with `linux-headers-$(uname -r)`
> - Stock Arch / EndeavourOS — should work; we run a custom kernel but the DKMS modules don't depend on those custom patches

## Affected hardware

Confirmed working:
- Dell XPS 16 DA16260 (Panther Lake) on CachyOS — primary dev hardware
- Dell XPS 16 DA16260 (Panther Lake) on Fedora 44 Silverblue — independently confirmed by @tverhaeghe (intel/vision-drivers#37)
- Dell Pro 14 PB14250 (Lunar Lake) on Arch — install confirmed by @acmodeu (intel/ipu7-drivers#26), streaming TBD

Likely also helps (untested by us, please report):
- Dell Latitude 9440 / 7440 / 7450
- Lenovo ThinkPad X9
- Dell Pro Max 16 MA16250
- ASUS Vivobook X1407Q (Snapdragon X — different host, same sensor)
- Any laptop with Intel IPU7 + Synaptics SVP7500 + OV08x40/HM1092 sensors

## What you get

| Status | Camera |
|--------|--------|
| ✅ Works | RGB front-facing camera (OV08x40) — for video calls, photos, etc. |
| ⏳ In progress | IR camera (HM1092) — for Windows Hello-style face auth |

## Quick install

> 🛟 **Back up your kernel + initramfs first.** DKMS modules can occasionally break boot if they fail to load and something on the boot path depends on them. These camera modules shouldn't be on the boot path, but be safe:
>
> ```bash
> sudo cp /boot/vmlinuz-$(uname -r){,.pre-svp7500-fix.bak}
> sudo cp /boot/initramfs-$(uname -r).img{,.pre-svp7500-fix.bak}  # Fedora/RHEL
> # OR
> sudo cp /boot/initrd.img-$(uname -r){,.pre-svp7500-fix.bak}     # Debian/Ubuntu
> # OR (Arch / CachyOS)
> sudo cp /boot/initramfs-linux*.img{,.pre-svp7500-fix.bak}
> ```
>
> If you're on Btrfs with `snapper`, just take a snapshot:
> `sudo snapper create --description "pre-svp7500-fix"`
>
> **Recovery if something breaks:** boot to your previous kernel entry (most bootloaders keep one), then:
> ```bash
> sudo dkms remove -m intel-cvs -v 1.0 --all
> sudo dkms remove -m hm1092 -v 1.0 --all
> sudo dkms remove -m int3472-patched -v 1.0 --all
> sudo dkms remove -m ipu-bridge-patched -v 1.0 --all
> sudo rm -f /etc/udev/rules.d/99-svp7500-no-autosuspend.rules
> sudo reboot
> ```

Requires DKMS and kernel headers:
```bash
# Fedora
sudo dnf install dkms kernel-devel

# Arch / CachyOS
sudo pacman -S dkms linux-headers

# Debian / Ubuntu
sudo apt install dkms "linux-headers-$(uname -r)"
```

Then:
```bash
sudo ./install.sh
sudo reboot
```

After reboot:
```bash
cam --list                                # should show 1 camera (your front-facing RGB)
cam --camera=1 --capture=5 --file=/tmp/test.raw    # capture 5 frames
```

If the camera does not show up, check `sudo dmesg | grep -E 'Intel CVS|hm1092|ov08x40'` and report results to https://github.com/intel/vision-drivers/issues/37

## What's in here

5 components, each fixes a different piece of the broken stack:

### 1. `intel-cvs` DKMS (the headline fix)
- Patches `cvs_init()` to remove a buggy `IRQF_ONESHOT` flag from `devm_request_irq()`. The flag was meaningless on a non-threaded handler and caused a kernel WARNING. **More importantly: it made IRQ delivery from the SVP7500 unreliable, which is why the bridge wedges itself after brief idle periods.**
- Adds verbatim Windows-format MIPI config payloads (RGB + IR), captured from USBPcap traces of Windows on identical hardware.
- Exposes a sysfs `cmd` interface (`echo state > cmd`, `echo mipi-ir > cmd`, etc.) for runtime experiments.

### 2. `int3472-patched` DKMS
- Adds support for GPIO type `0x02` (IR LED) — needed for HM1092 sensors.
- Adds an SSDB `controllogicid` fallback walk through the ACPI namespace. Required on USBIO platforms (where the `_DEP` chain is broken and `acpi_dev_get_next_consumer_dev()` returns NULL).

### 3. `ipu-bridge-patched` DKMS
- Adds `HIMX1092` to the `supported_sensors[]` array so the kernel actually enumerates the IR sensor.

### 4. `hm1092` DKMS
- New v4l2-subdev driver for Himax HM1092.
- 198-register init sequence reverse-engineered from a Windows USBPcap during Hello face auth.
- Lazy IR LED management: LED is OFF when the sensor is in standby, only turns ON during streaming. Matches Windows behavior. Stock implementations leave the LED ON 24/7 after module load.

### 5. `ov05c10` DKMS
- RGB sensor driver for boards that pair the SVP7500 bridge with the **OV05C10** (`OVTI05C1`) sensor — e.g. the **Dell Pro Plus 14 PB14250** — instead of the OV08x40 used on the DA16260.
- The driver is the out-of-tree Intel one from [`intel/ipu6-drivers`](https://github.com/intel/ipu6-drivers) (`drivers/media/i2c/ov05c10.c`); it was never mainlined, so affected boards have no RGB camera until it's installed. The bridge half already enumerates `OVTI05C1` — this supplies the missing sensor driver.
- Packaged as DKMS so it survives kernel upgrades. Builds clean against 6.18 / 7.0 / 7.1.

### 6. udev rule
- Disables USB autosuspend on the SVP7500 device. Bridge firmware appears to have issues with power state transitions; keeping it always-on prevents some failure modes.

## Why this needed reverse engineering at all

The Synaptics SVP7500 is a proprietary MIPI bridge chip. Synaptics has not published its command reference publicly. Combined with the Intel IPU7 staging-driver stack (also fairly opaque), the camera "just doesn't work" on most Linux distros for affected laptops.

This fix pack is the result of three months of community reverse-engineering: USBPcap captures from Windows installs, Ghidra analysis of `Vision.sys`, kernel-side patches, and a lot of iteration.

The remaining mystery — IR camera streaming — appears to require additional bridge commands not visible in standard USBPcap captures. Investigation continues at https://github.com/intel/vision-drivers/issues/37

## Uninstall

```bash
sudo dkms remove -m intel-cvs -v 1.0 --all
sudo dkms remove -m hm1092 -v 1.0 --all
sudo dkms remove -m int3472-patched -v 1.0 --all
sudo dkms remove -m ipu-bridge-patched -v 1.0 --all
sudo rm -f /etc/udev/rules.d/99-svp7500-no-autosuspend.rules
sudo rm -rf /usr/src/intel-cvs-1.0 /usr/src/hm1092-1.0 \
            /usr/src/int3472-patched-1.0 /usr/src/ipu-bridge-patched-1.0
sudo udevadm control --reload-rules
sudo reboot
```

## License

The DKMS modules contain code from Intel (`intel-cvs`, `ipu-bridge`), Himax (sensor reference), and original work on top. License terms inherit from each upstream component — primarily GPL-2.0.

## Credits

- @jibsta210 — patch development, reverse engineering, testing
- @tverhaeghe — USBPcap traces from Windows on matching hardware (the key dataset)
- Intel `vision-drivers` and `ipu7-drivers` upstream maintainers — for the base code we patched
- Hans de Goede (@hdegoede) — Linux mainline IPU6/7 camera maintainer
