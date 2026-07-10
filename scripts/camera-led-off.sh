#!/bin/sh
# camera-led-off — turn the latched SVP7500 privacy LED off without a reboot.
#
# On boards where the privacy LED is a one-way firmware latch (lights at the
# first camera stream, normally stays on until shutdown — e.g. Dell Pro 14
# Plus PB14250), real power-loss of the SVP7500 bridge is the only thing
# that clears it. A GPIO reset clears the LED too, but wedges the chip's
# bulk transport identically, so the USB port power-cycle is strictly better.
#
# Method (proven on a PB14250, 2026-07-04):
#   1. Power-cycle the bridge's USB port (usbX-portN/disable 1 -> 0).
#   2. Wait out the chip's internal fw init (~1-3 min; sensor i2c times out
#      with -110 until ready — the same warmup happens at boot, hidden by
#      deferred probe retries).
#   3. Bind ov05c10 once. Requires the swnode self-heal patch from this
#      pack's ov05c10 (client re-creation poisons the fwnode graph
#      otherwise). isys then rebinds the subdev automatically.
#
# ⚠️ Do NOT poll readiness by retrying the driver bind. A failed kernel
# probe (-110 during warmup) followed by a re-bind hard-froze a PB14250
# (2026-07-06): the port-cycle teardown fires a regulator over-put WARNING
# (regulator_put <- devm_regulator_release <- i2c_del_adapter <-
# usbio_i2c_remove) and the re-probe over that poisoned state locks the
# machine (suspected use-after-free). This script polls with a RAW
# userspace i2c read instead (no kernel probe path) and binds exactly once.
# If the single bind fails, rerun the whole script (fresh port cycle) or
# reboot — never retry the bind by hand.
#
# ⚠️ Never unbind the intel-ipu7 PCI driver — that also hard-hangs.
#
# LED semantics after this: OFF until the next actual camera stream
# (sensor probe does not latch it), then latched again until the next run.
#
# Requires: this pack's ov05c10 DKMS (with swnode self-heal); python3.
# Optional: set CAMERA_RELAY_SERVICE if a service holds the camera open
# (defaults to ipu7-cam-relay; silently skipped when absent).
# Run with sudo. Camera is offline for the duration (~2-4 min).
set -u

[ "$(id -u)" = 0 ] || { echo "camera-led-off: must run as root (sudo)"; exit 1; }

SENSOR=i2c-OVTI05C1:00
SENSOR_ADDR=0x10
DRV=/sys/bus/i2c/drivers/ov05c10
RELAY=${CAMERA_RELAY_SERVICE:-ipu7-cam-relay}

# ---- locate the SVP7500 (06cb:0701) and its hub port ----------------------
BRIDGE=
for d in /sys/bus/usb/devices/*/idVendor; do
    dir=$(dirname "$d")
    if [ "$(cat "$d")" = "06cb" ] && [ "$(cat "$dir/idProduct" 2>/dev/null)" = "0701" ]; then
        BRIDGE=$dir
        break
    fi
done
[ -n "$BRIDGE" ] || { echo "FAILED: no SVP7500 (06cb:0701) on USB"; exit 1; }

DEVNAME=$(basename "$BRIDGE")            # e.g. 3-3 (root hub) or 3-2.4 (nested)
BUSNUM=$(cat "$BRIDGE/busnum")
PORTNUM=${DEVNAME##*[.-]}
if [ "$DEVNAME" = "${BUSNUM}-${PORTNUM}" ]; then
    PORT="/sys/bus/usb/devices/usb${BUSNUM}/${BUSNUM}-0:1.0/usb${BUSNUM}-port${PORTNUM}/disable"
else
    HUBDEV=${DEVNAME%[.-]$PORTNUM}
    PORT="/sys/bus/usb/devices/${HUBDEV}:1.0/${HUBDEV}-port${PORTNUM}/disable"
fi
[ -e "$PORT" ] || { echo "FAILED: derived port control $PORT does not exist — find your hub port's 'disable' attr manually"; exit 1; }
echo "SVP7500 at $DEVNAME, port control $PORT"

# ---- raw readiness probe ---------------------------------------------------
# Set the 16-bit register pointer (0x00fd, the page register the kernel probe
# touches first) and read one byte back, straight through /dev/i2c-N. Acks
# only once the SVP7500 fw is up; times out (-110) during warmup. Touches no
# kernel driver/devres state — safe to repeat.
i2c_ready() {
    python3 - "$1" "$SENSOR_ADDR" << 'PYEOF' 2>/dev/null
import fcntl, os, sys
I2C_SLAVE_FORCE = 0x0706
fd = os.open(sys.argv[1], os.O_RDWR)
fcntl.ioctl(fd, I2C_SLAVE_FORCE, int(sys.argv[2], 16))
os.write(fd, bytes([0x00, 0xfd]))
os.read(fd, 1)
PYEOF
}

# ---- power-cycle -----------------------------------------------------------
echo "camera-led-off: power-cycling SVP7500 (camera offline ~2-4 min)"
RELAY_WAS_ACTIVE=0
if systemctl is-active --quiet "$RELAY" 2>/dev/null; then
    RELAY_WAS_ACTIVE=1
    systemctl stop "$RELAY"
fi

echo 1 > "$PORT"
sleep 3
echo 0 > "$PORT"

# wait for USB re-enumeration to re-create the sensor i2c client
i=0
until [ -e "/sys/bus/i2c/devices/$SENSOR" ]; do
    i=$((i+1))
    [ $i -gt 10 ] && { echo "FAILED: sensor i2c device did not reappear — check dmesg"; exit 1; }
    sleep 3
done

# resolve the i2c bus number — it changes across re-enumerations
BUS=$(basename "$(dirname "$(readlink -f "/sys/bus/i2c/devices/$SENSOR")")")
DEV="/dev/${BUS}"
[ -e "$DEV" ] || { echo "FAILED: resolved $DEV does not exist — check dmesg"; exit 1; }
echo "sensor on $DEV, polling chip fw readiness (raw i2c, no bind)..."

# poll raw i2c until the chip acks; require 3 consecutive acks so we don't
# bind on a half-initialised fw. NO driver bind happens during this loop.
acks=0
i=0
while [ $acks -lt 3 ]; do
    i=$((i+1))
    [ $i -gt 40 ] && { echo "FAILED: chip never acked after ~6 min 40s — check dmesg, do NOT bind by hand"; exit 1; }
    if i2c_ready "$DEV"; then
        acks=$((acks+1))
        echo "  chip acked ($acks/3)"
    else
        acks=0
        echo "  chip fw warming up (poll $i/40), retry in 10s..."
    fi
    sleep 10
done

# ---- bind exactly ONCE — see incident note above ---------------------------
if [ ! -e "$DRV/$SENSOR" ]; then
    echo "$SENSOR" > "$DRV/bind" 2>/dev/null || true
fi
if [ -e "$DRV/$SENSOR" ]; then
    echo "ov05c10 bound — camera recovered"
else
    echo "FAILED: single bind attempt did not stick — DO NOT retry the bind."
    echo "Rerun this script (fresh port power-cycle) or reboot. Check dmesg."
    exit 1
fi

# IR sensor: best effort (non-streaming anyway, keeps boot parity)
echo i2c-HIMX1092:00 > /sys/bus/i2c/drivers/hm1092/bind 2>/dev/null || true

[ "$RELAY_WAS_ACTIVE" = 1 ] && systemctl start "$RELAY"
echo "Done. LED OFF until the next camera stream."
