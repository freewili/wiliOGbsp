#!/usr/bin/env python3
"""fw — FreeWili OG BSP task runner (cross-platform).

Commands:
  fw build [app] [--baud N]  configure+build an app for the RP2040 target
  fw flash <app> [--uf2 P]   reboot the app's own CPU into BOOTSEL, then copy
                              its .uf2 (--uf2 for a UF2 built outside this
                              repo, e.g. a project consuming the BSP)
  fw bootloader              build and flash the display serial bootloader
  fw bootsel --cpu C|--port P  reboot one CPU into BOOTSEL, no button
  fw console [--port P]      attach to a CPU's USB CDC console
  fw test                    build+run the host unit tests (CTest, no
                              hardware), then the tools/tests/ unit tests
                              for this task runner
  fw new-app <name> --cpu display|main
Add --print to print the command(s) instead of running them.

`fw console` supersedes the `fw mon` that earlier versions of this file
promised: both are "read a CPU's USB CDC output", and one command with a
product-string filter covers the bootloader and applications alike. It
needs pyserial.

--baud sets the inter-CPU link rate. It is a CMake cache variable, not a
runtime flag, because the rate is compiled into BOTH binaries and they must
agree; selecting a slower rate therefore rebuilds. 12.5 Mbaud is the design
rate and the PL011 ceiling; the fallback ladder is 10000000, 8333333, then
6250000.

Flashing is UF2 drag-and-drop. BOOTSEL is no longer a physical act: a CPU
running code with USB up is identified by its USB product string and rebooted
into the bootloader by opening its CDC at 1200 baud (the hardware record and 30), so
`fw flash` targets exactly one CPU and exactly one RPI-RP2 volume can appear.
When the CPU cannot be identified -- an older binary that predates
fwog_usb_product(), or a hung one -- it falls back to asking you to press
BOOTSEL and waiting. `fw bootloader` is the once-per-board step that puts the
serial bootloader on the display CPU in the first place; after that, display
firmware arrives over the link inside main's UF2.
"""
import argparse
import collections
import pathlib
import shutil
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
# Must name an app that actually exists in apps/, or a bare `fw build`
# always fails. Only template_display and template_main exist so far.
DEFAULT_APP = "template_main"

# The display serial bootloader. Flashed once per board by UF2; after that
# the display's application firmware arrives over the link, embedded in
# main's UF2.
BOOTLOADER_APP = "bl_display"

# Raspberry Pi's USB vendor ID. Both RP2040s used to enumerate under it, and
# an image built before fwog_usb_ids() landed still does -- which is why every
# filter here accepts it alongside the one below.
RPI_USB_VID = 0x2E8A

# Intrepid Control Systems' vendor ID, and the two product IDs FreeWili's own
# finder maps to SerialMain and SerialDisplay (freewili-finder,
# src/fwfinder.cpp:40). fwog_usb_ids() in bsp/CMakeLists.txt sets these.
#
# Identifying by PID rather than by product string matters because the string
# is a string: it can be renamed, and an older image can carry a stale one.
# The PID is set by the BSP in the same call every app already makes.
ICS_USB_VID = 0x093C
CPU_PID = {"display": 0x2055, "main": 0x2054}

# Either VID may legitimately appear while a fleet is part-updated.
FWOG_USB_VIDS = (RPI_USB_VID, ICS_USB_VID)
# What fwog_usb_product() puts in bl_display's USBD_PRODUCT. Without a product
# string the bootloader and a running application are indistinguishable on the
# host -- both would say "Pico".
# The full string is now "FWOG display bl <NNN>". This value deliberately
# STOPS before the version: _pick_console_port matches it as a SUBSTRING, so
# leaving the version out means bumping the bootloader's VERSION does not
# quietly break `fw console`.
FWOG_BL_USB_PRODUCT = "FWOG display bl"
MSYS_GCC = pathlib.Path("C:/msys64/mingw64/bin/gcc.exe")

# `pid` is last and defaulted so the many four-argument constructions in the
# tests, written before PID identification existed, keep working unchanged.
CpuPort = collections.namedtuple("CpuPort", "device vid serial product pid",
                                 defaults=(None,))

