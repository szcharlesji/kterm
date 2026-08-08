[![Build Status](https://travis-ci.com/bfabiszewski/kterm.svg?branch=master)](https://travis-ci.com/bfabiszewski/kterm) [![Coverity Status](https://img.shields.io/coverity/scan/10306.svg)](https://scan.coverity.com/projects/bfabiszewski-kterm)
# \# kterm

This is a simple GTK+ terminal emulator with embedded virtual keyboard. It is based on VteTerminal library. Some initial settings may be defined in [kterm.conf](kterm.conf) file. Keyboard layouts are defined in [xml config](layouts/keyboard.xml) files. The keyboard config files follow the same rules as matchbox keyboard configs (backward compatible with kterm 1.x which used embedded matchbox keyboard).

Kterm has been developed for Kindle Touch. It is reported to also work on Paperwhites. Generally it should work on any platform which supports GTK+, either version 2 or 3.

#### Touch

kterm mediates button 1 rather than handing it straight to the terminal,
because at press time a tap, a drag and a hold are indistinguishable. The
gesture is classified as it develops:

| gesture | result |
| --- | --- |
| tap | click at that cell |
| drag | scroll |
| hold ~600 ms, then drag | precise drag: real press/motion/release reach the application, so text selection and dragging a pane divider work |
| two finger tap | popup menu (right mouse button elsewhere) |
| status bar button | popup menu |

Dragging emits scroll events rather than moving the scrollback directly. That
detail matters: VTE reports a wheel button to the application when it has asked
for mouse tracking and scrolls its own buffer when it has not, so the same
gesture pages a full screen TUI and scrolls the scrollback at a shell without
kterm having to work out which one is in front.

A status bar above the terminal shows the clock, date and battery and carries a
menu button. Set `statusbar = 0` to hide it.

#### The escape sequence shim (`ktsh`)

Kindle firmware ships GTK+ 2.20, which pins kterm to VTE 0.28 — a terminal
emulator from 2011. VTE of that vintage *prints* any escape sequence it does
not recognise as literal text, so a modern shell or TUI leaves debris all over
the screen, and it cannot answer the queries those programs use to discover
what the terminal looks like.

`ktsh` is a small POSIX program that kterm runs the child shell under. It gives
the shell its own pty and filters both directions:

  * removes shell integration sequences (`OSC 7` cwd, `OSC 133` prompt marks,
    `OSC 633`), hyperlink wrappers, kitty graphics, Sixel, `XTGETTCAP` — the
    escape boxes that appear after every fish or zsh prompt;
  * answers `OSC 10/11/12` colour queries with the colours kterm actually
    painted, so applications stop assuming a dark background;
  * folds 24-bit colour onto a palette VTE can render;
  * rewrites VTE's legacy X10 mouse reports into the SGR form (`DECSET 1006`)
    that applications now ask for, which is what makes touch work in nvim and
    friends instead of spraying `␛[M` at the screen.

Because it filters the byte stream rather than the local shell, it fixes remote
sessions too — `ssh` into a machine running fish and the prompt is clean.

Set `shim = 0` in `kterm.conf`, pass `-S 0`, or set `KTSH_DISABLE=1` to bypass
it. `ktsh -h` lists its own options; `ktsh -d <file>` logs both byte streams.

#### Keyboard [XML config](layouts/keyboard.xml) **\<nodes\>** and **attributes**:
  * **\<layout\>** - layout
  * **\<row\>** - row
  * **\<space\>** - empty spacer
  * **\<key\>** - key, attributes:
    * **extended** = [true|false], *optional*, if true only visible in landscape mode, defaults to false;
    * **fill** = [true|false], *optional*, if true button will expand to use all available space, defaults to false;
    * **width** = [width in kterm units], *optional*, 1000 is standard width, 2000 will be double width key and so on; defaults to 1000;
    * **obey-caps** = [true|false], *optional*, should button change on caps lock press, defaults to false;
  * **\<default\>** - default variant (no modifier)
  * **\<shifted\>** - shifted variant (shift/caps lock modifier pressed)
  * **\<mod1\>** - mod1 variant (mod1 modifier pressed)
  * **\<mod2\>** - mod2 variant (mod2 modifier pressed)
  * **\<mod3\>** - mod3 variant (mod3 modifier pressed)
    * attributes for all variant nodes (default, shifted, …):
    * **display** = [character|image\:/path/to/image], *required*, character to display or image path (absolute must start with slash, otherwise relative to config);
    * **action** = [character|special name|modifier\:name], *required for keys with image label, modifiers, special buttons*, character sent to terminal, name of special action, or name of modifier, defaults to *display* attribute value;
    * for a list of special key names see [this lookup table](https://github.com/bfabiszewski/kterm/blob/master/parse_layout.c#L41); valid modifier keys are: shift, caps, ctrl, alt, mod1, mod2, mod3
 
 
#### Command line options:
```
$ ./kterm -h
Usage: kterm [OPTIONS]
        -c <0|1>      color scheme (0 light, 1 dark)
        -d            debug mode
        -e <command>  execute command in kterm
        -E <var>      set environment variable
        -f <family>   font family
        -h            show this message
        -k <0|1>      keyboard off/on
        -l <path>     keyboard layout config path
        -m <0|1>      mouse reporting off/on
        -o <U|R|L>    screen orientation (up, right, left)
        -s <size>     font size
        -S <0|1>      escape sequence shim (ktsh) off/on
        -t <encoding> terminal encoding
        -u <B|I|U>    cursor shape (block, I-beam, underline)
        -v            print version and exit
```

#### Fonts

Nerd Font glyphs work, with one caveat: pango 1.26 on the device predates
HarfBuzz shaping, so programming ligatures are not composed. See
[fonts/README.md](fonts/README.md) for installing a font — the Kindle rootfs is
read only, so kterm ships its own `fonts.conf` and the launcher points
`FONTCONFIG_FILE` at it.

For a list of what constitutes valid encodings, check [this list][iana-character-sets] or the list returned by `iconv -l`.

#### Screenshots
![kterm reversed color scheme][screenshot1] 
![kterm hidden keyboard][screenshot2]
![kterm landscape mode][screenshot3]

#### Changelog
  * **2.6**: option for setting cursor shape (by efskap)
  * **2.5**: option for setting terminal encoding (by deuill)
  * **2.4**: fix: some special keys won't work, add path to terminfo in Kindle start script
  * **2.3**: fix: issue with uneven keyboard rows height, unresponsivness when popup menu is cancelled, autotools build and other minor bugs
  * **2.2**: fix launching kterm with hidden keyboard; add config/command line option for screen rotation (screen orientation will also be restored to initial value after quitting kterm); fix flickering at start; remove unneeded files from kindle package, add high resolution key images; some other minor issues
  * **2.1**: mainly small bugfixes, compilation warnings
  * **2.0**: major rewrite; added native keyboard which replaces matchbox keyboard - better stability, easier to compile and maintain; updated to build on modern frameworks; added autotools project; option to rotate screen on Kindle; option to add environment variables on command line
  * **0.7**: keyboard layout for Paperwhite (by nasser), also larger labels for Touch keyboard
  * **0.6**: parameter for choosing different keyboard layout from config and command line; new keyboard layout for Touch and Paperwhite (by cubemike99 and DuckieTigger)
  * **0.5**: support for mouse events (for ncurses apps etc)
  * **0.4**: added command line options(see kterm -h), fixed colors
  * **0.3**: xterm-like "-e" option support (by Fvek), simple config file added, removed bug with blocked key-up event when quiting with exit command (traced by aditya3098)
  * **0.2**: patch for matchbox-keyboard to get rid of absolute paths in config file
  * **0.1**: first release

#### Building
* dependencies: [GTK+](https://github.com/GNOME/gtk) version 2 or 3 (use `--enable-gtk3`), [VTE](https://github.com/GNOME/vte), optionally [dbus](https://www.freedesktop.org/wiki/Software/dbus/) for Kindle screen rotation
* `$ ./autogen.sh`
* `$ ./configure`
* `$ make`
* `$ sudo make install`
* for Kindle build use `--enable-kindle --sysconfdir=/mnt/us/extensions/kterm` configure options. If you are cross-compiling run `make dist-kindle` instead of `make install`. It will create zip package in build directory.

#### Packages 
* for Kindle Touch/Paperwhite are available at http://www.fabiszewski.net/kindle-terminal/

#### License
 * GPL version 3 or (at your option) any later version

[screenshot1]:http://www.fabiszewski.net/kindle-terminal/screenshot_v2_1.png "kterm screenshot"
[screenshot2]:http://www.fabiszewski.net/kindle-terminal/screenshot_v2_2.png "kterm screenshot"
[screenshot3]:http://www.fabiszewski.net/kindle-terminal/screenshot_v2_3.png "kterm screenshot"
[iana-character-sets]: https://www.iana.org/assignments/character-sets/character-sets.txt
