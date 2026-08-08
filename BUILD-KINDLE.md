# Cross-building kterm for a modern Kindle

Notes from building kterm 2.7 for a Paperwhite 5 on firmware 5.18.4.0.1.
Almost none of this is discoverable from the error messages, so it is written
down here.

## Target

Check the firmware first and never infer the ABI from the model:

```sh
ssh root@kindle 'cat /etc/prettyversion.txt; od -An -tx1 -j36 -N4 /bin/busybox'
```

The four bytes are the ELF `e_flags`. `02 04 00 05` little-endian is
`0x05000402`: EABI version 5, **hard float** (`0x400`). Firmware >= 5.16.3 is
hard float, so the toolchain target is `kindlehf`. A soft-float binary will not
load. Every binary you produce must match on that `0x400` bit.

PW5 / 5.18.4 specifics: armv7l, glibc **2.20**, GTK+ **2.20.1**, glib
**2.29.18**, pango 1.26.2, VTE is not present (kterm links it statically),
ncurses **5.6** (library only, no headers), DPI 298.

## Toolchain and SDK

Linux only; macOS is not a supported host.

```sh
sudo apt install -y build-essential git wget curl unzip texinfo libtool \
    libtool-bin cmake meson ninja-build autopoint zip gperf bison flex \
    help2man gawk zlib1g-dev libexpat1-dev libncurses-dev pkg-config rsync \
    libarchive-dev nettle-dev libgmp-dev libssl-dev

git clone --recursive --depth=1 https://github.com/koreader/koxtoolchain
cd koxtoolchain && ./gen-tc.sh kindlehf          # ~31 min on 16 threads
cd kindle-sdk && ./gen-sdk.sh kindlehf           # needs sudo, mounts firmware
```

`libtool-bin` matters: crosstool-ng wants `/usr/bin/libtool`, and the `libtool`
package only ships `libtoolize`. `libarchive-dev` and `nettle-dev` matter
because `gen-sdk.sh` builds KindleTool, which warns about them and then fails
on `archive.h`.

crosstool-ng strips the write bit from the whole toolchain tree when it
finishes. To add anything to the sysroot: `chmod -R u+w ~/x-tools/<target>`.

## Gaps in the SDK sysroot

`gen-sdk.sh` extracts the firmware's libraries but the metadata is templated
from a modern distribution, so **the `Version:` fields lie**. `glib-2.0.pc`
claims 2.82.4 and `gtk+-2.0.pc` claims 2.24.33, while the headers and the
shipped `.so` are 2.29.18 and 2.20.1. Trust the headers, not the `.pc`.

Three things are missing outright and have to be supplied before VTE will
configure:

