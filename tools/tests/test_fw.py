import contextlib
import io
import pathlib
import sys
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import fw  # noqa: E402


class TestFwCommands(unittest.TestCase):
    def test_configure_then_build(self):
        # `fw build` must work on a fresh checkout with no build/ tree, so it
        # configures before building. `cmake --build --preset` alone fails
        # with "not a directory" when nothing has been configured yet.
        self.assertEqual(fw.configure_command(), ["cmake", "--preset", "target"])
        self.assertEqual(
            fw.build_command("hello_main"),
            ["cmake", "--build", "--preset", "target", "--target", "hello_main"],
        )

    def test_uf2_path(self):
        """apps/ is one folder per _display/_main PAIR with the suffix
        dropped (AGENTS.md), and the build tree mirrors apps/ -- so the
        folder is "hello", not "hello_main"."""
        self.assertEqual(
            fw.uf2_path("hello_main"),
            fw.REPO_ROOT / "build" / "apps" / "hello" / "hello_main.uf2",
        )
        self.assertEqual(
            fw.uf2_path("hello_display"),
            fw.REPO_ROOT / "build" / "apps" / "hello" / "hello_display.uf2",
        )

    def test_uf2_path_override_wins_and_ignores_repo_root(self):
        """--uf2 is what lets a project consuming this BSP as a submodule use
        `fw flash`: its build tree is not under REPO_ROOT, which is derived
        from fw.py's own location and cannot be guessed."""
        got = fw.uf2_path("hello_main", "/elsewhere/out/hello_main.uf2")
        self.assertEqual(got, pathlib.Path("/elsewhere/out/hello_main.uf2"))
        self.assertNotIn(str(fw.REPO_ROOT), str(got))

    def test_uf2_path_override_none_falls_back_to_the_default(self):
        self.assertEqual(fw.uf2_path("hello_main", None),
                         fw.uf2_path("hello_main"))

    def test_app_cpu_from_suffix(self):
        self.assertEqual(fw.app_cpu("template_display"), "display")
        self.assertEqual(fw.app_cpu("hello_display"), "display")
        self.assertEqual(fw.app_cpu("template_main"), "main")
        self.assertEqual(fw.app_cpu("hello_main"), "main")

    def test_app_cpu_unknown_raises(self):
        with self.assertRaises(ValueError):
            fw.app_cpu("mystery_app")

    def test_test_command_is_four_phases(self):
        # CTest (configure+build+run) plus a fourth phase running the
        # tools/tests/ unit tests, so `fw test` covers both trees. Before
        # this, nothing in the repo ever ran tools/tests/test_fw.py.
        cmds = fw.test_command()
        self.assertEqual(len(cmds), 4)
        self.assertEqual(cmds[0][0], "cmake")   # configure
        self.assertEqual(cmds[1][0], "cmake")   # build
        self.assertEqual(cmds[2][0], "ctest")   # run
        self.assertEqual(cmds[3][0], sys.executable)
        self.assertIn("unittest", cmds[3])
        self.assertIn("discover", cmds[3])

    def test_new_app_rejects_bad_cpu(self):
        with self.assertRaises(ValueError):
            fw.new_app("whatever", "coprocessor")

    def test_new_app_rejects_name_cpu_mismatch(self):
        # `fw flash` decides which processor to tell you to put into BOOTSEL
        # purely from the app-name suffix. An app named _main but built from
        # the display template would send you to reflash the wrong half of
        # the board, so the mismatch is refused at creation time.
        with self.assertRaises(ValueError):
            fw.new_app("foo_main", "display")
        with self.assertRaises(ValueError):
            fw.new_app("foo_display", "main")

    @staticmethod
    def _fake_template(root):
        """apps/template as it is laid out on disk: one folder, one
        CMakeLists.txt declaring both halves, sources under display/ and
        main/."""
        tpl = root / "apps" / "template"
        (tpl / "display").mkdir(parents=True)
        (tpl / "main").mkdir(parents=True)
        (tpl / "CMakeLists.txt").write_text(
            "add_executable(template_display display/main.c)\n"
            "target_link_libraries(template_display fwog_display_bsp)\n"
            "\n"
            "add_executable(template_main main/main.c)\n"
            "target_link_libraries(template_main fwog_main_bsp)\n"
            "fwog_embed_display_image(template_main)\n")
        (tpl / "display" / "main.c").write_text("int main(void){return 0;}\n")
        (tpl / "main" / "main.c").write_text("int main(void){return 0;}\n")
        return tpl

    def test_new_app_scaffolds_the_display_half_only(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._fake_template(root)

            dest = fw.new_app("blinky_display", "display", repo_root=root)

            # The FOLDER drops the suffix; the TARGET keeps it.
            self.assertEqual(dest.name, "blinky")
            self.assertTrue((dest / "display" / "main.c").exists())
            self.assertFalse((dest / "main").exists())
            text = (dest / "CMakeLists.txt").read_text()
            self.assertIn("blinky_display", text)
            self.assertNotIn("template", text)
            self.assertNotIn("add_executable(blinky_main", text)

    def test_new_app_main_half_does_not_embed_a_missing_display(self):
        """A main-only app has no <folder>_display for the default pair
        lookup, so an active fwog_embed_display_image() call would fail at
        configure time. It must be left commented, with instructions."""
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self._fake_template(root)

            dest = fw.new_app("blinky_main", "main", repo_root=root)

            self.assertTrue((dest / "main" / "main.c").exists())
            self.assertFalse((dest / "display").exists())
            text = (dest / "CMakeLists.txt").read_text()
            self.assertIn("add_executable(blinky_main", text)
            self.assertNotIn("add_executable(blinky_display", text)
            for line in text.splitlines():
                if "fwog_embed_display_image(blinky_main)" in line:
                    self.assertTrue(line.lstrip().startswith("#"), line)

    def test_scan_rpi_rp2_no_volume(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            empty = root / "empty"
            empty.mkdir()
            self.assertIsNone(fw._scan_rpi_rp2([empty]))

    def test_scan_rpi_rp2_single_volume(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            vol = root / "RPI-RP2"
            vol.mkdir()
            (vol / "INFO_UF2.TXT").write_text("UF2 Bootloader\n")
            other = root / "not-a-bootsel-volume"
            other.mkdir()
            self.assertEqual(fw._scan_rpi_rp2([other, vol]), vol)

    def test_scan_rpi_rp2_refuses_multiple_volumes(self):
        # Both RP2040s on this board present indistinguishable RPI-RP2
        # volumes, and both being in BOOTSEL at once is a realistic bring-up
        # state. `fw flash` must refuse rather than guess which one to
        # flash -- guessing wrong flashes the wrong half of the board.
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            vol1 = root / "vol1"
            vol1.mkdir()
            (vol1 / "INFO_UF2.TXT").write_text("UF2 Bootloader\n")
            vol2 = root / "vol2"
            vol2.mkdir()
            (vol2 / "INFO_UF2.TXT").write_text("UF2 Bootloader\n")
            with self.assertRaises(fw.MultipleRp2VolumesError):
                fw._scan_rpi_rp2([vol1, vol2])

    def test_new_app_refuses_to_overwrite(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tpl = root / "apps" / "template"
            (tpl / "main").mkdir(parents=True)
            (tpl / "CMakeLists.txt").write_text("x")
            # The folder is the target name WITHOUT its suffix, so this is
            # what a clash looks like now.
            (root / "apps" / "taken").mkdir(parents=True)
            with self.assertRaises(FileExistsError):
                fw.new_app("taken_main", "main", repo_root=root)


import collections

FakePort = collections.namedtuple("FakePort", "device vid pid product")

FakeSerialPort = collections.namedtuple(
    "FakeSerialPort", "device vid serial_number product description")


class TestBootloaderAndConsole(unittest.TestCase):
    def setUp(self):
        # The single-candidate fallback in _pick_console_port now prints a
        # note when it attaches to an unmatched port, and several tests here
        # exercise exactly that path with fake ports (COM7, COM60...). Silence
        # it so `fw test`'s own output stays dot-clean; no test in this class
        # asserts against stdout.
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.addCleanup(stack.close)

    def test_configure_command_unchanged_without_baud(self):
        # The existing no-argument behavior must not shift: --baud is an
        # addition, and every other caller passes nothing.
        self.assertEqual(fw.configure_command(), ["cmake", "--preset", "target"])

    def test_configure_command_with_baud(self):
        # The link rate is compile-time and shared by BOTH binaries, so it
        # is a cache variable, not a runtime flag. Note this is NOT
        # -DPICO_BOARD -- that one must never appear on a command line.
        self.assertEqual(
            fw.configure_command(8333333),
            ["cmake", "--preset", "target", "-DFWOG_LINK_BAUD=8333333"],
        )

    def test_bootloader_app_targets_the_display_cpu(self):
        # `fw flash` derives the CPU from the name suffix alone, so the
        # bootloader app must be named honestly or it would send the
        # operator to BOOTSEL the wrong processor.
        self.assertEqual(fw.app_cpu(fw.BOOTLOADER_APP), "display")

    def _ports(self):
        return [
            FakePort("COM3", 0x1234, 0x0001, "Some Serial Adapter"),
            FakePort("COM4", fw.RPI_USB_VID, 0x000A, "FWOG display bl"),
            FakePort("COM5", fw.RPI_USB_VID, 0x000A, "Pico"),
        ]

    def test_pick_console_port_matches_the_bootloader(self):
        self.assertEqual(fw._pick_console_port(self._ports()), "COM4")

    def test_pick_console_port_ignores_non_raspberry_vids(self):
        ports = [FakePort("COM3", 0x1234, 1, "FWOG display bl")]
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port(ports)

    def test_pick_console_port_empty_filter_matches_any_pico(self):
        # An application CDC has the stock "Pico" product string, so an
        # empty filter is how you attach to a running app rather than the
        # bootloader. Two candidates then, so it must refuse.
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port(self._ports(), "")
        one = [FakePort("COM5", fw.RPI_USB_VID, 0x000A, "Pico")]
        self.assertEqual(fw._pick_console_port(one, ""), "COM5")

    def test_pick_console_port_refuses_when_ambiguous(self):
        # Both RP2040s can present a CDC at once. Guessing would attach to
        # the wrong half of the board.
        ports = [
            FakePort("COM4", fw.RPI_USB_VID, 0x000A, "FWOG display bl"),
            FakePort("COM6", fw.RPI_USB_VID, 0x000A, "FWOG display bl"),
        ]
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port(ports)

    def test_pick_console_port_reports_nothing_found(self):
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port([])

    def test_pick_console_port_tolerates_a_missing_product_string(self):
        # pyserial reports product=None on Windows for every RP2040 CDC
        # (usbser.sys does not surface it). This test originally asserted
        # that such a port must NOT match the bootloader filter -- which is
        # correct in the abstract and useless in practice: it made
        # `fw console` unable to find the bootloader on this project's own
        # platform, confirmed against real hardware.
        #
        # The contract now: a filtered search that finds nothing falls back
        # to vendor ID, so a lone RP2040 CDC is used. Refusing to guess
        # between SEVERAL is preserved and covered in
        # TestConsolePortWindowsFallback.
        ports = [FakePort("COM7", fw.RPI_USB_VID, 0x000A, None)]
        self.assertEqual(fw._pick_console_port(ports), "COM7")
        self.assertEqual(fw._pick_console_port(ports, ""), "COM7")

    def test_pick_console_port_is_case_insensitive(self):
        ports = [FakePort("COM4", fw.RPI_USB_VID, 0x000A, "fwog DISPLAY bl")]
        self.assertEqual(fw._pick_console_port(ports), "COM4")

class TestConsolePortWindowsFallback(unittest.TestCase):
    """Windows' usbser.sys does not expose USB product strings to pyserial:
    product is None and description is "USB Serial Device (COMxx)". The
    product filter is therefore unusable there, which real hardware showed --
    `fw console` could never auto-find the bootloader on this project's own
    platform."""

    def setUp(self):
        # Same rationale as TestBootloaderAndConsole.setUp: several tests
        # here hit the single-candidate fallback's new print, with fake
        # ports. Silence it; no test in this class asserts against stdout.
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.addCleanup(stack.close)

    def test_falls_back_to_vendor_id_when_no_product_strings(self):
        ports = [FakePort("COM60", 0x2E8A, 0x000A, None)]
        self.assertEqual(fw._pick_console_port(ports), "COM60")

    def test_fallback_still_refuses_to_guess_between_two(self):
        # Both ports report no product string at all -- the platform truly
        # cannot tell them apart, so the hint should blame the platform.
        ports = [FakePort("COM60", 0x2E8A, 0x000A, None),
                 FakePort("COM65", 0x2E8A, 0x000A, None)]
        with self.assertRaises(fw.ConsolePortError) as cm:
            fw._pick_console_port(ports)
        self.assertIn("--port", str(cm.exception))
        # The message must say WHY it cannot tell them apart.
        self.assertIn("product strings", str(cm.exception))
        self.assertIn("this platform", str(cm.exception))

    def test_fallback_message_does_not_blame_the_platform_when_products_are_present(self):
        # _cpu_ports() carries real product strings even on Windows. If
        # neither CPU happens to be running the bootloader -- e.g. both are
        # running application images -- the filter still falls back to
        # vendor ID and still refuses between two candidates, but the reason
        # is "nothing matched", not "this platform hides product strings":
        # both ports here plainly report one.
        ports = [FakePort("COM60", 0x2E8A, 0x000A, "FWOG lcd test"),
                 FakePort("COM65", 0x2E8A, 0x000A, "Pico")]
        with self.assertRaises(fw.ConsolePortError) as cm:
            fw._pick_console_port(ports)
        msg = str(cm.exception)
        self.assertIn("--port", msg)
        self.assertIn("product strings", msg)
        self.assertNotIn("this platform", msg)

    def test_product_filter_still_wins_where_it_is_available(self):
        # Linux/macOS populate product, so the bootloader is picked out even
        # with an application CDC present.
        ports = [FakePort("COM65", 0x2E8A, 0x000A, "Pico"),
                 FakePort("COM60", 0x2E8A, 0x000A, "FWOG display bl")]
        self.assertEqual(fw._pick_console_port(ports), "COM60")

    def test_non_rp2040_ports_never_match(self):
        ports = [FakePort("COM16", 0x0403, 0x6014, "FTDI")]
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port(ports)


class TestPnpProductParsing(unittest.TestCase):
    """The Windows product-string lister emits `serial|product` per line.

    Joining on the serial number rather than a COM number is deliberate:
    pyserial reports serial_number on Windows but not product, so the serial
    is the one field both sides already agree on. Parsing COM numbers out of
    friendly-name strings was the alternative and is far more fragile."""

    def test_parses_a_single_line(self):
        got = fw._parse_pnp_products("E463A8574B114535|Pico\n")
        self.assertEqual(got, {"E463A8574B114535": "Pico"})

    def test_product_may_contain_spaces(self):
        got = fw._parse_pnp_products("E463A8574B434635|FWOG display lcd\n")
        self.assertEqual(got, {"E463A8574B434635": "FWOG display lcd"})

    def test_parses_several_lines(self):
        got = fw._parse_pnp_products(
            "E463A8574B114535|FWOG main template\n"
            "E463A8574B434635|FWOG display lcd\n")
        self.assertEqual(len(got), 2)
        self.assertEqual(got["E463A8574B114535"], "FWOG main template")

    def test_tolerates_crlf(self):
        # PowerShell emits CRLF. So does everything else in this repo.
        got = fw._parse_pnp_products("E463A8574B114535|Pico\r\n")
        self.assertEqual(got, {"E463A8574B114535": "Pico"})

    def test_empty_product_maps_to_empty_string(self):
        # BusReportedDeviceDesc can be absent; -ErrorAction SilentlyContinue
        # then yields an empty field rather than dropping the line.
        got = fw._parse_pnp_products("E463A8574B114535|\n")
        self.assertEqual(got, {"E463A8574B114535": ""})

    def test_ignores_blank_and_malformed_lines(self):
        got = fw._parse_pnp_products(
            "\n"
            "no-pipe-here\n"
            "   \n"
            "E463A8574B114535|Pico\n")
        self.assertEqual(got, {"E463A8574B114535": "Pico"})

    def test_empty_input_is_an_empty_map(self):
        self.assertEqual(fw._parse_pnp_products(""), {})


class TestCpuPorts(unittest.TestCase):
    def test_enriches_pyserial_ports_with_product_strings(self):
        # The Windows case: pyserial has the serial but no product; the
        # PowerShell map supplies the product.
        ports = [FakeSerialPort("COM65", fw.RPI_USB_VID, "E463A8574B114535",
                                None, "USB Serial Device (COM65)")]
        got = fw._cpu_ports(ports=ports,
                            products={"E463A8574B114535": "FWOG main template"})
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0].device, "COM65")
        self.assertEqual(got[0].vid, fw.RPI_USB_VID)
        self.assertEqual(got[0].serial, "E463A8574B114535")
        self.assertEqual(got[0].product, "FWOG main template")

    def test_falls_back_to_pyserial_product_when_map_is_empty(self):
        # The Linux/macOS case, and the Windows case when powershell fails.
        ports = [FakeSerialPort("/dev/ttyACM0", fw.RPI_USB_VID, "ABC",
                                "FWOG display lcd", "ttyACM0")]
        got = fw._cpu_ports(ports=ports, products={})
        self.assertEqual(got[0].product, "FWOG display lcd")

    def test_keeps_non_rp2040_ports_so_diagnostics_stay_useful(self):
        # _pick_console_port lists every port it saw when it finds nothing.
        # Dropping foreign ports here would gut that message.
        ports = [FakeSerialPort("COM16", 0x0403, "6", None, "USB Serial Port")]
        got = fw._cpu_ports(ports=ports, products={})
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0].vid, 0x0403)

    def test_tolerates_a_port_with_no_serial_number(self):
        ports = [FakeSerialPort("COM9", fw.RPI_USB_VID, None, None, "d")]
        got = fw._cpu_ports(ports=ports, products={})
        self.assertEqual(got[0].serial, "")


