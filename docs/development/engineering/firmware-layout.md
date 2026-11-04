<p align="right">
  <a href="firmware-layout.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Firmware Layout

This repository targets an ESP32-C3 with 8 MB Flash. The current application
uses two OTA app slots so the device can update over the LAN.

## Default layout

The default partition table contains:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x6000` | ESP-IDF and application key-value storage |
| `otadata` | data/OTA | `0xF000` | `0x2000` | OTA boot-selection data |
| `phy_init` | data/PHY | `0x11000` | `0x1000` | PHY initialization data |
| `ota_0` | app/ota_0 | `0x20000` | `0x3F0000` | First application slot |
| `ota_1` | app/ota_1 | `0x410000` | `0x3F0000` | Second application slot |

The first image with this layout must be written over USB at `0x0`. Later
updates can use the device OTA page, which writes the inactive slot and keeps
NVS settings.

## Custom layouts

Users may edit `partitions.csv` to resize, move, add, or remove partitions for
their application. A custom table may use OTA slots, filesystem/resource
partitions, or other application-specific data. Keep the 8 MB device boundary,
avoid overlaps, and make sure the application image is flashed at the start of
an app partition large enough to contain it. When a derivative changes its
layout, update that project's documentation and flashing instructions.

## Enforced validation

Run:

```bash
./tools/validate.sh --firmware
```

The check builds in an isolated directory, creates the merged image, reads the
configured image offsets from `flash_args`, validates the partition-table MD5,
partition bounds, unique labels, and non-overlap, then ensures the application
offset matches an app partition large enough to contain it. It intentionally
does not require the default partition list. CI runs the same gate.

Every image listed in `flash_args`, including user-defined resources and OTA
data, must exist, be nonempty and match the merged bytes at its configured
offset. Image ranges must stay within 8 MB and must not overlap. Additional
images must fit entirely inside a configured partition; an offset inside that
partition is allowed. Merely declaring a resource partition does not require a
preloaded image, but listing an image in `flash_args` makes it mandatory.

For USB flashing, write only `build/FoloToy-AI-Passport-full.bin`; the
similarly named app-only `build/FoloToy-AI-Passport.bin` does not contain the
bootloader or partition table. For OTA, upload the app-only
`build/FoloToy-AI-Passport.bin` through the device page; never upload the merged
file there.

OTA updates preserve NVS. A merged image written from `0x0` fills the gaps
between images and can reset NVS and PHY data, so use OTA for ordinary updates
and reserve the merged image for initial provisioning or intentional full
refreshes.

## Flashing and stored data

> **No backup of the firmware already installed on the device is required
> before downloading (flashing) new firmware.** Do not make reading out the
> original firmware or saving a full-Flash dump a prerequisite for this
> workflow. The new firmware replaces the original firmware; this workflow
> does not retain an automatic rollback copy or promise that the original
> firmware can be restored.

Firmware and user data are different. If existing NVS settings, application
records, or files must be kept, export or otherwise save them before flashing
using a method supported by that application. Not requiring an original-firmware
backup does not guarantee data preservation or authorize a full-chip erase.

The verified merged image is written from `0x0`. Because the merged file pads
the gaps between images, flashing it can reset the NVS and PHY data regions.
Use the merged image for blank-device provisioning or an intentional complete
refresh. During normal development, use segmented `idf.py flash` when existing
NVS state should be preserved; this also requires a compatible partition layout
and flash targets that do not overwrite those data regions. `idf.py erase-flash`
erases all user data. Do not add it as a routine prerequisite: use it only when
a complete erase is explicitly intended and any data that must be kept has
been saved.
