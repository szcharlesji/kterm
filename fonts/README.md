# kterm fonts

Anything dropped in this directory becomes available to kterm, because the
launcher points `FONTCONFIG_FILE` at `fonts.conf` here. The Kindle rootfs is
read only and its own fontconfig only scans `/usr/share/fonts` and `~/.fonts`,
so this is the supported place to add fonts.

After adding a font, rebuild the cache on the device:

```sh
FONTCONFIG_FILE=/mnt/us/extensions/kterm/fonts/fonts.conf fc-cache -f
FONTCONFIG_FILE=/mnt/us/extensions/kterm/fonts/fonts.conf fc-list | grep -i nerd
```

Then set the family in `kterm.conf`, e.g.

```
font_family = "JetBrainsMono Nerd Font Mono"
```

## Getting a Nerd Font

No font binary is checked into this repository. Two options:

**1. Symbols only (no download needed if KOReader is installed).**
`fonts.conf` already adds `/mnt/us/koreader/fonts`, which contains
`nerdfonts/symbols.ttf`. Keep `font_family` as a normal monospace face
(`DejaVu Sans Mono`) and fontconfig will pull icon glyphs from the symbol
font. Smallest footprint, and text metrics stay exactly as they are today.

To install the symbol font without KOReader, fetch it from the Nerd Fonts
release and copy it here:

```sh
curl -fL -o NerdFontsSymbolsOnly.zip \
  https://github.com/ryanoasis/nerd-fonts/releases/latest/download/NerdFontsSymbolsOnly.zip
unzip -j NerdFontsSymbolsOnly.zip 'SymbolsNerdFontMono-Regular.ttf' -d .
```

**2. A fully patched monospace font.** Better if you want consistent
metrics for icons and text in one face. `Mono` variants keep every glyph
single width, which is what a terminal needs:

```sh
curl -fL -o JetBrainsMono.zip \
  https://github.com/ryanoasis/nerd-fonts/releases/latest/download/JetBrainsMono.zip
unzip -j JetBrainsMono.zip 'JetBrainsMonoNerdFontMono-Regular.ttf' \
                           'JetBrainsMonoNerdFontMono-Bold.ttf' -d .
```

Then `font_family = "JetBrainsMono Nerd Font Mono"`.

## What will not work

VTE 0.28 renders through pango 1.26, which predates HarfBuzz shaping
integration, so programming ligatures (`!=`, `=>`) are not composed. The
individual characters still render correctly.