class TestPickCpuPort(unittest.TestCase):
    """Which COM port is the display, and which is main.

    Getting this wrong is not symmetric: a main-CPU image on the display
    drives GPIO 29 against the PDM microphone's output. So the rule is
    positive identification or nothing -- an unrecognised product string is
    never assumed to be either CPU."""

    def _both(self):
        return [
            fw.CpuPort("COM65", fw.RPI_USB_VID, "E463A8574B114535",
                       "FWOG main template"),
            fw.CpuPort("COM60", fw.RPI_USB_VID, "E463A8574B434635",
                       "FWOG display lcd"),
        ]

    def test_finds_the_display(self):
        self.assertEqual(fw._pick_cpu_port(self._both(), "display"), "COM60")

    def test_finds_main(self):
        self.assertEqual(fw._pick_cpu_port(self._both(), "main"), "COM65")

    def test_stock_pico_string_identifies_nothing(self):
        # THE important one. Before this scheme, main and any display app that
        # forgot USBD_PRODUCT both said "Pico". Inferring "Pico means main"
        # would misidentify a display running template_display as main.
        ports = [fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "Pico")]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "main")
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_no_match_raises(self):
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port([], "display")

    # ---- Identification by PID (093C:2054 / 093C:2055) ----
    #
    # The product string is a string: it can be renamed, and an older image
    # may carry a stale one. The PID is set by the BSP in the same call every
    # app makes, and is what FreeWili's own finder matches on. So it is tried
    # FIRST, with the prefix kept as the fallback for boards flashed before
    # this landed.

    def _both_pid(self):
        return [
            fw.CpuPort("COM65", fw.ICS_USB_VID, "E463A8574B114535",
                       "FWOG main template 001", fw.CPU_PID["main"]),
            fw.CpuPort("COM60", fw.ICS_USB_VID, "E463A8574B434635",
                       "FWOG display lcd 001", fw.CPU_PID["display"]),
        ]

    def test_identifies_by_pid(self):
        self.assertEqual(fw._pick_cpu_port(self._both_pid(), "display"),
                         "COM60")
        self.assertEqual(fw._pick_cpu_port(self._both_pid(), "main"), "COM65")

    def test_pid_beats_a_contradicting_product_string(self):
        """The PID is set by the BSP; the string can be stale after a rename.

        If they disagree the PID wins, because it is the half a human cannot
        typo."""
        ports = [fw.CpuPort("COM60", fw.ICS_USB_VID, "S1",
                            "FWOG main oops 001", fw.CPU_PID["display"])]
        self.assertEqual(fw._pick_cpu_port(ports, "display"), "COM60")

    def test_falls_back_to_the_prefix_for_an_older_image(self):
        """A board flashed before the VID/PID change still enumerates
        2E8A:000A. It must resolve, not drop to the manual prompt."""
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1",
                            "FWOG display lcd 001", 0x000A)]
        self.assertEqual(fw._pick_cpu_port(ports, "display"), "COM60")

    def test_stock_pico_on_the_new_vid_still_identifies_nothing(self):
        """Positive identification or nothing, on either VID."""
        ports = [fw.CpuPort("COM65", fw.ICS_USB_VID, "S1", "Pico", 0x9999)]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "main")
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_two_ports_with_the_same_pid_refuse_rather_than_guess(self):
        ports = [
            fw.CpuPort("COM60", fw.ICS_USB_VID, "S1", "FWOG display lcd 001",
                       fw.CPU_PID["display"]),
            fw.CpuPort("COM61", fw.ICS_USB_VID, "S2", "FWOG display bench 001",
                       fw.CPU_PID["display"]),
        ]
        with self.assertRaises(fw.CpuPortError) as cm:
            fw._pick_cpu_port(ports, "display")
        self.assertIn("--port", str(cm.exception))

    def test_a_mixed_fleet_resolves_each_cpu_once(self):
        """One CPU already updated, the other not. Both must still resolve."""
        ports = [
            fw.CpuPort("COM60", fw.ICS_USB_VID, "S1", "FWOG display lcd 001",
                       fw.CPU_PID["display"]),
            fw.CpuPort("COM65", fw.RPI_USB_VID, "S2", "FWOG main template",
                       0x000A),
        ]
        self.assertEqual(fw._pick_cpu_port(ports, "display"), "COM60")
        self.assertEqual(fw._pick_cpu_port(ports, "main"), "COM65")

    def test_pid_of_the_other_cpu_is_never_evidence(self):
        """A main PID must not satisfy a request for the display."""
        ports = [fw.CpuPort("COM65", fw.ICS_USB_VID, "S1",
                            "FWOG main template 001", fw.CPU_PID["main"])]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_two_matches_refuse_rather_than_guess(self):
        ports = [
            fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "FWOG display lcd"),
            fw.CpuPort("COM61", fw.RPI_USB_VID, "S2", "FWOG display template"),
        ]
        with self.assertRaises(fw.CpuPortError) as cm:
            fw._pick_cpu_port(ports, "display")
        self.assertIn("--port", str(cm.exception))

    def test_foreign_vid_never_matches(self):
        # An FTDI adapter whose description happens to contain the prefix must
        # not be mistaken for a CPU on this board.
        ports = [fw.CpuPort("COM16", 0x0403, "6", "FWOG display lcd")]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_prefix_requires_its_trailing_space(self):
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "FWOG displayfoo")]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_display_prefix_does_not_match_main(self):
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "FWOG display lcd")]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "main")

    def test_matches_case_insensitively(self):
        # Neither the prefix table nor a live product string is guaranteed
        # a particular case. Deleting only the port's .lower() call makes
        # this fail; deleting only the prefix's .lower() call (leaving
        # CPU_PRODUCT_PREFIX's own lowercase "display"/"main" as the only
        # correct case) would not -- both calls have to stay for a caller
        # that uppercases either side.
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "fwog DISPLAY lcd")]
        self.assertEqual(fw._pick_cpu_port(ports, "display"), "COM60")

    def test_empty_product_never_matches(self):
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "")]
        with self.assertRaises(fw.CpuPortError):
            fw._pick_cpu_port(ports, "display")

    def test_bootloader_is_identified_as_the_display(self):
        # bl_display links through fwog_link_bootloader, which passes
        # `display`, so the bootloader identifies as the display CPU too.
        ports = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", "FWOG display bl")]
        self.assertEqual(fw._pick_cpu_port(ports, "display"), "COM60")

    def test_unknown_cpu_name_raises_value_error(self):
        with self.assertRaises(ValueError):
            fw._pick_cpu_port(self._both(), "coprocessor")