| Missing | Why it matters | Fix |
|---|---|---|
| ncurses headers | VTE calls `tgetent`/`tgetstr` and its configure requires `ncurses.h` + `term.h` | cross-build ncurses; 6.5 defaults to wide-char, so symlink `libncurses.a`→`libncursesw.a` and `include/ncurses`→`include/ncursesw` |
| `gio-unix-2.0` | `vtestream-file.h` includes `<gio/gunixinputstream.h>`; headers absent entirely | copy `gio/gunix*.h` from the matching glib 2.29.18 source, write the `.pc` |
| `cairo-xlib` | `cairo-xlib.h` and the backend are present, only the `.pc` is missing | write the `.pc` |
| `dbus-1` | screen rotation; `libdbus-1.so.3` present, no headers | build dbus 1.6.18 far enough to generate `dbus-arch-deps.h` (it is architecture specific, so the host's copy is wrong), install headers, write the `.pc`. Its configure needs an XML backend, and expat headers are also missing — `expat.h` is architecture independent so the host's copy is fine |

## VTE 0.28.2

The last VTE with GTK+2 support, which is what GTK 2.20 pins us to.

```sh
./configure --host=arm-kindlehf-linux-gnueabihf --prefix=$PREFIX \
    --disable-shared --enable-static --disable-introspection \
    --disable-gnome-pty-helper --disable-gtk-doc --disable-Bsymbolic \
    --with-gtk=2.0 \
    CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" \
    CFLAGS="-O2 -g -fcommon -DG_CONST_RETURN=const -Wno-error \
            -Wno-deprecated-declarations -Wno-implicit-function-declaration \
            -Wno-incompatible-pointer-types"
```

`-DG_CONST_RETURN=const` because glib removed that macro long ago and VTE 0.28
is full of it. Extract the source fresh — seeding the cross tree from a native
build leaves x86 objects that `distclean` misses, and the ARM linker then fails
with *"file format not recognized"*.

## Three build fixes that live in configure.ac

These are real defects that only show up against a 2026 toolchain, and all
three are now handled by `configure.ac`:

1. **`-lm`** — a statically linked libvte needs `round()`, and pkg-config never
   lists libm. `AC_SEARCH_LIBS([round], [m])`.
2. **`GDK_X11_LIBS` was never set.** `Makefile.am` referenced it but no
   `PKG_CHECK_MODULES` ever populated it. It used to not matter because the X
   libraries arrived indirectly; binutils no longer resolves symbols through an
   intermediate DSO, so `libgdk-x11`'s `XGetGeometry` goes unresolved.
3. **The static-libvte probe ran too early**, before the X11 and libm checks,
   so it reported failure for unrelated reasons and silently fell back to a
   dynamic libvte. It now probes with the same libraries the real link gets.

## The two that cost the most time

**`gtk_init()` segfaults inside `g_thread_init_glib`.**

Modern binutils enables RELRO by default. A non-PIE executable referencing
libglib's exported variables needs copy relocations for them, and the linker
parks `g_threads_got_initialized` in `.data.rel.ro` — which the loader makes
read-only. glib 2.29 still keeps threads in a separate library, so
`gtk_init` → `g_type_init` → `g_thread_init_glib` writes that flag and dies on
a read-only page. Kindle's own binaries have no RELRO segment; the Kindle
branch of `configure.ac` now passes `-Wl,-z,norelro` to match.

Symptoms that mislead: calling `g_thread_init()` yourself makes it crash
*sooner*, and linking `-lgthread-2.0` does not help. A plain GTK2 hello-world
built with the same toolchain works, which is the fastest way to prove the
toolchain is fine and the problem is link flags.

**`access(path, X_OK)` lies on `/mnt/us`.**

The extension directory is a FUSE volume (`fuse.fsp`) mounted without
`default_permissions`, so `access()` reports perfectly runnable binaries as not
executable and kterm silently ran without its shim. Use `stat()` and test the
mode bits instead — see `is_executable()` in `kterm.c`.

## Debugging on the device

`gdb` is present. Symbols are stripped from the firmware libraries, so resolve
addresses against the sysroot copies:

```sh
gdb --batch -ex run -ex 'bt 15' -ex 'info sharedlibrary' --args ./kterm ...
```

Take each frame address, subtract the library's runtime `.text` start from
`info sharedlibrary`, add the `.text` vaddr from `readelf -S` on the sysroot
copy, and find the nearest symbol with `nm -D`.

Debug output is block-buffered when stdout is a pipe, so a crash eats the whole
trace. `kterm -d` now sets `setvbuf(_IONBF)` for exactly this reason.

Screenshots: the framebuffer is 8bpp grayscale, stride 1248, 1648 rows visible.

```sh
dd if=/dev/fb0 of=/tmp/fb.raw bs=1248 count=1648
```

Convert with any raw-to-PNG tool at 1248x1648, 8-bit grayscale.

Careful with `pkill -f kterm` over ssh — the pattern matches your own command
line and kills the connection. Use `pkill -x kterm`.

## Verifying the result

```sh
readelf -h kterm   # Flags: 0x5000400, hard-float ABI
readelf -d kterm   # no libvte entry: it is static
readelf -l kterm   # no GNU_RELRO segment
```

Then check every `DT_NEEDED` actually exists on the device before deploying.
