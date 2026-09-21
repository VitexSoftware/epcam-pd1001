# Testing ep800 in a VM

Developing USB video drivers on the host can hard-lock the machine (no oops in
the journal). Validate changes in a KVM guest with USB passthrough.

## Existing libvirt guest

Example using domain `debian13`:

```bash
virsh -c qemu:///system start debian13
# wait for guest IP, then:
scp ep800-dkms_*.deb vitex@GUEST:/tmp/
ssh vitex@GUEST 'sudo dpkg -i /tmp/ep800-dkms_*.deb'

# attach webcam (plug it in first)
cat >/tmp/ep800-usb.xml <<'EOF'
<hostdev mode='subsystem' type='usb' managed='yes'>
  <source>
    <vendor id='0x041e'/>
    <product id='0x400d'/>
  </source>
</hostdev>
EOF
virsh -c qemu:///system attach-device debian13 /tmp/ep800-usb.xml --live

ssh vitex@GUEST 'sudo modprobe ep800; v4l2-ctl --list-devices'
```

Detach and shut down:

```bash
virsh -c qemu:///system detach-device debian13 /tmp/ep800-usb.xml --live
virsh -c qemu:///system shutdown debian13
```

## QEMU helper script

See [`scripts/run-qemu-usb.sh`](../scripts/run-qemu-usb.sh) for a disposable
cloud-image style launch with optional USB host passthrough.

## Guest smoke test

```bash
sudo modprobe ep800
v4l2-ctl --list-devices
timeout 10 v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5
cvlc v4l2:///dev/video0 :v4l2-width=176 :v4l2-height=144 :v4l2-chroma=BGR3
```