class TestConsoleUsesEnrichedPorts(unittest.TestCase):
    """_pick_console_port could never find the bootloader on Windows: with no
    product strings it fell back to matching on vendor ID alone and then
    refused whenever both CPUs were plugged in. Feeding it CpuPort records
    with real product strings retires that."""

    def setUp(self):
        # Same rationale as TestBootloaderAndConsole.setUp: the single-
        # candidate fallback's new print fires here too, with fake ports.
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.addCleanup(stack.close)

    def test_bootloader_is_picked_out_from_two_rp2040s(self):
        ports = [
            fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "FWOG main template"),
            fw.CpuPort("COM60", fw.RPI_USB_VID, "S2", "FWOG display bl"),
        ]
        self.assertEqual(fw._pick_console_port(ports), "COM60")

    def test_bootloader_constant_matches_what_cmake_generates(self):
        # fwog_usb_product(bl_display display) yields "FWOG display bl".
        self.assertEqual(fw.FWOG_BL_USB_PRODUCT, "FWOG display bl")

    def test_vendor_id_fallback_survives_for_powershell_less_hosts(self):
        # When _pnp_products() returns {} the products are still None, and the
        # old behaviour must remain: one RP2040 is used, two are refused.
        one = [fw.CpuPort("COM60", fw.RPI_USB_VID, "S1", None)]
        self.assertEqual(fw._pick_console_port(one), "COM60")
        two = one + [fw.CpuPort("COM65", fw.RPI_USB_VID, "S2", None)]
        with self.assertRaises(fw.ConsolePortError):
            fw._pick_console_port(two)