# Windows' usbser.sys does not surface the USB product string to pyserial
# (see _port_product), but SetupAPI has it. Get-PnpDeviceProperty reaches it
# unelevated; the registry copy under Properties\{540b947e-...}\0004 does NOT
# -- reading that returns "Requested registry access is not allowed".
#
# The product string lives on the composite PARENT, not on the COM port, which
# is why the parent is looked up first. The parent's instance ID ends in the
# USB serial number, e.g. USB\VID_2E8A&PID_000A\E463A8574B114535, and that
# serial is the join key back to pyserial -- which does report serial_number
# on Windows.
#
# BOTH VIDs are matched. Filtering on 2E8A alone silently returned NO product
# strings once the BSP moved to 093C -- found on a board, not in a test, on
# 2026-07-28: identification still worked because it now keys on PID first,
# but `fw console`'s substring match and the "nothing matched" diagnostic had
# both quietly lost their input.
_PNP_SCRIPT = (
    "Get-PnpDevice -Class Ports | "
    "Where-Object { $_.Status -eq 'OK' -and ("
    "$_.InstanceId -like '*VID_2E8A*' -or $_.InstanceId -like '*VID_093C*') } | "
    "ForEach-Object { "
    "$p = (Get-PnpDeviceProperty -InstanceId $_.InstanceId "
    "-KeyName 'DEVPKEY_Device_Parent').Data; "
    "$d = (Get-PnpDeviceProperty -InstanceId $p "
    "-KeyName 'DEVPKEY_Device_BusReportedDeviceDesc' "
    "-ErrorAction SilentlyContinue).Data; "
    "'{0}|{1}' -f $p.Split('\\')[-1], $d }"
)


def configure_command(baud=None):
    cmd = ["cmake", "--preset", "target"]
    if baud is not None:
        # A cache variable, because both binaries compile it in and must
        # agree. Unlike PICO_BOARD (which must NEVER be passed on a command
        # line -- see the hardware record), this one has no CMakeLists-side default
        # being overridden; bsp/CMakeLists.txt declares it as a cache entry
        # precisely so it can be set from here.
        cmd.append(f"-DFWOG_LINK_BAUD={baud}")
    return cmd


def build_command(app):
    return ["cmake", "--build", "--preset", "target", "--target", app]


def uf2_path(app, override=None):
    """Where `fw flash <app>` looks for the image.

    REPO_ROOT comes from this file's own location, so the default is THIS
    repository's build tree. That is right for in-tree apps and wrong for a
    project that consumes the BSP as a submodule: its build output lives in
    its own tree, which this script has no way to guess. `--uf2 <path>` is
    the escape hatch for that case -- the rest of `fw flash` (identify the
    CPU, touch it at 1200 baud, wait for the volume, refuse when two are
    mounted) is exactly what a consumer wants, and none of it depends on
    where the file came from.

    The CPU is still inferred from `app`, so a consumer passing --uf2 must
    still name its app with the _display/_main suffix -- see app_cpu().

    The build tree mirrors apps/, and apps/ is one folder per app PAIR with
    the _display/_main suffix dropped (AGENTS.md: "the folder drops the
    suffix; the target keeps it") -- e.g. apps/bench holds both bench_display
    and bench_main. So the .uf2 for `app` lands in build/apps/<folder>/, not
    build/apps/<app>/, and folder must be recovered by stripping the suffix
    app_cpu() just used to identify the CPU."""
    if override is not None:
        return pathlib.Path(override)
    folder = app
    for suffix in ("_display", "_main"):
        if app.endswith(suffix):
            folder = app[:-len(suffix)]
            break
    return REPO_ROOT / "build" / "apps" / folder / f"{app}.uf2"


def app_cpu(app):
    """Which CPU an app targets, from its name suffix. The suffix is a
    convention the templates enforce; it keeps `fw flash` from telling you to
    BOOTSEL the wrong processor."""
    if app.endswith("_display"):
        return "display"
    if app.endswith("_main"):
        return "main"
    raise ValueError(
        f"cannot tell which CPU '{app}' targets: "
        "app names must end in _display or _main"
    )


def _removable_drives_win():
    """Drive roots Windows reports as removable.

    Probing every letter D-Z is the obvious approach and the wrong one: a
    disconnected mapped network drive can block the filesystem call for many
    seconds, and `do_flash` polls twice a second, so one stale mount turns a
    bounded wait into a silent hang. GetDriveTypeW answers from the drive
    table without touching the device, so filtering to DRIVE_REMOVABLE (2)
    first means we never stat a network or optical drive at all."""
    import ctypes
    DRIVE_REMOVABLE = 2
    out = []
    for d in "DEFGHIJKLMNOPQRSTUVWXYZ":
        root = f"{d}:\\"
        try:
            if ctypes.windll.kernel32.GetDriveTypeW(root) == DRIVE_REMOVABLE:
                out.append(pathlib.Path(root))
        except OSError:
            continue
    return out


