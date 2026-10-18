# Smart342 for Windows - user guide

Smart342 for Windows lets you use a **Scitex Smart 342 / 342L** film scanner with a normal Windows PC. It runs Scitex's own scanner software inside the program, so the scanner works exactly as it did on its original Macintosh - and it adds RAW 16-bit files, HDR, negative conversion and more.

**Version 0.5.0 - a public test version.** So far it has been used with one scanner. If you try it, please tell us how it went (see "If something goes wrong").

Not affiliated with Scitex, Creo or Kodak.


## What it adds to the scanner

Everything the original software did still works the same way. On top of that:

- **RAW 16-bit TIFF** - the scanner's own data with its full precision (up to 14 bits in the shadows), before any colour table, curve or sharpening. The most editing latitude the scanner can give.
- **HDR** - a second, longer exposure merged into the shadows: about 2 to 3 times less noise in the darkest parts, lined up precisely with the first pass.
- **Pixel shift** - the lens is moved by a fraction of a pixel between passes and the passes are combined: twice the resolution across the scan (up to 5324 dpi with the 35 mm lens), with real extra detail.
- **Step-and-hold** - the scanner's carriage stands still while each line is exposed, so the picture is sharper along the scan.
- **Averaging** - several passes of the same frame averaged for less noise.
- **RAW sharpening** - an extra copy sharpened to undo the softness of the scanner's lens (measured on your own scanner if you run Set up with the Scitex focus chart).
- **Colour registration** - red and blue lined up exactly with green in every RAW.
- **Negatives** - colour negatives turned into a ready-to-edit positive TIFF automatically (orange mask removed, neutral colours), with a long-exposure option for dense film.
- **Fix streaks** - measures your own scanner and removes the fine streaks along the scan (from about 1 % to under 0.1 %).
- **Safe by design** - the program never changes anything stored in the scanner.


## What you need

