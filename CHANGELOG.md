# Changelog

## 1.0.7 — 2026-09-21

- AppStream driver metainfo (`cz.vitexsoftware.ep800`) with stock icon `ep800`.
- Install hicolor icons generated from the VLC preview screenshot.

## 1.0.6 — 2026-09-21

- Snapshot button on interrupt EP 0x82 → Linux input `KEY_CAMERA` (not HID).

## 1.0.5 — 2026-09-21

- Fix blue skin / R↔B swap: demosaic output is already BGR24 (match legacy `BGRon=0`).

## 1.0.4 — 2026-09-21

- Fix `decode_bayer` kernel oops (workqueue raced with freeing the RGB buffer).
- Bounds-check demosaic writes; flush decode work via vb2 wait_prepare/finish.

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