class MultipleRp2VolumesError(RuntimeError):
    """More than one RPI-RP2 BOOTSEL volume is mounted at once.

    Both RP2040s on this board present indistinguishable RPI-RP2 volumes,
    and both being in BOOTSEL at once is a realistic bring-up state (e.g.
    after double-tapping BOOTSEL on both, or forgetting one was already in
    it). There is no way to tell them apart from the mount alone, so
    guessing which one to flash risks flashing the wrong half of the board;
    refusing is the only safe move."""


def _scan_rpi_rp2(candidates):
    """Return the single RP2040 BOOTSEL volume among `candidates`, or None
    if there isn't one. Raises MultipleRp2VolumesError if more than one
    candidate looks like an RP2040 in BOOTSEL (INFO_UF2.TXT at its root,
    which is what the RP2040 bootrom presents).

    Split out from find_rpi_rp2() so the decision logic is testable without
    touching the real filesystem: tests inject a list of Path candidates
    directly."""
    found = []
    for c in candidates:
        try:
            if (c / "INFO_UF2.TXT").exists():
                found.append(c)
        except OSError:
            continue
    if len(found) > 1:
        names = ", ".join(str(f) for f in found)
        raise MultipleRp2VolumesError(
            f"multiple RPI-RP2 volumes found ({names}): both RP2040s on "
            "this board look identical in BOOTSEL mode, so which one is "
            "which cannot be told from the mount alone. Put ONLY the CPU "
            "you want to flash into BOOTSEL (power down or disconnect the "
            "other) and try again.")
    return found[0] if found else None


def find_rpi_rp2():
    """Return the mount path of a connected RP2040 in BOOTSEL mode, or None.

    Identified by INFO_UF2.TXT at the volume root, which is what the RP2040
    bootrom presents. Raises MultipleRp2VolumesError if more than one such
    volume is mounted -- see that class for why this refuses rather than
    picking one."""
    if sys.platform == "win32":
        candidates = _removable_drives_win()
    else:
        # Mount points sit one or two levels down (/media/<label> or
        # /media/<user>/<label>). rglob would descend into every directory of
        # every mounted filesystem, which on a machine with a NAS share under
        # /media can take minutes per call.
        candidates = []
        for base in ("/media", "/run/media", "/Volumes"):
            b = pathlib.Path(base)
            if not b.is_dir():
                continue
            try:
                for lvl1 in b.iterdir():
                    if not lvl1.is_dir():
                        continue
                    candidates.append(lvl1)
                    try:
                        candidates += [p for p in lvl1.iterdir() if p.is_dir()]
                    except OSError:
                        continue
            except OSError:
                continue
    return _scan_rpi_rp2(candidates)


def _host_toolchain_args():
    """Pin a host C compiler and Ninja for the standalone tests/ tree. Entries
    are omitted when a tool is absent, so CMake falls back to its defaults."""
    args = []
    if sys.platform == "win32":
        if MSYS_GCC.exists():
            args += [f"-DCMAKE_C_COMPILER={MSYS_GCC}"]
        ninja_root = pathlib.Path.home() / ".pico-sdk" / "ninja"
        ninja = next(iter(sorted(ninja_root.glob("*/ninja.exe"), reverse=True)), None) \
            if ninja_root.is_dir() else None
        if ninja is not None:
            args += ["-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}"]
    elif shutil.which("ninja"):
        args += ["-G", "Ninja"]
    return args


def test_command():
    """The full `fw test` pipeline: the host CTest tree (bsp/ logic, no
    hardware) plus the tools/tests/ unit tests for this task runner itself.
    Neither tree ran the other before this: CTest never touched tools/tests,
    so its nine tests were reachable by nothing in the repo. Both must pass
    for `fw test` to pass."""
    tests_dir = REPO_ROOT / "tests"
    build_dir = REPO_ROOT / "build-tests"
    configure = ["cmake", "-S", str(tests_dir), "-B", str(build_dir)]
    configure += _host_toolchain_args()
    return [
        configure,
        ["cmake", "--build", str(build_dir)],
        ["ctest", "--test-dir", str(build_dir), "--output-on-failure"],
        [sys.executable, "-m", "unittest", "discover",
         "-s", str(REPO_ROOT / "tools" / "tests")],
    ]


