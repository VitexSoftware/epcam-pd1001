# Changelog

## 1.0.3 — 2026-09-21

- Fix disconnect vs vb2 release deadlock (VLC hang / Ctrl+C ignored / D-state unbind).
- Null `udev` on disconnect so I/O fails fast; proper `v4l2_device` release path.
- Avoid `cancel_work_sync` under the vb2 ioctl lock during stop.

## 1.0.2 — 2026-09-21

- Move EPLite/Bayer decode from USB URB softirq to a workqueue (host freezes).
- Debian DKMS package polish.

## 1.0.1 — 2026-09-21

- Add `VIDIOC_ENUMINPUT` / `G_INPUT` / `S_INPUT` and parm/frameinterval ioctls
  so VLC and FFmpeg can open the device.

## 1.0.0 — 2026-09-21

- Initial modern V4L2 + videobuf2 driver for Creative PD1001 / EP800.
- DMA-safe USB control buffers, DKMS packaging.
