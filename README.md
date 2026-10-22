# rastertoxrhbpl – Dell C1760nw / Xerox Phaser 6000B & 6010N on Linux (ARM and x86)

A portable **CUPS driver** for the host-based colour laser printers
**Dell C1760nw**, **Xerox Phaser 6010N** and **Xerox Phaser 6000B**. These
printers have no PostScript or PCL interpreter. The computer has to render the
page, convert the colours and compress the image into Fuji Xerox's *HBPL*
format. The original drivers only exist for Windows and macOS (x86/PPC), so the
printers do not work on a Raspberry Pi or any other ARM machine.

This project replaces the macOS filter chain of the original Xerox driver
(`XeroxHBFilter.bundle`, version 6.10) with a small portable C
re-implementation, reconstructed by reverse engineering. The output is
**byte-identical** to the original driver (apart from the date and time in the
PJL header). It runs on Linux arm64, armhf and x86_64, and on macOS.

Together with the Dell C1760nw's network port and CUPS' standard sharing you get
a **print server for iPhone, Mac, Android and Windows without any driver on the
client** (AirPrint / Mopria / IPP Everywhere), see
[AirPrint, Mopria and IPP](#airprint-mopria-and-ipp).

## Status

| What | Result |
|---|---|
| Printing on a **Dell C1760nw** (network, port 9100) from a Raspberry Pi 4, Debian 13 "trixie", aarch64, CUPS 2.4.10 | works (colour pages verified) |
| Output compared with the original Xerox filters | byte-identical, 28 regression tests |
| Regression tests | pass on macOS, Linux arm64 and Linux armhf |
| AirPrint printing from a Mac through the shared queue | works |
| Bonjour announcement (AirPrint `URF`, Mopria keys) | verified on the Pi; iPhone, Android and Windows clients not tested yet |
| Xerox Phaser 6010N / 6000B | PPDs are generated, but these printers were **not** tested on real hardware |

The Dell C1760nw accepts the Phaser 6010N job format, so this driver uses the
**6010N PPD** for it.

## Features

- Paper sizes incl. custom sizes, paper type
- Colour and black & white, image type (Standard / Photo / Presentation / Web /
  POP / Off), screen, draft mode
- Skip blank pages, rotate image by 180°, colour balance (12 controls)
- Copies and collation
- Output is the printer's native HBPL job with lossless "alla5" compression

### Not ported

- **Trapping, edge enhancement, sharpness** (`XRM_TP`, `XRM_EE`, `XRM_DF`).
  These are image filters that the Mac driver runs on the host. They were
  removed from the PPD, so the printer is told they are "off", like the
  original default.
- Brightness / contrast / saturation and manual duplex from the macOS dialog
  plug-ins. The Dell C1760nw prints single-sided only anyway.
- The lossy fallback ("Jako") for black & white pages whose compressed size
  exceeds 50 MB. No real page does this.
- One bug of the original is deliberately not reproduced: if `XRM_PR` gets no
  job options at all, it rotates every second page by 180°. CUPS always passes
  options, so this does not occur here.

## Requirements

- CUPS and a C compiler, for Raspberry Pi OS / Debian:
  `sudo apt install build-essential libcups2-dev cups cups-filters ghostscript`
- **Two files from the original Xerox macOS driver package.** They are not
  part of this repository (they belong to Xerox):
  - `Payload/Library/Printers/Xerox/DLUT/Xerox Phaser 6010.dlut` (colour tables)
  - `Payload/Library/Printers/PPDs/Contents/Resources/Xerox Phaser 6010N.gz`
    (and `Xerox Phaser 6000B.gz`); the PPDs are gzip files without a `.gz`
    suffix in the original package.

  Download the macOS driver for the Phaser 6010 from Xerox and expand the
  package (on a Mac: `pkgutil --expand-full Xerox*.pkg out`). Pass the
  resulting `Payload` folder to `make` as `XEROX_PAYLOAD`.

## Installation

```sh
make
make check XEROX_PAYLOAD=/path/to/Payload      # optional, 28 regression tests
sudo make install XEROX_PAYLOAD=/path/to/Payload
```

Installed files:

| File | Destination |
|---|---|
| filter | `$(cups-config --serverbin)/filter/rastertoxrhbpl` |
| colour table | `/usr/share/xerox-phaser6000/Xerox Phaser 6010.dlut` |
| PPDs | `/usr/share/ppd/xerox-phaser6000/Xerox-Phaser-6000B.ppd`, `…-6010N.ppd` |

Cross-compiling, e.g. for a Raspberry Pi: `make CC=aarch64-linux-gnu-gcc`
(needs `libcups2-dev:arm64`). `sudo make uninstall` removes everything again.

> **CUPS 2.4.10 and newer:** Older versions of `xrsettings.c` defined
> `_CUPS_NO_DEPRECATED` unconditionally, which turns the PPD API into
> "unavailable" with current CUPS headers and breaks the build. It is now
> limited to macOS.

## Adding the printer

Network printer (Dell C1760nw, raw port 9100):

```sh
sudo lpadmin -p DellC1760nw -E -v socket://PRINTER-IP:9100 \
     -P /usr/share/ppd/xerox-phaser6000/Xerox-Phaser-6010N.ppd \
     -D "Dell C1760nw"
sudo lpoptions -p DellC1760nw -o PageSize=A4
lp -d DellC1760nw test.pdf
```

USB printer (Phaser 6000B): `lpinfo -v | grep -i xerox` shows the URI, then use
it with `-v` and the `…6000B.ppd`.

Filter chain on Linux: `pdftopdf → gstoraster → rastertoxrhbpl`.

Test without a printer (writes the job to a file instead of sending it):

```sh
/usr/sbin/cupsfilter -e -p /usr/share/ppd/xerox-phaser6000/Xerox-Phaser-6010N.ppd \
    -m printer/foo -o PageSize=A4 test.pdf > job.prn
```

`-e` is required, otherwise `cupsfilter` ignores the PPD's filters. On Debian
`cupsfilter` lives in `/usr/sbin`, which is not in a normal user's `PATH`.

> **Speed:** The filter is quick (about 1 s per A4 page on an Apple-silicon Mac),
> but Ghostscript rasterising dominates on small boards. On a Raspberry Pi 4 a
> photo-heavy colour page takes about a minute until the printer starts.

## AirPrint, Mopria and IPP

CUPS can share the queue as a standard IPP printer and announce it via Bonjour
(mDNS), so iPhones, Macs, Android devices and Windows find it without any
driver. CUPS converts the incoming PDF or Apple raster (URF) to the printer's
format with the filter chain above.

1. Install Avahi (the mDNS daemon): `sudo apt install avahi-daemon`
2. In `/etc/cups/cupsd.conf` change/add (back up the file first):

   ```
   Port 631                 # instead of "Listen localhost:631"
   ServerAlias *
   Browsing Yes             # was "Browsing No"
   IdleExitTimeout 0        # keep cupsd running so the announcement stays

   <Location />
     Order allow,deny
     Allow @LOCAL           # only the local network may print
   </Location>
   ```

   Leave the `/admin` locations as they are, so administration stays local and
   protected.
3. Share the queue and restart CUPS:

   ```sh
   sudo lpadmin -p DellC1760nw -o printer-is-shared=true -o printer-error-policy=retry-job
   sudo systemctl restart cups
   ```

Check the announcement on the Pi (`sudo apt install avahi-utils`):

```sh
avahi-browse -rtp _ipp._tcp                       # should show URF=… and pdl=application/pdf,image/urf,…
avahi-browse -tp _universal._sub._ipp._tcp        # AirPrint sub-type
```

Using it:

- **iPhone / iPad / Mac:** pick the printer in the print dialog. On a Mac, add it
  under *Printers & Scanners* with the "AirPrint" driver.
- **Android:** the built-in default print service (Mopria / IPP Everywhere)
  should find it (the queue announces `mopria-certified=1.3`). If not, add it manually by IP with
  `ipp://PI-IP:631/printers/DellC1760nw`.
- **Windows 10/11** (untested): *Add printer → The printer I want isn't listed → select a
  shared printer by name*, URL `http://PI-IP:631/printers/DellC1760nw`.

> **Security:** Printing is allowed for the whole local network without
> authentication, as with any AirPrint printer. mDNS does not cross VLANs or
> subnets, so phones and the Pi need to be in the same network (or use an mDNS
> repeater).

## Checking the printer

The Dell C1760nw has a web interface (`http://PRINTER-IP/index.htm`) and answers
SNMP, which helps to tell "the Pi did not send anything" from "the printer did
not print":

```sh
snmpget -v1 -c public -Oqv PRINTER-IP 1.3.6.1.2.1.43.10.2.1.4.1.1   # total page counter
snmpget -v1 -c public -Oqv PRINTER-IP 1.3.6.1.2.1.25.3.5.1.1.1      # 3 idle, 4 printing
snmpwalk -v1 -c public -Oqv PRINTER-IP 1.3.6.1.2.1.43.18.1.1.8      # alerts, e.g. toner
```

If the page counter goes up while a job runs, the printer printed.

## Layout

| File | Counterpart in the original | Purpose |
|---|---|---|
| `src/rastertoxrhbpl.c` | `XRM_MF`, `XRM_SP`, `XRM_FXR`, `XRM_PR` | read CUPS raster, spool pages, copies |
| `src/xrsettings.c` | `GetPrintSetting` etc. | evaluate PPD and options |
| `src/xrcolor.c` | `XRM_CC` | RGB→CMYK: sRGB conversion + 17³ LUT, tetrahedral interpolation |
| `src/xrpage.c` | `XRM_SBP`, `XRM_TRC` | page pipeline, blank pages, draft mode |
| `src/alla5.c` | `XRM_AL5CB` | lossless "alla5" compressor (Fuji Xerox) |
| `src/hbpl.c` | `XRM_HBPL` | PJL header, HBPL records, job info |
| `tools/make-ppd.sh` | – | derives the Linux PPD from the original Xerox PPD |
| `test/` | – | regression tests against reference hashes of the original filters |

The tests compare each stage and complete jobs with SHA-256 hashes produced by
the original filters. `make check` needs `XEROX_PAYLOAD`; without it, the tests
that need the original files are skipped.

## Format notes (result of the reverse engineering)

**HBPL job**: `ESC%-12345X` + PJL header … `@PJL ENTER LANGUAGE=HBPL`, then
binary records: `A` (job), per page `C` (page attributes) `Q` `R` (image data)
`S` `D`, then `B` (end), followed by PJL `FXJOBINFO` comments and `@PJL EOJ`.
Attributes are `id, 0xA0|type, value` (number) or `id, 0xC0|type, x, y`,
little endian; type 0/1 = 1 byte, 2/3 = 2 bytes, 4–6 = 4 bytes. Copies:
collate off → `COPIES=n` and one output; collate on → `QTY=1` and the host
repeats the pages n times.

**alla5** (compression type 7 = alla5 with LPH marker):

- Every line gets 8 LPH pixels in front (`FFFFFFFF` + 7× `0`) and is padded to a
  multiple of 8 pixels. Colour is 32-bit pixels (C,M,Y,K), black & white is 8 bit (K).
- Model: 5 predictors in a move-to-front list (repeat left, above, above-left,
  above-right, above-right+2; for B/W: 8 pixels left instead of above-right+2).
  A symbol is a run (predictor *k*, length ≤ 17057) or an error (byte-wise
  difference to the left neighbour). Runs at the end of a line stay open and
  continue in the next line; after a run shorter than the maximum the predictor
  numbering shifts by −1.
- Static prefix codes from group tables (`cw_order_*`, `cw_run_graphics`,
  `cw_error_graphics`, `cw_marker_ver1`); the order table switches adaptively
  between "image" and "graphics" per line (counter ±9, marker 0). The end
  marker is 2.
- 5 bit streams (MSB first, padded with 1 bits): stream 0 = order/run/marker,
  streams 1–4 = error codes per colour component. Container: 16-byte length
  table (streams 0, 4, 1, 2) followed by the streams in order 0, 4, 1, 2, 3; an
  empty stream is sent as a single `A`.

**Colour**: non-white pixels → gamma 2.2 (16 bit) → 3×3 matrix
(XYZ→sRGB · sRGB→XYZ, 16.16 fixed point) → sRGB curve (4096-entry table) →
tetrahedral interpolation in the sub-LUT of the `A_DLUT_PRINT` section of the
`.dlut` file (Normal = 1, Photo = 4, Presentation = 5, Web = 7, POP = 9).
Image type "Off": simple under-colour removal. Black & white:
`K = 255 − (0.30 R + 0.59 G + 0.11 B)` in 16-bit fixed point.

Build note: `-ffp-contract=off` is required (otherwise aarch64 fuses multiply-adds
and the results differ), and the colour matrix is computed through volatile
products. A checksum self-test of the tables runs at start-up.

## Legal notes

This is an independent, unofficial project and is not affiliated with Dell,
Xerox or Fuji Xerox. "Dell", "Xerox" and "Phaser" are trademarks of their
respective owners. The file format and algorithms were reconstructed by
analysing the original driver binaries for interoperability. This repository
contains no Xerox or Dell software: the colour table and PPDs have to come from
your own copy of the original driver package.