- **A Windows PC** - Windows 10 or 11, 64-bit, with an Intel or AMD processor.
- **A SCSI card** in the PC, with its Windows driver installed. Most SCSI cards that have a Windows 10 / 11 driver should work.
- **A SCSI cable** from the card to the scanner (and the scanner's terminator, as always).

That is all - the Scitex software comes with the program. More about SCSI cards: see "SCSI cards in detail" below.


## Getting started

1. Unpack the zip file anywhere, for example to your desktop.
2. Switch the scanner on and wait for its steady green light.
3. Start **smart342.exe**. Windows asks for permission (it needs to reach the SCSI card) - answer **Yes**. If Windows says the program is from an unknown publisher: **More info > Run anyway**.
4. The program finds the scanner by itself.
5. The first time, it offers to **copy your scanner's settings** from the scanner into the program. Choose that. The program restarts by itself.
6. Wait until the bottom of the window says **Ready**.

The first time, the program also offers **Set up this scanner** (a few minutes per lens). You can do it now or later from the Tools menu.


## Scanning

1. Choose **Original** (Transparent or Reflective), **Cassette** (the film holder you put in), **Mode** (Colour or Greyscale) and **Media** (Positive or Negative). Press **Preview**.
2. Drag a rectangle on the preview around the part you want.
3. Set the **resolution** (the allowed range is shown under it), the **file format** and the file name.
4. Press **Scan**. The file goes to the **Scans** folder ("Open Scans folder" opens it).

**Stop** ends the running job. Close the window when you are done; the scanner parks itself.

If the Scitex software shows a message, it appears as a small window - answer it as you would on the Mac.


## If something goes wrong

- **The scanner is not found:** check that it is switched on with a steady green light, that the cable is in, and that the SCSI card appears in Windows' Device Manager without a problem sign. Then start the program again. The program lists the SCSI devices it can see.
- **"The scanner is not answering":** switch the scanner off, wait 10 seconds, switch it on, wait for the green light, start the program again.
- **Large scans fail but previews work:** your SCSI card's driver limits how much data one command may carry. The program tells you at the start if this applies - see "SCSI card limits" below.
- **Anything else:** every start makes a folder in **Logs** named with the date and time. Zip that folder and attach it to an issue on GitHub (or send it to the maintainer) with a few words about what you did and what happened.


## Safety and warranty

- The program **never changes what the scanner stores** (its settings, its scanner number, its firmware). A safety lock inside the program blocks every such command; it cannot be switched off. It only reads the scanner's settings, and only sends what scanning needs - the same as the original Mac software did.
- It has no calibration or service functions.
- The program is provided **as is, without warranty of any kind**, and you use it at your own risk.


---

# More details

## SCSI cards in detail

The program talks to the scanner through Windows' own standard SCSI interface ("SCSI pass-through"). That interface is the same for every SCSI card, so the program does not depend on a particular card. A card works if:

- **Windows 10 / 11 (64-bit) has a driver for it, and the driver loads.** Many cards from the 1990s never got one. In Device Manager the card must appear under "Storage controllers" without a warning sign.
- **Its driver allows SCSI pass-through.** Normal SCSI card drivers do. Many USB and FireWire SCSI adapters do not.
- **One command may carry at least 300 KB** - see "SCSI card limits".
- **The connector fits.** The scanner has a narrow (50-pin) single-ended SCSI connection. Cards with a narrow connector fit with a normal cable; wide or LVD cards that also work single-ended fit with the right cable or adapter. Differential-only (HVD) cards do not work with the scanner.

In Device Manager the scanner itself shows up as "SCITEX SMART340 SCSI Scanner Device", often 8 times and with a warning sign. That is normal: Windows has no driver for the scanner and does not need one.

If the card's driver does not load, check Windows Security > Device security > Core isolation > **Memory integrity**: some older drivers are not compatible with it, and Windows does not load them while it is on. Turning it off lowers one of Windows' protections against harmful drivers - decide for yourself.

### SCSI card limits

The Scitex software reads the scanner's data in pieces of up to **300 KB (307,200 bytes)** per command. Every SCSI card driver tells Windows how much one command may carry; the program asks for it at every start and writes it to the log ("SCSI card limits: ..."). If it is less than 300 KB, the program says so in a message: previews and small scans work, larger scans would fail with Windows error 87.

Many SCSI card drivers (in particular those built on Windows' classic SCSI port driver, which includes most older cards) have a setting for this, the registry value **MaximumSGList** (a DWORD) in
`HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\<the card's driver>\Parameters\Device`.
It is the number of memory pages (4 KB each) one command may use; 300 KB needs at least **76**, and **255** (the highest allowed value) is a safe choice. Create the keys if they are missing, then restart Windows. The driver's name is shown in Device Manager > the card > Properties > Driver > Driver Details (for example aic78xx). A driver without this setting may have its own; check its documentation.

## Tested hardware

These have been used successfully:

| Part | Tested |
|---|---|
| Scanner | Scitex Smart 342L (reports as "SCITEX SMART340", firmware 1.C8), motion board type 1 ("Master 17 / Main 11.11") |
| SCSI card | Adaptec AHA-2906 (AIC-7850), Windows driver aic78xx, MaximumSGList = 255 (the 65 that was set before was too low for large scans) |
| Connection | the card in a PCIe-to-PCI adapter (ASMedia ASM1083) in a desktop PC - the adapter's own power connector must be plugged in |
| PC | Windows 11, AMD Ryzen, desktop |

Known not to work, or with problems:

- A SCSI card in a **Thunderbolt** enclosure crashed laptops (a problem of that adapter chip with the laptops' memory protection).
- A **RATOC FR1SX** FireWire-to-SCSI adapter: Windows refused the pass-through.
- The aic78xx driver above is not compatible with **Memory integrity** (it must be off for that card). After every (re)install of that driver, check that `HKLM\SYSTEM\CurrentControlSet\Services\aic78xx\Start` is `3`: a reinstall can set it to 0, and a failing card can then stop Windows from starting.
- Scanners with motion board type 2 have never been tried.
- Windows on ARM (e.g. Snapdragon laptops): no SCSI card drivers exist for it.

Please report any other card, PC or scanner - whether it works or not - so this list can grow.

## Starting for the first time, in detail

- The program looks for the scanner on every SCSI card and ID: first in Windows' own list of devices (nothing is sent to them), then by asking each ID. With several Scitex scanners it asks which one; with none it lists what it found and lets you type the ID (it is on the scanner's front panel). The place is saved; Tools > SCSI connection searches again.
- The Scitex settings file in the package belongs to another scanner, so at the very first start the Scitex software stops at its scanner-number check. The program then offers to copy your scanner's own parameters from the scanner's flash disk into the settings file - a read-only copy, the same as "Scanner (Flash Disk) --> Mac" in the original Calibration program - and starts again. The previous settings file is kept.
- Optional but recommended: **Tools > Fix streaks** (about 10 minutes, see below).

## More about scanning

- Holders with several frames are previewed frame by frame; pick the frame above the preview.
- "Max" sets the highest resolution; you can also tick "Crop (mm)" and type the crop.
- Several crops: "Add crop to list" for each, then "Scan list".
- Stop waits while the scanner is sending a picture until the picture is in (stopping in the middle of a picture would leave the scanner needing to be switched off and on).

### Film holders and lenses

Every holder the original software offers is in the Cassette list, with the same lens. The finest real detail across the scan (the CCD's own sampling) depends on the lens:

| Lens | CCD sampling | Holders |
|---|---|---|
| 35 mm | 2662 dpi | T 35mm, 4 T35mm H/V, Strip 35mm, Framed 8x35mm |
| 6x6 | 1349 dpi | T 6 x 6, T 6 x 7, 3 T6x7 H, Strip 6 x 7 |
| 4x5 | 781 dpi | T 4 x 5, Strip 4 x 5, 150mm |
| 8x10 | 406 dpi | T 8 x 10 H/V, All Board, A4 H/V, All Board-A3 |

Finer resolutions are interpolated across the scan; along the scan the carriage steps at the resolution you choose.

## RAW 16-bit TIFF

File format **RAW 16-bit TIFF** saves the scanner's own data of the crop before any gradation, colour table or sharpening: linear values, 16 bits per channel. Use it for the most editing latitude.

- **RAW passes:** 2 to 16 scans of the same crop, aligned and averaged (less noise). "Keep each pass" saves every pass too.
- **RAW exposure HDR +1 / +2 stops (experimental):** a second pass with a 2x or 4x longer exposure, merged into the shadows. Both passes and the merged file are saved.
- **RAW exposure Long x2 / x4 (experimental), for negatives and other dense film:** one pass with a 2x or 4x longer exposure, saved as it comes (2x or 4x brighter than a Normal RAW, as its description says). A colour negative lets through only about an eighth of the light of clear glass, so at Normal it uses a small part of the scanner's range; Long x4 gives it about four times as many brightness levels and less noise. The positive (POS16) is matched to it automatically. If the log says values are clipped, use Long x2.
- **RAW gamma:** Linear, or Gamma 2.2 (easier to view; all processing is done on the linear values first).
- **Greyscale + RAW:** one 16-bit channel from the green CCD row, the sharpest of the three.
- **Negative + RAW:** a positive TIFF is saved next to the RAW ("... POS16.tif"); see "Negatives".
- Large crops are streamed from the scanner in one request.

**For the most detail from a frame:** RAW 16-bit TIFF, Tools > Pixel shift RAW on, Tools > RAW step-and-hold on (the default), then Scan. Without pixel shift, type the CCD's own resolution (2662 with the 35 mm lens, 1349 with the 6x6 lens); the program sends it a hair below, so every column is the CCD's own sample.

## Tools menu

- **Pixel shift RAW:** 2x finer across the scan for a crop. Several passes with the lens a few motor steps apart, combined. Not saved (off at the next start).
- **RAW step-and-hold:** at the lens's CCD resolution or finer, the carriage stands still while each line is exposed. Sharper along the scan, about 1.4x the time. On by default.
- **RAW colour registration:** red and blue are measured against green in every RAW and moved onto it. On by default.
- **RAW sharpening:** an extra "... SHARP" file that restores the lens's measured loss. The RAW itself is untouched.
- **Negatives:** the positive TIFF on or off, contrast, colour levels per frame or kept for a whole roll, rotation of the positive.
- **Set up this scanner:** see below.
- **Fix streaks:** see below.
- **SCSI connection:** shows where the scanner is; can search every ID again.
- **Detailed log in the window:** every line of the session log in the window.
- **Diagnostics:** tests and research settings. Leave them alone unless asked.

## Set up this scanner

For each lens you have a holder for: choose the holder, put a piece of film with fine detail in it, and the program measures how far one lens step moves the picture (10 small RAWs, about 7 minutes per lens). Pixel shift uses the result. If you have the Scitex focus chart, the lens's sharpness is measured too, for RAW sharpening. Results are saved in `smart342.ini` under your scanner's number. Without the setup, pixel shift uses general values.

## Fix streaks

Fine streaks along the scan, strongest in bright areas, come from a stored "clean gain vector" that does not match your scanner (the settings file may come from another scanner, or dust on the KEEP CLEAN strip got into it).

1. Clean the KEEP CLEAN strip and the glass.
2. Put a holder in with the selected frame **empty** (a frame whose glass is clean and not scratched).
3. Tools > Fix streaks. Check that the holder named in the window is the one in the scanner, then Start. The program checks that the frame is empty, makes 5 small RAWs (about 4 minutes, not saved) and shows how much streaking is left with the vector in use and with the new one (measured on green; the vector corrects all three colours).
4. "Store it and start again". Only the settings on this PC change; nothing is written to the scanner.

## Negatives

With Media "Negative" the preview is shown as a positive, and every RAW also gets a 16-bit positive TIFF ("POS16"). The orange mask is removed and blacks and whites come out neutral, using the levels of the whole previewed frame, so a crop keeps the frame's colours. Tools > Negatives sets contrast, keeps one frame's levels for a whole roll, and rotates the positive. The RAW is never changed.


## Log files

Every start makes a folder in `Logs\` named with the date and time: `ui-log.txt` (every message), `trace.txt` (what the Scitex software did) and `scsi-trace.bin` (every SCSI command). The window shows the main messages; Tools > Detailed log in the window shows all of them.


## Without a scanner

`smart342.exe --simulate` runs everything against a simulated scanner (a test pattern instead of a picture), in its own copy of the work folder.


## Licence

Smart342 for Windows itself (the program and its source code) is released under the MIT licence; see LICENSE. The Scitex software in the `Volume` folder is not covered by that licence and remains the property of its owners; see NOTICE.