def new_app(name, cpu, repo_root=REPO_ROOT):
    if cpu not in ("display", "main"):
        raise ValueError(f"cpu must be 'display' or 'main', got {cpu!r}")
    # `fw flash` derives the CPU from the app-name suffix alone -- it never
    # inspects which BSP library the app linked. So an app named foo_main but
    # built from the display template would tell the operator to BOOTSEL the
    # main CPU and flash a display binary onto it. Refuse the contradiction
    # here, where it is still cheap.
    if app_cpu(name) != cpu:
        raise ValueError(
            f"app name {name!r} implies the {app_cpu(name)} CPU but --cpu is "
            f"{cpu!r}; `fw flash` trusts the name suffix, so these must agree")
    # apps/<folder>/{display,main}/main.c with ONE CMakeLists.txt at the
    # folder root: a display app and its main companion are one deliverable
    # (the main UF2 carries the display image inside it), so they are scaffolded
    # together and declared together. `name` is the TARGET name and keeps its
    # _display/_main suffix -- fw flash and the USB product string both read
    # it -- while the folder is that name without the suffix.
    folder = name.rsplit("_", 1)[0]
    src = pathlib.Path(repo_root) / "apps" / "template"
    dest = pathlib.Path(repo_root) / "apps" / folder
    if dest.exists():
        raise FileExistsError(dest)
    shutil.copytree(src, dest)

    # The template is a pair. Keep only the half that was asked for, and drop
    # the other half's declaration from the merged CMakeLists.
    cml = dest / "CMakeLists.txt"
    text = cml.read_text().replace("template_", f"{folder}_")
    other = "main" if cpu == "display" else "display"
    if (dest / other).exists():
        shutil.rmtree(dest / other)
        # Each target's block runs from its add_executable() to the blank line
        # before the next one; drop the unwanted target's block wholesale.
        blocks = [b for b in text.split("\n\n")
                  if f"add_executable({folder}_{other}" not in b]
        text = "\n\n".join(blocks)
        if cpu == "main":
            # There is no <folder>_display to carry, so the default pair
            # lookup in fwog_embed_display_image() would fail at configure
            # time. Leave the call in, commented, with what to do about it --
            # a main app that never embeds one cannot update the display CPU.
            text = text.replace(
                f"fwog_embed_display_image({name})",
                "# No display app in this folder to carry. Either scaffold one\n"
                "#   fw new-app " + f"{folder}_display --cpu display\n"
                "# and drop the comment below, or name another app's display\n"
                "# explicitly, e.g. fwog_embed_display_image("
                f"{name} template_display).\n"
                "# Without it this app cannot update the display CPU at all.\n"
                f"# fwog_embed_display_image({name})")
    cml.write_text(text)
    return dest


def _run(cmds, do_print):
    if isinstance(cmds[0], str):
        cmds = [cmds]
    for c in cmds:
        if do_print:
            print(" ".join(str(x) for x in c))
        else:
            subprocess.run(c, cwd=REPO_ROOT, check=True)


class ConsolePortError(RuntimeError):
    """No single USB CDC port matched, so there is nothing safe to attach to."""


def _port_product(p):
    """The USB product string for a pyserial port, or "" if unavailable.

    Windows' inbox usbser.sys driver does not surface it: pyserial reports
    product=None and description="USB Serial Device (COMxx)" for every RP2040
    CDC, with manufacturer="Microsoft". Linux and macOS do populate product.
    Checking description too costs nothing and helps on platforms that put the
    product string there instead."""
    for field in (getattr(p, "product", None), getattr(p, "description", None)):
        if field:
            return field
    return ""


