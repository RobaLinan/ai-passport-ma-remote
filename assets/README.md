<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, sound effects, and battery profiles, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### Noto Sans CJK SC subsets (MA remote UI)

| File | Use |
| --- | --- |
| [`fonts/NotoSansCJKsc-Regular.otf`](fonts/NotoSansCJKsc-Regular.otf) | Source font, Noto Sans CJK SC Regular, SIL Open Font License 1.1 ([`fonts/OFL.txt`](fonts/OFL.txt)); downloaded from [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk). |
| [`fonts/chars.txt`](fonts/chars.txt) | 7542 characters: printable ASCII, the GB2312 repertoire, UI punctuation (including U+00B7), and the UI strings used by this application; regenerate with `tools/gen_ma_font_chars.py`. |
| `fonts/app_font_20.c` | LVGL 20 px, 4 bpp title font (`app_font_20`). |
| `fonts/app_font_16.c` | LVGL 16 px, 4 bpp body font (`app_font_16`). |

Generate with `lv_font_conv` 1.5.3; VGL data is uncompressed and unkerned:

```bash
npx --yes lv_font_conv@1.5.3 \
  --font assets/fonts/NotoSansCJKsc-Regular.otf \
  --symbols "$(cat assets/fonts/chars.txt)" \
  --size 20 --bpp 4 --format lvgl --no-compress --no-kerning \
  --lv-font-name app_font_20 --lv-include lvgl.h \
  --output assets/fonts/app_font_20.c
```

Repeat with `--size 16 --lv-font-name app_font_16 --output assets/fonts/app_font_16.c`.
The generated sources are compiled into `main` by `main/CMakeLists.txt` and declared in
`main/app_ui.c`; the binary font data stays in Flash. Glyphs outside the GB2312
repertoire render as missing-glyph boxes, so extend `chars.txt` and regenerate when the
UI starts displaying a wider character set.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Battery profiles

[`battery/wanli-500mah-profile2-xil40x-20261020.txt`](battery/wanli-500mah-profile2-xil40x-20261020.txt)
contains the supplied CW2017 profile for the Wanli New Energy 500 mAh cell
(profile2, XIL40X, 2026-10-20). It is an unchanged copy of the project-supplied
file, retained at the maintainer's request for this firmware update. Its
original filename is recorded in the paired Chinese document. No separate
license was included in the supplied file.

The format is 80 lines of two hexadecimal digits, one byte per line in register
order. The BSP embeds those bytes in `components/bsp/src/bsp_battery.c`; the
host gate checks the source file and initializer for an exact match. Text-file
SHA-256: `7ea96eba4634f4495a37c25ffacd54c1deaa0693b162c892d671a2ba68460919`.
Decoded 80-byte SHA-256:
`fa7365671093ef449c5bdcfb4adb279e3d275f888109f50b6b2f21e9b96220d5`.
This is a fuel-gauge profile, not a charging configuration or a generic profile
for every 500 mAh cell; see the [hardware guide](../docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md#9-cw2017-fuel-gauge).