class TestBootsel(unittest.TestCase):
    """fw bootsel opens a running CPU's CDC at 1200 baud, which reboots it
    into the UF2 bootloader (the hardware record). On Windows the open itself then
    FAILS, because the device vanishes mid-open -- that error is the success
    indicator. So success cannot be read from the touch; it is read from a
    volume appearing."""

    def setUp(self):
        # do_bootsel's print statements are the real ones, and on real
        # hardware they name real COM ports -- so even though every test
        # here injects a fake touch/find_volume, capture stdout/stderr
        # rather than letting them hit the real streams. This also gives
        # every test in the class a buffer (self.out/self.err) to assert
        # against.
        self.out = io.StringIO()
        self.err = io.StringIO()
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(self.out))
        stack.enter_context(contextlib.redirect_stderr(self.err))
        self.addCleanup(stack.close)

    def _ports(self):
        return [
            fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "FWOG main template"),
            fw.CpuPort("COM60", fw.RPI_USB_VID, "S2", "FWOG display lcd"),
        ]

    def test_touches_the_identified_port_and_reports_the_volume(self):
        touched = []
        rc = fw.do_bootsel(cpu="main", ports=self._ports(),
                           touch=touched.append,
                           find_volume=_volume_after(1, pathlib.Path("E:\\")))
        self.assertEqual(rc, 0)
        self.assertEqual(touched, ["COM65"])

    def test_touches_the_display_when_asked_for_the_display(self):
        touched = []
        fw.do_bootsel(cpu="display", ports=self._ports(),
                      touch=touched.append,
                      find_volume=_volume_after(1, pathlib.Path("E:\\")))
        self.assertEqual(touched, ["COM60"])

    def test_explicit_port_skips_identification_entirely(self):
        # The bootstrap path: before both CPUs carry structural product
        # strings, nothing is identifiable and --port is the only way in.
        touched = []
        rc = fw.do_bootsel(port="COM99", ports=[], touch=touched.append,
                           find_volume=_volume_after(1, pathlib.Path("E:\\")))
        self.assertEqual(rc, 0)
        self.assertEqual(touched, ["COM99"])

    def test_refuses_when_a_volume_is_already_mounted(self):
        # Touching now would create a SECOND RPI-RP2 volume, which is exactly
        # the state fact 15 exists to prevent -- and neither can be identified
        # once mounted.
        touched = []
        rc = fw.do_bootsel(cpu="main", ports=self._ports(),
                           touch=touched.append,
                           find_volume=lambda: pathlib.Path("E:\\"))
        self.assertEqual(rc, 1)
        self.assertEqual(touched, [], "must not touch anything")

    def test_reports_failure_when_no_volume_appears(self):
        touched = []
        rc = fw.do_bootsel(cpu="main", ports=self._ports(),
                           touch=touched.append, timeout_s=0,
                           find_volume=lambda: None)
        self.assertEqual(rc, 1)

    def test_unidentifiable_cpu_touches_nothing(self):
        touched = []
        ports = [fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "Pico")]
        rc = fw.do_bootsel(cpu="main", ports=ports, touch=touched.append,
                           find_volume=lambda: None)
        self.assertEqual(rc, 1)
        self.assertEqual(touched, [])

    def test_two_mounted_volumes_are_refused(self):
        def boom():
            raise fw.MultipleRp2VolumesError("two volumes")
        touched = []
        rc = fw.do_bootsel(cpu="main", ports=self._ports(),
                           touch=touched.append, find_volume=boom)
        self.assertEqual(rc, 1)
        self.assertEqual(touched, [])

    def test_print_mode_touches_nothing(self):
        touched = []
        rc = fw.do_bootsel(cpu="main", ports=self._ports(), do_print=True,
                           touch=touched.append,
                           find_volume=lambda: None)
        self.assertEqual(rc, 0)
        self.assertEqual(touched, [])


