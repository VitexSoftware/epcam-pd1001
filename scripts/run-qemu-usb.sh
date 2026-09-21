#!/bin/bash
# Boot a Debian cloud VM with optional USB passthrough of the PD1001.
# Host stays safe if the guest hangs.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${DIR}/.." && pwd)"
IMG="${IMG:-$DIR/debian-13-generic-amd64.qcow2}"
SEED="${SEED:-$DIR/seed.img}"
MEM="${MEM:-2048}"
CPUS="${CPUS:-2}"
VID="${USB_VID:-041e}"
PID="${USB_PID:-400d}"

if [[ ! -f "$IMG" ]]; then
  echo "Missing disk image: $IMG"
  echo "Download once, e.g.:"
  echo "  wget -O $IMG \\"
  echo "    https://cloud.debian.org/images/cloud/trixie/latest/debian-13-generic-amd64.qcow2"
  exit 1
fi

USB_ARGS=()
if lsusb -d "${VID}:${PID}" >/dev/null 2>&1; then
  BUS=$(lsusb -d "${VID}:${PID}" | sed -n 's/Bus \([0-9]*\) Device \([0-9]*\).*/\1/p')
  DEV=$(lsusb -d "${VID}:${PID}" | sed -n 's/Bus \([0-9]*\) Device \([0-9]*\).*/\2/p')
  BUS=$((10#$BUS))
  DEV=$((10#$DEV))
  echo "Passthrough USB ${VID}:${PID} hostbus=${BUS},hostaddr=${DEV}"
  USB_ARGS=(-device qemu-xhci,id=xhci
            -device usb-host,hostbus="${BUS}",hostaddr="${DEV}")
else
  echo "Webcam ${VID}:${PID} not present — starting VM without USB passthrough."
fi

SEED_ARGS=()
if [[ -f "$SEED" ]]; then
  SEED_ARGS=(-drive "file=${SEED},if=virtio,format=raw")
fi

exec qemu-system-x86_64 \
  -enable-kvm \
  -m "$MEM" -smp "$CPUS" \
  -drive "file=${IMG},if=virtio,format=qcow2" \
  "${SEED_ARGS[@]}" \
  -virtfs "local,path=${ROOT},mount_tag=hostshare,security_model=none,id=hostshare" \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -nographic \
  "${USB_ARGS[@]}"
