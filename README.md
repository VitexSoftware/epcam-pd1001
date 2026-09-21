# ep800 — Creative PD1001 / Endpoints EP800 webcam driver

Out-of-tree Linux **V4L2** driver for Endpoints **EP800** USB cameras, including:

| Device | USB ID |
|--------|--------|
| Creative Webcam PD1001 | `041e:400d` |
| Endpoints EP800 reference | `03e8:1005` |

Protocol and EPLite/Bayer decoding are based on the historical
[epcam](https://sourceforge.net/projects/epcam/) driver (GPLv2+), rewritten for
modern kernels with **videobuf2**, DMA-safe USB control transfers, and a
workqueue-based decode path.

**Status:** works for capture (~2–3 fps on Full-Speed USB). Colour and exposure
are rough. Prefer testing in a VM first if you are developing the module
([docs/vm-testing.md](docs/vm-testing.md)).

## Requirements

- Linux 6.x headers for the running kernel (`linux-headers-$(uname -r)`)
- `build-essential`, `dkms` (for the Debian package)

## Quick start (DKMS)

```bash
# from a release .deb, or build one:
dpkg-buildpackage -b -us -uc
sudo dpkg -i ../ep800-dkms_*.deb
sudo modprobe ep800
v4l2-ctl --list-devices
```

## Manual build

```bash
make
sudo insmod ./ep800.ko
# sudo insmod ./ep800.ko debug=1
# sudo insmod ./ep800.ko force_bayer=1
sudo rmmod ep800
```

## Using the camera

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all

# VLC
cvlc v4l2:///dev/video0
cvlc v4l2:///dev/video0 :v4l2-width=176 :v4l2-height=144 :v4l2-chroma=BGR3

# FFmpeg / ffplay
ffplay -f v4l2 -input_format bgr24 -video_size 176x144 /dev/video0
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5
```

Replace `/dev/video0` with the node shown for **Creative PD1001**.

## Formats and controls

- Pixel format: `BGR24` (`BGR3`)
- Sizes: 176×144, 320×240, 352×288, 400×300 (within sensor max)
- Controls: brightness, contrast, saturation, hue

## Package layout

```
ep800.c / ep800.h   driver sources
Makefile            out-of-tree kbuild
dkms.conf           DKMS recipe
debian/             Debian packaging (ep800-dkms)
docs/               extra documentation
scripts/            helper scripts
legacy/epcam-0.9/   historical epcam 0.9 sources (reference only)
```

## License

GPL-2.0-or-later. See [LICENSE](LICENSE) and [debian/copyright](debian/copyright).

## Credits

- Original epcam: Jeroen B. Vreeken and later contributors (SourceForge epcam)
- Modern V4L2 port: Vítězslav Dvořák