def _volume_after(n, vol):
    """A find_volume that returns None n times, then the volume. Models the
    real delay between the touch and the drive appearing."""
    state = {"calls": 0}

    def find():
        state["calls"] += 1
        return vol if state["calls"] > n else None
    return find


class TestFlashTouches(unittest.TestCase):
    """fw flash identifies the CPU from the app-name suffix -- which it has
    always trusted, and which new_app refuses to let contradict the template --
    then touches only that CPU. Every uncertain path degrades to the prompt
    that is already proven on hardware."""

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.uf2 = pathlib.Path(self.tmp.name) / "template_main.uf2"
        self.uf2.write_bytes(b"UF2\n")
        # Signature must match the real uf2_path(app, override=None): do_flash
        # passes the --uf2 value positionally, so a one-arg double would fail
        # for reasons that have nothing to do with what these tests check.
        patcher = unittest.mock.patch.object(fw, "uf2_path",
                                             lambda app, override=None: self.uf2)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.vol = pathlib.Path(self.tmp.name) / "vol"
        self.vol.mkdir()

        # do_flash's print statements are the real ones, and on real
        # hardware they name real COM ports and real drive letters -- so
        # even though every test here injects fakes, capture stdout/stderr
        # rather than letting them hit the real streams. self.out/self.err
        # are available to every test in the class, not just the ones that
        # used to redirect locally.
        self.out = io.StringIO()
        self.err = io.StringIO()
        stack = contextlib.ExitStack()
        stack.enter_context(contextlib.redirect_stdout(self.out))
        stack.enter_context(contextlib.redirect_stderr(self.err))
        self.addCleanup(stack.close)

    def _ports(self):
        return [
            fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "FWOG main template"),
            fw.CpuPort("COM60", fw.RPI_USB_VID, "S2", "FWOG display lcd"),
        ]

    def test_touches_the_apps_own_cpu_then_copies(self):
        touched = []
        rc = fw.do_flash("template_main", False, ports=self._ports(),
                         touch=touched.append,
                         find_volume=_volume_after(1, self.vol))
        self.assertEqual(rc, 0)
        self.assertEqual(touched, ["COM65"], "must touch main, not display")
        self.assertTrue((self.vol / "template_main.uf2").exists())

    def test_already_mounted_volume_is_used_without_touching(self):
        # The fresh-board path, and the only way to flash a CPU that is
        # already in BOOTSEL. Today's exact behaviour; must not regress.
        touched = []
        paused = []
        rc = fw.do_flash("template_main", False, ports=self._ports(),
                         touch=touched.append,
                         find_volume=lambda: self.vol,
                         pause=lambda: paused.append(True))
        self.assertEqual(rc, 0)
        self.assertEqual(touched, [], "must not touch when already mounted")
        self.assertTrue((self.vol / "template_main.uf2").exists())
        # This is the one remaining path by which a main image can reach the
        # display CPU -- identification never runs here -- so the warning
        # must actually be printed, and the pause that makes "Ctrl-C now" a
        # real affordance rather than an empty suggestion must actually run.
        printed = self.out.getvalue()
        self.assertIn("Ctrl-C now if that is not the MAIN CPU.", printed)
        self.assertEqual(paused, [True], "must actually pause, not just say so")

    def test_unidentifiable_cpu_falls_back_to_the_manual_prompt(self):
        # A board whose binaries predate fwog_usb_product() reports "Pico".
        # Nothing is identified, nothing is touched, and the operator is asked
        # exactly as before -- and that prompt must actually be printed, not
        # just implied by the return code, or a change that silently dropped
        # it would keep this test green.
        touched = []
        ports = [fw.CpuPort("COM65", fw.RPI_USB_VID, "S1", "Pico")]
        rc = fw.do_flash("template_main", False, ports=ports,
                         touch=touched.append,
                         find_volume=_volume_after(1, self.vol))
        self.assertEqual(rc, 0)
        self.assertEqual(touched, [])
        self.assertTrue((self.vol / "template_main.uf2").exists())
        printed = self.out.getvalue()
        self.assertIn("note:", printed)
        self.assertIn("no running RP2040 identified as the main CPU", printed)
        self.assertIn("Put the MAIN CPU into BOOTSEL, waiting for RPI-RP2 ...",
                      printed)

    def test_two_volumes_refuse_before_anything_is_touched(self):
        def boom():
            raise fw.MultipleRp2VolumesError("two volumes")
        touched = []
        rc = fw.do_flash("template_main", False, ports=self._ports(),
                         touch=touched.append, find_volume=boom)
        self.assertEqual(rc, 1)
        self.assertEqual(touched, [])

    def test_timeout_waiting_for_the_volume_is_reported_and_nothing_is_copied(self):
        # do_flash's own timeout path (distinct from do_bootsel's, which has a
        # different message and return path): the CPU was identified and
        # touched, but no volume ever showed up. timeout_s=0 makes
        # _wait_for_volume perform exactly one check, so this stays fast.
        touched = []
        rc = fw.do_flash("template_main", False, ports=self._ports(),
                         touch=touched.append,
                         find_volume=lambda: None, timeout_s=0)
        self.assertEqual(rc, 1)
        self.assertEqual(touched, ["COM65"])
        self.assertIn("no RPI-RP2 volume appeared", self.err.getvalue())
        self.assertFalse((self.vol / "template_main.uf2").exists())

    def test_missing_uf2_is_reported_before_touching_anything(self):
        self.uf2.unlink()
        touched = []
        rc = fw.do_flash("template_main", False, ports=self._ports(),
                         touch=touched.append,
                         find_volume=lambda: self.vol)
        self.assertEqual(rc, 1)
        self.assertEqual(touched, [])

    def test_display_app_touches_the_display(self):
        touched = []
        fw.do_flash("lcd_display", False, ports=self._ports(),
                    touch=touched.append,
                    find_volume=_volume_after(1, self.vol))
        self.assertEqual(touched, ["COM60"])


if __name__ == "__main__":
    unittest.main()