def _parse_pnp_products(text):
    """Parse `serial|product` lines into a {serial: product} map.

    Pure, so the tests never spawn a shell. Malformed and blank lines are
    skipped rather than raising: this is a best-effort enrichment, and its
    correct failure mode is "no product strings", which degrades to the
    manual BOOTSEL prompt."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "|" not in line:
            continue
        serial, _, product = line.partition("|")
        serial = serial.strip()
        if serial:
            out[serial] = product.strip()
    return out


def _pnp_products():
    """{serial: product} for RP2040 CDCs on Windows; {} everywhere else.

    Every failure returns {} rather than raising. A missing powershell.exe, a
    non-zero exit or a timeout must not break `fw flash` -- without product
    strings nothing is identified, so it falls back to asking the operator to
    press BOOTSEL, which is exactly what it did before this existed."""
    if sys.platform != "win32":
        return {}
    try:
        proc = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive",
             "-Command", _PNP_SCRIPT],
            capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return {}
    if proc.returncode != 0:
        return {}
    return _parse_pnp_products(proc.stdout)


def _cpu_ports(ports=None, products=None):
    """Every serial port, as CpuPort, with the product string filled in where
    the platform hides it from pyserial.

    Foreign-VID ports are kept, not filtered: _pick_console_port lists every
    port it saw in its "nothing matched" message, and that diagnostic is worth
    more than the few records it costs. Both pickers do their own VID check."""
    if ports is None:
        from serial.tools import list_ports
        ports = list_ports.comports()
    if products is None:
        products = _pnp_products()
    out = []
    for p in ports:
        serial = getattr(p, "serial_number", None) or ""
        out.append(CpuPort(p.device, p.vid, serial,
                           products.get(serial) or _port_product(p),
                           getattr(p, "pid", None)))
    return out


# What fwog_usb_product() in bsp/CMakeLists.txt puts in USBD_PRODUCT. The
# trailing space is load-bearing: without it "FWOG display" would also prefix
# a hypothetical "FWOG displayfoo".
CPU_PRODUCT_PREFIX = {
    "display": "FWOG display ",
    "main": "FWOG main ",
}


class CpuPortError(RuntimeError):
    """A specific CPU could not be identified from the USB product strings.

    Raised for both "nothing matched" and "several matched". Neither is
    recoverable by guessing: a main-CPU image on the display drives GPIO 29
    against the PDM microphone's output, so the cost of picking wrong is not
    symmetric with the cost of asking."""


def _pick_cpu_port(ports, cpu):
    """The serial device for `cpu`, or raise.

    Matches a prefix of the USB product string. A string that matches neither
    prefix -- the SDK's stock "Pico", say -- identifies NOTHING; it is never
    treated as evidence for the other CPU. That is what makes an un-reflashed
    board fall back to the manual prompt instead of guessing.

    Two independent pieces of evidence, tried in order:

      1. The USB PID -- 093C:2055 for display, 093C:2054 for main -- which
         fwog_usb_ids() sets and which FreeWili's fwfinder matches on.
      2. The product-string prefix, for a board flashed before that landed.

    PID first because the string is a string: it survives a rename or a stale
    image, and the PID does not. When both are present and disagree, the PID
    wins. Neither is ever inferred from the absence of the other.

    Pure, so the whole decision is testable without a serial port."""
    try:
        prefix = CPU_PRODUCT_PREFIX[cpu].lower()
    except KeyError:
        raise ValueError(
            f"cpu must be one of {sorted(CPU_PRODUCT_PREFIX)}, got {cpu!r}")

    matches = [p for p in ports
               if p.vid == ICS_USB_VID and p.pid == CPU_PID[cpu]]
    if not matches:
        matches = [p for p in ports
                   if p.vid in FWOG_USB_VIDS
                   and (p.product or "").lower().startswith(prefix)]
    if not matches:
        seen = ", ".join(
            f"{p.device}({p.product or '?'} "
            f"{p.vid:04X}:{p.pid:04X})" if p.pid is not None else
            f"{p.device}({p.product or '?'} {p.vid:04X})"
            for p in ports if p.vid in FWOG_USB_VIDS) or "none"
        raise CpuPortError(
            f"no running RP2040 identified as the {cpu} CPU; FreeWili CDC "
            f"ports seen: {seen}. Expected {ICS_USB_VID:04X}:"
            f"{CPU_PID[cpu]:04X} or a product string starting "
            f"\"{CPU_PRODUCT_PREFIX[cpu]}\". An image built before "
            "fwog_usb_product() landed reports \"Pico\" and cannot be told "
            "apart.")
    if len(matches) > 1:
        names = ", ".join(p.device for p in matches)
        raise CpuPortError(
            f"several ports claim to be the {cpu} CPU ({names}); pass --port "
            "to choose one")
    return matches[0].device


def _pick_console_port(ports, product_substr=FWOG_BL_USB_PRODUCT):
    """Choose one serial port from pyserial's comports() listing.

    Matches on the Raspberry Pi vendor ID plus a case-insensitive substring of
    the USB product string. The default filter finds the display bootloader;
    pass "" to match any RP2040 CDC, which is how you attach to a running
    application.

    Refuses rather than guesses when more than one matches: both RP2040s on
    this board can present a CDC at the same time, and attaching to the wrong
    one is confusing at exactly the moment you are debugging.

    WINDOWS: pyserial itself does not surface the product string (see
    _port_product) -- but do_console() calls this with _cpu_ports() rather
    than raw pyserial ports, and _cpu_ports() fills the product in from
    SetupAPI via _pnp_products(). So the filter DOES distinguish the
    bootloader on Windows, in the normal call path. The vendor-ID fallback
    described below is for a caller that passes raw pyserial ports instead
    (or when _pnp_products() itself comes back empty, e.g. no powershell.exe)
    -- there, a filtered search that finds nothing falls back to matching on
    vendor ID alone, and still refuses if that leaves more than one
    candidate.

    That fallback is also reachable with product strings fully intact, e.g.
    from _cpu_ports(): neither RP2040 might be running an image whose product
    matches this filter (an old image, or the wrong one) even though both
    report perfectly good strings. The "multiple matches" error below tells
    the two situations apart, so it never blames "this platform" for a
    mismatch that has nothing to do with the platform.

    Split out from do_console() so the decision is testable without a serial
    port or pyserial installed."""
    # Both VIDs: a part-updated board can have one CPU on 093C and the other
    # still on 2E8A, and `fw console` must reach either.
    rp = [p for p in ports if p.vid in FWOG_USB_VIDS]
    want = (product_substr or "").lower()
    matches = [p for p in rp if want in _port_product(p).lower()]

    fell_back = False
    if not matches and want:
        # The filter matched nothing. Vendor ID is all we have left to go on
        # -- regardless of *why* the filter missed, which is sorted out below
        # only if this leaves more than one candidate to explain.
        matches = rp
        fell_back = True

    if not matches:
        seen = ", ".join(f"{p.device}({_port_product(p) or '?'})" for p in ports) or "none"
        raise ConsolePortError(
            f"no USB CDC port matching {product_substr!r}; ports seen: {seen}. "
            "The display bootloader's console only enumerates after 10 s of "
            "main-CPU silence -- if main is healthy it will never appear.")
    if len(matches) > 1:
        names = ", ".join(p.device for p in matches)
        if not fell_back:
            hint = ""
        elif any(_port_product(p) for p in matches):
            # Product strings ARE available here; the filter just didn't
            # match any of them. Saying "this platform" would send someone
            # chasing a Windows/PowerShell problem that does not exist.
            hint = (f" (these ports report product strings, but none contain "
                    f"{product_substr!r})")
        else:
            hint = (" (this platform does not expose USB product strings, so the "
                    "bootloader cannot be picked out automatically)")
        raise ConsolePortError(
            f"multiple matching ports ({names}); pass --port to choose one{hint}")
    if fell_back:
        # The filter missed, and there was only one RP2040 CDC to fall back
        # to -- so it is used, silently before this. Now there is enough
        # information to say so, even though the behaviour (attach anyway)
        # does not change.
        p = matches[0]
        seen_product = _port_product(p) or "<no product string>"
        print(f"note: {p.device} reports {seen_product!r}, which does not "
              f"contain {product_substr!r}; attaching anyway as the only "
              "RP2040 present.")
    return matches[0].device

def do_flash(app, do_print, timeout_s=120, touch=None, find_volume=None,
             ports=None, pause=None, uf2_override=None):
    """Copy an app's UF2 to a BOOTSEL volume, putting the right CPU there first.

    The CPU comes from app_cpu(), i.e. the app-name suffix, which this command
    has always trusted -- new_app() refuses a name that contradicts its
    template precisely to protect that inference.

    Three paths, and the last two are what this command did before the touch
    existed, unchanged:
      - a volume is already mounted -> use it, and say so loudly, because
        which CPU it belongs to cannot be determined from the mount. This is
        the only way to flash a fresh board (nothing has run fwog_usb_product()
        yet) or a CPU that was put into BOOTSEL by hand, so it stays even
        though the other two paths make it unnecessary in the common case.
        It is also the ONE remaining path by which a main image can reach the
        display CPU -- identification never runs here -- so the NOTE below is
        followed by a real pause, not just a printed suggestion to Ctrl-C.
      - the CPU is identified       -> touch it, then wait
      - it is not                   -> ask the operator, then wait

    touch/find_volume/ports/pause are injectable so the whole decision tree is
    testable without a board (mirrors do_bootsel).

    find_volume() can raise MultipleRp2VolumesError from either the pre-touch
    check or the post-touch wait; both mean the same thing (fact 15 -- two
    RP2040s in BOOTSEL cannot be told apart) and both must refuse before
    anything is touched or copied, so one try/except covers both call sites
    rather than duplicating the same handler twice.
    """
    touch = touch or _touch_port
    find_volume = find_volume or find_rpi_rp2
    pause = pause or _pause_before_flash
    uf2 = uf2_path(app, uf2_override)
    cpu = app_cpu(app)
    if do_print:
        print(f"touch <{cpu} CPU> at 1200 baud to reboot it into BOOTSEL, "
              f"then copy {uf2} -> <RPI-RP2 volume>   # {cpu} CPU")
        return 0
    if not uf2.exists():
        hint = ("check the path" if uf2_override
                else f"run `fw build {app}` first")
        print(f"{uf2} not found — {hint}", file=sys.stderr)
        return 1

    try:
        vol = find_volume()
        already_mounted = vol is not None
        if not already_mounted:
            target = None
            try:
                target = _pick_cpu_port(
                    _cpu_ports() if ports is None else ports, cpu)
            except CpuPortError as e:
                print(f"note: {e}")
            if target:
                print(f"touching {target} at 1200 baud to reboot the "
                      f"{cpu.upper()} CPU into BOOTSEL ...")
                touch(target)
            else:
                print(f"Put the {cpu.upper()} CPU into BOOTSEL, "
                      "waiting for RPI-RP2 ...")
            vol = _wait_for_volume(find_volume, timeout_s)
    except MultipleRp2VolumesError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if already_mounted:
        print(f"NOTE: an RPI-RP2 volume is already mounted at {vol}.")
        print("      Which CPU that is cannot be determined from the mount, "
              f"so this copies {uf2.name} to it as-is.")
        print(f"      Ctrl-C now if that is not the {cpu.upper()} CPU.")
        # A real pause, not just a printed suggestion: this is the one
        # remaining path by which a main image can reach the display CPU,
        # because identification never runs here. Injectable so the tests
        # that exercise this path stay instant.
        pause()
    elif not vol:
        print("no RPI-RP2 volume appeared", file=sys.stderr)
        return 1

    shutil.copy(uf2, vol / uf2.name)
    print(f"copied {uf2.name} to {vol}")
    return 0


def _pause_before_flash(seconds=3.0):
    """The real pause behind do_flash's "Ctrl-C now" NOTE, for the
    already-mounted path where identification never ran. A printed
    suggestion with nothing enforcing it is not a guard -- this is. Its own
    function (rather than a bare time.sleep(3) inline) so do_flash's `pause`
    parameter can inject a no-op and the tests that exercise this path stay
    instant."""
    time.sleep(seconds)


def _touch_port(port):
    """Reboot the CPU behind `port` into the UF2 bootloader (the hardware record).

    Opening a pico_stdio_usb CDC at 1200 baud is the whole mechanism:
    pico_usb_reset's tud_cdc_line_coding_cb() sees
    PICO_USB_RESET_MAGIC_BAUD_RATE and calls rom_reset_usb_boot_extra(). No
    DTR handshake is involved -- setting the line coding is enough, which is
    why merely opening triggers it. It is on by default in every binary here.

    The open then FAILS on Windows with "A device attached to the system is
    not functioning", because the device disappears mid-open. That error is
    the success indicator, not a fault, and it is indistinguishable from a
    genuine failure at this level -- so this swallows it and the CALLER
    decides success by waiting for a volume."""
    import serial
    try:
        serial.Serial(port, 1200).close()
    except (OSError, serial.SerialException):
        pass


def _wait_for_volume(find_volume, timeout_s):
    """Poll for a BOOTSEL volume until it appears or the deadline passes.

    Returns the volume path, or None on timeout. Propagates
    MultipleRp2VolumesError for the caller to report -- both callers print it
    the same way but return different things.

    Checks BEFORE testing the deadline, so timeout_s=0 still performs exactly
    one check -- that is what keeps a "no volume ever appears" test fast
    instead of a real-time wait. Shared by do_bootsel and do_flash,
    which must not each carry their own copy of this loop."""
    deadline = time.time() + timeout_s
    while True:
        vol = find_volume()
        if vol:
            return vol
        if time.time() >= deadline:
            return None
        time.sleep(0.5)


def do_bootsel(cpu=None, port=None, do_print=False, timeout_s=30,
               touch=None, find_volume=None, ports=None):
    """Reboot one specific CPU into BOOTSEL.

    This is what makes fact 15's one-CPU-at-a-time rule hold by construction
    rather than by the operator remembering it: identify first, touch exactly
    one port, and exactly one RPI-RP2 volume can appear.

    touch/find_volume/ports are injectable so the whole decision tree is
    testable without a board."""
    touch = touch or _touch_port
    find_volume = find_volume or find_rpi_rp2

    if do_print:
        print(f"touch {port or f'<{cpu} CPU>'} at 1200 baud   # -> BOOTSEL")
        return 0

    # Refuse if something is already in BOOTSEL. Touching would add a second
    # volume, and once mounted the two are indistinguishable (fact 15).
    try:
        mounted = find_volume()
    except MultipleRp2VolumesError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if mounted:
        print(f"error: an RPI-RP2 volume is already mounted at {mounted}. "
              "Touching another CPU now would leave two mounted, which cannot "
              "be told apart. Flash or unplug that one first.", file=sys.stderr)
        return 1

    if port is None:
        try:
            port = _pick_cpu_port(_cpu_ports() if ports is None else ports, cpu)
        except CpuPortError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

    if cpu is not None:
        print(f"touching {port} at 1200 baud to reboot the {cpu.upper()} "
              "CPU into BOOTSEL ...")
    else:
        # --port bypasses identification entirely, so the CPU name is the
        # one thing this message cannot claim to know -- and it is the
        # operator's only cross-check on that path.
        print(f"touching {port} at 1200 baud to reboot into BOOTSEL "
              "(CPU identity unknown -- --port was used) ...")
    touch(port)

    try:
        vol = _wait_for_volume(find_volume, timeout_s)
    except MultipleRp2VolumesError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if vol:
        print(f"{port} is now in BOOTSEL at {vol}")
        return 0
    print(f"no RPI-RP2 volume appeared within {timeout_s}s after touching "
          f"{port}. The CPU may not have been running code with USB up -- "
          "fact 30 needs a live CDC, and it cannot rescue a hung image.",
          file=sys.stderr)
    return 1


def do_bootloader(do_print, timeout_s=120):
    """Build and flash the display serial bootloader. Once per board."""
    _run([configure_command(), build_command(BOOTLOADER_APP)], do_print)
    return do_flash(BOOTLOADER_APP, do_print, timeout_s=timeout_s)


def do_console(port=None, product=FWOG_BL_USB_PRODUCT, do_print=False):
    if do_print:
        print(f"attach to {port or '<auto>'}   # product filter {product!r}")
        return 0
    try:
        import serial
    except ImportError:
        print("fw console needs pyserial:  python -m pip install pyserial",
              file=sys.stderr)
        return 1

    if port is None:
        try:
            port = _pick_console_port(_cpu_ports(), product)
        except ConsolePortError as e:
            print(f"error: {e}", file=sys.stderr)
            return 1

    import threading
    # The baud rate is ignored by USB CDC; pyserial requires a number.
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        print(f"connected to {port} — type `help`, Ctrl-C to exit")
        stop = threading.Event()

        def reader():
            while not stop.is_set():
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        try:
            for line in sys.stdin:
                ser.write(line.rstrip("\r\n").encode() + b"\n")
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            t.join(timeout=1.0)
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog="fw")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build"); b.add_argument("app", nargs="?", default=DEFAULT_APP)
    b.add_argument("--print", action="store_true", dest="do_print")
    b.add_argument("--baud", type=int, default=None,
                   help="inter-CPU link rate; rebuilds both binaries")

    f = sub.add_parser("flash"); f.add_argument("app")
    # For projects that consume this BSP as a submodule: their UF2 is in their
    # build tree, not this one. See uf2_path().
    f.add_argument("--uf2", default=None,
                   help="path to the .uf2 to copy (default: this repo's "
                        "build/apps/<app>/<app>.uf2)")
    f.add_argument("--print", action="store_true", dest="do_print")

    bl = sub.add_parser("bootloader")
    bl.add_argument("--print", action="store_true", dest="do_print")

    bs = sub.add_parser("bootsel")
    g = bs.add_mutually_exclusive_group(required=True)
    g.add_argument("--cpu", choices=["display", "main"],
                   help="identify this CPU by its USB product string")
    g.add_argument("--port", help="touch this port without identifying it")
    bs.add_argument("--print", action="store_true", dest="do_print")

    c = sub.add_parser("console")
    c.add_argument("--port", default=None)
    c.add_argument("--product", default=FWOG_BL_USB_PRODUCT,
                   help='USB product substring; "" matches any RP2040 CDC')
    c.add_argument("--print", action="store_true", dest="do_print")

    t = sub.add_parser("test")
    t.add_argument("--print", action="store_true", dest="do_print")

    n = sub.add_parser("new-app"); n.add_argument("name")
    n.add_argument("--cpu", required=True, choices=["display", "main"])

    a = p.parse_args(argv)
    if a.cmd == "build":
        # Configure first: on a fresh checkout there is no build/ tree, and
        # `cmake --build --preset` fails with an unhelpful "not a directory".
        # Re-configuring an already-configured tree is cheap and idempotent.
        _run([configure_command(a.baud), build_command(a.app)], a.do_print); return 0
    if a.cmd == "flash":
        return do_flash(a.app, a.do_print, uf2_override=a.uf2)
    if a.cmd == "bootloader":
        return do_bootloader(a.do_print)
    if a.cmd == "bootsel":
        return do_bootsel(a.cpu, a.port, a.do_print)
    if a.cmd == "console":
        return do_console(a.port, a.product, a.do_print)
    if a.cmd == "test":
        _run(test_command(), a.do_print); return 0
    if a.cmd == "new-app":
        d = new_app(a.name, a.cpu)
        print(f"created {d}\nAdd `add_subdirectory(apps/{a.name})` to CMakeLists.txt")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
