# AGENTS.md — guide for contributors and AI agents

Orientation for anyone working in `wiliOGBsp`. Read it before changing
anything. It is dense on purpose: most of what is here was paid for on
hardware, and rediscovering it costs a board.

## What this is

A board-support monorepo for the **FreeWili OG** (a.k.a. FreeWili 1 /
Classic), which carries **two RP2040s**: a *display* CPU (LCD, buttons, LEDs,
IR, audio, sensors) and a *main* CPU (sub-GHz radios, FPGA, breakout I/O).
Both are covered here. One CMake configure builds both.

- `bsp/common/` — CPU-agnostic: CRC, link framing, `DIAG()`, I2C helpers, DSP.
- `bsp/display_cpu/`, `bsp/main_cpu/` — per-CPU libraries, each owning its
  **authoritative** `platform/board.h` pin map.
- `bsp/boards/freewili_og.h` — Pico-SDK board header, board-global facts only.
- `apps/` — executables. Linking `fwog_display_bsp` or `fwog_main_bsp` is what
  makes an app a display-CPU or main-CPU app.
- `bsp/display_cpu/bootloader/` — the display serial bootloader. Pure logic
  (`bl_receiver`, `bl_policy`, `bl_console`'s parser) is host-tested; only
  `bl_flash`, `bl_jump`, `bl_ship` and the I/O halves touch the SDK.
- `bsp/main_cpu/display_update/` — main's boot-time updater and the
  declarations for the embedded display image.
- `bsp/display_cpu/lvgl/` — an optional LVGL 9 port: the ST7789 panel and the
  five buttons as an LVGL display and keypad. **LVGL is not vendored here** —
  the consuming project supplies it and the BSP builds `fwog_display_lvgl`
  only if a target named `lvgl` already exists. Costs ~390 KB of flash and
  ~143 KB of RAM, so it is opt-in. See `display_cpu/lvgl/fwog_lvgl.h`.
- `tests/` — host CTest tree. No SDK, no hardware.
- `tools/fw.py` — the task runner.

Umbrella headers: `bsp/fwog_display.h`, `bsp/fwog_main.h`.
Pin map: `docs/hardware/pinmap.md`, machine-checked against both `board.h`
files by `tests/test_pinmap_display.c` and `tests/test_pinmap_main.c`.

This BSP is a clean C re-implementation of the original C++ FreeWili 1
firmware library (`rmpLib`), **not** a fork of it.

## Bring-up order

Two milestones come before any feature work, one per CPU. They are the same
requirement seen from two sides: **the board's recovery affordances are
software, and nothing else is safe to build until they are known to work.**

**Display CPU — power the board off.** Before the LCD, LEDs, audio, sensors
or radios, a display app must declare `FWOG_POWER_DEFAULT()`, call
`fwog_power_poll(now_ms)` once per main-loop iteration, and **be observed on
real hardware** powering the board off on a 6 s red hold. The first two are
code; only the third is evidence. `board_init()` forces the declaration to
exist (contract rule 2); nothing can force the call.

**Main CPU — kick the watchdog.** A main app must declare
`FWOG_WATCHDOG_DEFAULT()` and call `board_watchdog_kick()` every main-loop
iteration. Both are code; only the declaration is enforced. `board_init()`
forces the declaration to exist (contract rule 7); nothing can force the
call. A main app that does not kick resets every 8.3 s forever and holds the
display in reset through `GUI_NRESET`, so the display never runs and the
power-off path above is unreachable no matter how correctly it was written.

Both templates already do the right thing, so an app scaffolded with
`fw new-app` starts compliant. The rule exists to stop anyone deferring or
stripping it during early bring-up — which is when it is most tempting and
most costly.

## Command vocabulary

| Command                                        | What it does                                                                                            |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------- |
| `fw build [app]`                               | configure + build both halves of `apps/<app>/` via the `target` preset (Windows) or `target-posix` (macOS/Linux) |
| `fw flash <app>`                               | reboot the **main** CPU into BOOTSEL, then copy `<app>_main.uf2`, which carries the display image. Never flashes the display CPU |
| `fw test`                                      | build + run the host CTest tree, then the `tools/tests/` Python unit tests for `fw.py` itself            |
| `fw new-app <name>`                            | copy `apps/template` -- the display+main pair -- to `apps/<name>/` and add it to `CMakeLists.txt`        |
| `fw bootloader`                                | build and flash the display serial bootloader — once per board                                          |
| `fw bootsel --cpu display\|main` or `--port P` | reboot one CPU into BOOTSEL from the host, no button (`--port` bypasses identification)                 |
| `fw console [--port P]`                        | attach to a CPU's USB CDC console                                                                       |
| `fw build [app] --baud N`                      | rebuild both binaries at a different link rate                                                          |

`--print` shows the command instead of running it. `<app>` is the app's
**folder** name -- `fw build ogvegas`, not `ogvegas_main`. The old
`_display`/`_main` spellings are deprecated: `fw` still accepts them, warns,
and strips the suffix, so `fw flash ogvegas_display` flashes `ogvegas_main`.
Bare `fw build` builds `template`.

`fw new-app <name>` always scaffolds the display+main pair -- there is no
single-CPU form -- and adds `add_subdirectory(apps/<name>)` to the top-level
`CMakeLists.txt` itself, under `apps/template`: a pair carries its own display
image, so where its line sits does not matter. See "App layout" below.

## App layout

**One folder per app, with the CPU halves inside it.** A display app and its
main companion are one deliverable — the main UF2 carries the display image
inside it — so they live together and are declared by one `CMakeLists.txt` at
the folder root:

```
apps/ogvegas/
    CMakeLists.txt      declares BOTH ogvegas_display and ogvegas_main
    display/main.c
    main/main.c
```

Two rules follow, and they matter:

- **The folder drops the suffix; the target keeps it.** Targets stay
  `ogvegas_display` and `ogvegas_main`, because the USB product string and the
  UF2 record are built from them and `fwog_embed_display_image()` pairs them by
  name. `fw` commands take the folder and add the suffix themselves.
- **`fwog_embed_display_image(<name>_main)` defaults to `<name>_display`** —
  the app declared beside it in the same file, so it always exists by then.
  Pass a second argument to embed someone else's
  (`fwog_embed_display_image(foo_main lcd_display)`), or set
  `FWOG_DISPLAY_FIRMWARE` to force one display into *every* main binary. That
  global override still requires its target to be declared first, which is why
  `apps/lcd` and `apps/bl` — the display-only apps, which have no main of
  their own to carry them — come first in the top-level `CMakeLists.txt`.

A folder with no `main/` has no companion. `apps/cpuprobe` is flat: it targets
neither CPU.

> ### Do NOT UF2-flash a display APPLICATION
>
> It will not boot, and it takes the display CPU off USB — which is the one
> CPU with no BOOTSEL button.
>
> A display app's `.uf2` links at `0x10021000` and a raw UF2 copy writes only
> that. It does **not** write the app metadata sector at `0x10020000` — only
> `bootloader/bl_receiver.c`, i.e. the serial update path, ever writes it. So
> the bootloader's `app_valid` check fails against stale metadata, the app
> never runs, and the board goes dark and stops enumerating.
>
> Display application firmware is meant to arrive **over the link, embedded in
> the main-CPU binary**, which is why `fw flash` only ever flashes main and
> refuses a folder with no `main/`. The correct sequence is:
>
> ```
> fw build <your_app>      # builds both halves; main embeds the display
> fw flash <your_app>      # main pushes the display image, WITH metadata
> ```
>
> A pair needs no extra configure step. To push a display app that has no main
> of its own — `lcd_display`, say — name it globally and use any main app:
>
> ```
> cmake --preset target -DFWOG_DISPLAY_FIRMWARE=lcd_display   # target-posix on macOS/Linux
> fw build template && fw flash template
> ```
>
> That is also the recovery path if you have already UF2-flashed a display
> app: flashing a main app carrying the display image you want will reset the
> display, stream the image over the link, write the metadata, and send RUN.
> `fw bootloader` (a UF2 copy of the bootloader itself to the display CPU)
> remains correct and is the once-per-board step.

## Invariants

Source comments throughout this tree cite **"the hardware notes"** — a
maintainer-held, per-fact record of what was measured on real boards, which
register read what, and which claims are still unverified. It is not published.
Every invariant it holds that can bite an app author is reproduced below or in
the relevant driver header, so nothing here depends on having it.

These bite an automated edit soonest.

- **You do not need to press a button to enter BOOTSEL.** Open a running CPU's
  USB CDC port at **1200 baud** and it reboots into the UF2 bootloader.
  Combined with per-CPU USB identification you can target exactly one CPU, so
  only one `RPI-RP2` volume ever appears. `fw bootsel --cpu display|main` does
  this for you, and `fw flash` does it automatically. Identification needs a
  binary built after `fwog_usb_product()` landed; older images report `"Pico"`
  and fall back to the manual prompt.
- **Put only one CPU in BOOTSEL at a time.** Both present the same USB serial
  and their drive letters swap, so with two mounted you cannot tell which is
  which — and a main-CPU image on the display drives a pin against the
  microphone's output.
- **UART0 on both CPUs is the inter-CPU link, and must never carry stdio.**
  Every app must call `fwog_configure_stdio(<target>)`. The SDK defaults stdio
  to UART; leaving that default in place corrupts the link with `DIAG()`
  traffic. Never `printf` in driver code — use `DIAG()`.
- **The BSP runs at 200 MHz** (`vreg` 1.15 V, flash `CLKDIV` 4), a rated
  RP2040 operating point rather than an overclock. `clk_peri` does **not**
  follow `clk_sys` by itself — what holds it at 200 MHz is
  `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK 1` in
  `bsp/boards/freewili_og.h`. Delete that and the link silently caps at
  3 Mbaud and the LCD at 24 MHz, because `clk_peri` feeds UART and SPI.
  Never hardcode a PIO divider or baud rate; derive from `clock_get_hz()`.
- **`PIN_LCD_DC` is 12 and `PIN_LCD_CS` is 13**, despite what the legacy names
  (`DISPLAY_CS`, `DISPLAY_LCD_CS_ACTIVE_LOW`) suggest. The legacy names lie.
- Display apps call `fwog_display_app(<target>)`; main apps call
  `fwog_main_app(<target>)`. These replace the bare `fwog_configure_stdio()` +
  `pico_add_extra_outputs()` pair and add the app-offset linker region.
- Main apps call `fwog_display_update_run()`, never `board_release_display()`.
- Never grow the 128 KB bootloader reserve. If the bootloader does not fit,
  cut the console.
- Never pass `-DPICO_BOARD` on a cmake command line.
- **Never add a watchdog to the display CPU.**
- **Every main-CPU app must call `board_watchdog_kick()` every main-loop
  iteration.** `board_init()` arms an 8.3 s watchdog (`FWOG_WATCHDOG_MS`, the
  RP2040 hardware maximum) — the only way to recover a hung main CPU. It is a
  worst-case lockup backstop, not a liveness monitor, which is why the window
  is as long as the silicon allows. Omitting the kick builds, links and
  flashes cleanly, then resets the board every 8.3 s forever, taking the
  display down with it via `GUI_NRESET` so it never enumerates.
  `apps/template/main/main.c` is the reference; **`template/display` is
  not** — it correctly has no kick, and copying its loop into a main app is
  how this has actually gone wrong. To recover, hold **red** through a reset:
  a watchdog reset re-samples the BOOTSEL strap, and with the display CPU not
  running its `MAIN_BOOT_OE` lockout is released, so red should reach main's
  `QSPI_SS` and drop main into BOOTSEL within one watchdog period. That last
  step is reasoned from the pad reset default, not measured.

## The FwOGapp contract

Firmware built by this BSP must be identifiable, versioned, self-describing
and recoverable without a human touching the board. Seven rules. Five are
build failures; two are link errors whose limits are spelled out below.

**1. Every app declares `VERSION` and `DESCRIPTION`.**

```cmake
fwog_display_app(bench_display
    VERSION 001
    DESCRIPTION "Bench console for the display drivers: charger, RTC, ...")
```

`VERSION` is exactly three digits, bumped by hand. `DESCRIPTION` is required
and has no default — it is what the App Explorer shows a human choosing what
to flash. `NAME` defaults to the target minus its `_display`/`_main` suffix.
Missing or malformed is a configure error.

**2. The red button powers the board OFF. It cannot power it ON.**

Every display app declares a power policy or **does not link** —
`board_init()` references a symbol only these macros define:

- `FWOG_POWER_DEFAULT()` — red held 6 s → `fwog_ship_enter()`, with the
  countdown on the WS2812 bar. Then call `fwog_power_poll(now_ms)` once per
  main-loop iteration, and take your buttons from its return value rather than
  calling `fwog_buttons_poll()` again: that call carries the debounce state
  the hold machine depends on.
- `FWOG_POWER_CUSTOM()` — this app owns power itself. `bl_display` does,
  truthfully, through `bl_ship.c`.

**Read this limit.** The link error proves a policy was *declared*, not that
`fwog_power_poll()` is *called*. No linker can see that, and the runtime
backstop is unavailable because the display CPU must never have a watchdog.

**Waking is hardware. Do not try to implement it.** A gray hold reaches the
BQ25896's `/QON` pin; a USB attach re-enables the FET. Both work with nothing
running, which is exactly why they work.

**3. 1200-baud BOOTSEL must not be built away.**
`PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0` is a configure error. `FWOG_DIAG != 1`
is a warning, not an error — it is a supported build (`build-nodiag/`), but it
drops USB CDC and the reset path with it.

**4. The FPGA bitstream is frozen.**
`tools/tests/test_fpga_bitstream.py` pins its SHA-256. If it fails, read the
provenance note in `bsp/main_cpu/fpga/fpga_bitstream.h` before touching
anything: a bitstream of identical size, symbol name and size constant has
already been swapped for FreeWili 2 gateware once. **`CDONE=1` cannot detect
this** — it reports structural acceptance, so it comes up identically for the
wrong gateware. Only provenance can tell the two apart.

**5. Every app declares a USB identity: IDs, product string, manufacturer.**

`fwog_usb_ids()` and `fwog_usb_product()` in `bsp/CMakeLists.txt` set all
three. Both are called for you by `fwog_display_app()` / `fwog_main_app()`, so
an app that uses those macros gets this automatically.

**VID/PID are `093C:2054` (main) and `093C:2055` (display and the
bootloader).** `093C` is Intrepid Control Systems. These are what
`freewili-finder` maps to `SerialMain`/`SerialDisplay`. The old SDK default
`2E8A:000A` still resolves through that tool's fallback branch, which
disambiguates *by hub port* — positional, and labelled "for older firmware" in
its own source. `fw.py` identifies by PID first, product-string prefix second.

**The product string is `FWOG <cpu> <name> <version>`** — for example
`FWOG display bench 001`. `<cpu>` is `display` or `main`, `<name>` is the
app's `NAME` (the target minus its suffix by default) and `<version>` is its
three-digit `VERSION`. One name feeds both this string and the UF2 record, so
the string on the wire and the string in the image cannot disagree. This is
the *second* thing `fw.py` identifies a CPU by, so its `FWOG <cpu> ` prefix is
load-bearing — do not reformat it.

**The manufacturer string is `FreeWili OG`**, board-wide rather than per-app.
The SDK default is `Raspberry Pi`, which is true of the silicon and wrong
about the product a user is holding. Override it with
`-DFWOG_USB_MANUFACTURER=...`; it is a CACHE variable because
`fwog_usb_product()` is called from `apps/*/CMakeLists.txt`, a sibling scope
that does not inherit a plain `set()` from `bsp/`. Nothing identifies a CPU by
it — it is presentation.

**Both strings are capped at 47 characters and over-long or empty is a
configure error.** `stdio_usb_descriptors.c` copies string descriptors with
`len < USBD_DESC_STR_MAX - 1`, so exceeding the cap truncates *silently*. The
empty-manufacturer check is not defensive padding: a scope mistake once made
the variable empty, every length check still passed, and the board would have
enumerated blank with nothing saying so.

**`DESCRIPTION` is not a USB string.** It is a UF2-record field (rule 6) and
never reaches the wire — it is far longer than 47 characters by design,
because it is what the App Explorer shows a human choosing what to flash.
Do not try to put it in a descriptor.

**`bcdDevice` is unreachable — do not pursue it.** It is hardcoded at
`stdio_usb_descriptors.c:111` and `tud_descriptor_device_cb()` is a strong
symbol in a library whose sources compile into the target, so the only route
is replacing the descriptor set that 1200-baud BOOTSEL rides on. That would
trade the display CPU's only remote recovery for a version field it already
carries in its product string and its UF2 record.

**When changing USB IDs, flash an application first.** The display CPU has no
BOOTSEL button, and `fw bootloader` needs a working 1200-baud open
against the *running* image.

**6. Every image carries a `fwog_uf2_info_t` record.**
`bsp/common/uf2_info.h` — an 8-byte magic (`FWGOINFO`), name, description,
version, build, CRC. `tools/check_uf2_info.py` runs POST_BUILD and fails the
build if it is missing or wrong.

Three things a reader needs to know about it:

- **A main UF2 contains TWO records** — its own (`cpu=main`) and the display
  image it embeds (`cpu=display`). Consumers key on `cpu`; never assume one.
- **Display applications carry no `build`/`build_ts`, deliberately.**
  `display_update.c` skips the transfer by comparing the image CRC32, so a
  build-varying byte would force a display reflash on every commit. The
  bootloader *is* exempt: it is UF2-flashed with no CRC-skip path.
- **Nothing in the firmware references the record**, so it is held by
  `-Wl,--undefined=fwog_uf2_info`. `__attribute__((retain))` is ignored by
  this toolchain, and the SDK's KEEP'd `.binary_info.keep.*` section is wrong
  here — picotool walks that region as an array of pointers.

**7. Every main-CPU app declares a watchdog policy.**

The first rule here that binds *only* main apps, rather than display apps.
Every main app declares a policy or **does not link** — `board_init()`
references a symbol only these macros define, exactly as rule 2 does on the
display side:

- `FWOG_WATCHDOG_DEFAULT()` — this app calls `board_watchdog_kick()` from its
  main loop. What almost every main app should say.
- `FWOG_WATCHDOG_CUSTOM()` — this app owns the watchdog itself: it builds
  with `FWOG_WATCHDOG_MS` 0, or kicks from somewhere other than a main loop.
  Legitimate, but it is a claim the declarer is making, not one anything
  checks.

**Read this limit — it is the same limit rule 2 states.** The link error
proves a policy was *declared*, not that `board_watchdog_kick()` is reached
on every path. No linker can see that.

## Consuming this BSP from another project

`add_subdirectory(<path>/wiliOGbsp/bsp)` is supported. Two things differ from
working in-tree, both because the consuming project is the top-level one:

- **Build-helper scripts resolve through `FWOG_TOOLS_DIR`**, which defaults to
  this repository's `tools/` relative to `bsp/CMakeLists.txt`. It is a CACHE
  variable because `fwog_add_uf2_info()` and friends are called from
  `apps/*/CMakeLists.txt`, a sibling scope. Do not reintroduce
  `${CMAKE_SOURCE_DIR}/tools/...` at those call sites — that is the top-level
  project, so for a consumer it points into *their* tree and the build fails
  with a missing `gen_uf2_info.py`.
- **`fw flash` needs `--uf2 <path>`.** `fw.py` derives its repo root from its
  own location, so without that flag it looks for the image in *this* build
  tree. Everything else the command does — identify the CPU, touch it at 1200
  baud, wait for the volume, refuse when two are mounted — is unaffected.
  `fw bootsel` is location-independent and needs nothing.

The consumer still supplies what the top-level `CMakeLists.txt` does here:
`PICO_BOARD`, `PICO_BOARD_HEADER_DIRS` pointing at `bsp/boards`,
`PICO_STDIO_UART 0`, and `find_package(Python3)`.

## Porting a FreeWili 2 app

When converting a FreeWili 2 application to the OG's five-button front panel,
the following mapping is recommended.

- **Double-click gray** — show the D-pad controls.
- **Double-click yellow** — show home, OK, cancel and page.
- **Text and number entry** — use an on-screen keyboard driven by the arrow
  pad rather than trying to reproduce the FreeWili 2 input model.

The recommended arrow-pad layout is green (centre) for select/OK, yellow for
left, blue for right, gray for up, and red for down.

## Conventions

- Conventional Commits: `feat:`, `fix:`, `docs:`, `refactor:`, `test:`.
- `build/`, `build-tests/`, `*.uf2`, `*.elf`, `*.bin` are git-ignored. The
  checked-in FPGA bitstream `bsp/main_cpu/fpga/fpga_default_v5.bin` is an
  explicit exception — it is a source artifact, not a build output.
- Host tests must not require the SDK or a board. If a change cannot be
  host-tested, say so in the commit message rather than leaving it implied.
