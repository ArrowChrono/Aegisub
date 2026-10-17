"""Run the inventory process contract against independent ASS/font fixtures."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path, PureWindowsPath
import shutil
import subprocess
import sys
import time
import unittest


ROOT = Path.cwd()
FIXTURES = Path("tests/fixtures/font-usage-inventory")
FONTS = Path("tests/fixtures/gdi-fonts")
OPTIONS = None
RECORDS = []
DEADLINE = 0.0


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def windows_ui_languages():
    if sys.platform != "win32":
        raise RuntimeError("The inventory E2E requires Windows for the real exclusive-share lock and UI-language oracle")
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    query = kernel32.GetUserPreferredUILanguages
    query.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_wchar_p,
                      ctypes.POINTER(ctypes.c_uint32)]
    query.restype = ctypes.c_int
    count, size = ctypes.c_uint32(), ctypes.c_uint32()
    if not query(8, ctypes.byref(count), None, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    if size.value < 2 or size.value > 32768:
        raise RuntimeError("Invalid Windows preferred UI-language buffer size")
    buffer = ctypes.create_unicode_buffer(size.value)
    if not query(8, ctypes.byref(count), buffer, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    languages = [language for language in buffer[:size.value].split("\0") if language]
    if not languages or len(languages) != count.value:
        raise RuntimeError("Windows did not return the complete preferred UI-language list")
    return languages


def private_paths(value):
    if isinstance(value, str):
        value = value.replace(str(ROOT), ".").replace(ROOT.as_posix(), ".")
        if PureWindowsPath(value).is_absolute():
            return "external-fonts/" + PureWindowsPath(value).name
        if Path(value).is_absolute():
            return "external-fonts/" + Path(value).name
        return value
    if isinstance(value, list):
        return [private_paths(item) for item in value]
    if isinstance(value, dict):
        return {key: private_paths(item) for key, item in value.items()}
    return value


class InventoryE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.artifacts = Path(OPTIONS.artifacts)
        cls.artifacts.mkdir(parents=True, exist_ok=False)
        cls.runtime = cls.artifacts / "runtime"
        shutil.copytree(FIXTURES, cls.runtime)
        cls.data = cls.runtime / "root"
        cls.expected = json.loads((FIXTURES / "expected.json").read_text(encoding="utf-8"))
        cls.ui_languages = windows_ui_languages()
        cls.system_ui_locale = cls.ui_languages[0]
        shutil.copyfile(FONTS / "regular.ttf", cls.runtime / "fallback-fonts" / cls.expected["fallback"]["primary_file"])
        cls.before = {str(path): fingerprint(path) for path in cls.data.rglob("*") if path.is_file()}
        cls.sequence = 0
        cls.private = ["--matcher", "libass", "--additional-fonts", FONTS.as_posix(), "--exclude-system-fonts"]
        (cls.runtime / "empty").mkdir()
        header = (cls.data / "b.ASS").read_text(encoding="utf-8").split("Dialogue:")[0]
        cls.write("comments.ass", header + "Comment: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,A\n")
        cls.glyph_source_line = len(header.splitlines()) + 2
        cls.write("glyph.ass", header + "Dialogue: 0,0:00:03.00,0:00:04.00,Default,newline-only,0,0,0,,\\N\n"
                  + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,missing-glyphs-only,0,0,0,,BB\n")
        cls.write("warning.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\b1}A\n")
        cls.write("red-warning.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\b1}BB\n")
        cls.write("warning-italic.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\i1}A\n")
        cls.write("red-warning-italic.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\i1}BB\n")
        cls.write("ambiguous-red.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,AB\n")
        (cls.runtime / "ambiguous-fonts").mkdir()
        for filename in ("first.ttf", "second.ttf"):
            shutil.copyfile(FONTS / "regular.ttf", cls.runtime / "ambiguous-fonts" / filename)
        cls.write("aliases.ass", header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,A{\\fn" + cls.expected["family"] + " Regular}A\n")
        cls.write("missing.ass", (header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,A\n").replace(cls.expected["family"], cls.expected["missing_family"]))
        cls.write("short-missing.ass", (header + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,A\n").replace(cls.expected["family"], cls.expected["short_missing_family"]))
        cls.write("unsupported.ssa", header)
        cls.write("broken.ass", header + "Dialogue: 0,not-a-time,0:00:02.00,Default,malformed-time,0,0,0,,A\n")
        (cls.artifacts / "fingerprints.json").write_text(json.dumps({
            "executable_sha256": fingerprint(Path(OPTIONS.exe)),
            "fonts": {path.name: fingerprint(path) for path in FONTS.glob("*.ttf")},
            "fallback_font_sha256": fingerprint(cls.runtime / "fallback-fonts" / cls.expected["fallback"]["fallback_file"]),
            "noncanonical_font_sha256": fingerprint(FIXTURES / "noncanonical-fonts" / "regular-600.ttf"),
            "oracle_sha256": fingerprint(FIXTURES / "expected.json"),
        }, indent=2), encoding="utf-8")
        (cls.artifacts / "locale-precondition.json").write_text(json.dumps({
            "source": "GetUserPreferredUILanguages(MUI_LANGUAGE_NAME)",
            "languages": cls.ui_languages,
            "primary_inventory_locale": cls.system_ui_locale,
        }, indent=2), encoding="utf-8")

    @classmethod
    def write(cls, name, text):
        (cls.runtime / name).write_text(text, encoding="utf-8")

    @classmethod
    def tearDownClass(cls):
        after = {str(path): fingerprint(path) for path in cls.data.rglob("*") if path.is_file()}
        if after != cls.before:
            raise AssertionError("inventory changed its read-only three-file source fixture")

    def run_cli(self, command, inputs, options=(), expected=0, structured=True, error_contains=()):
        type(self).sequence += 1
        stem = f"{self.sequence:02d}-{self._testMethodName}"
        arguments = [OPTIONS.exe, "fontcollector", command, *map(str, inputs), *options]
        remaining = DEADLINE - time.monotonic()
        self.assertGreater(remaining, 0, "900-second suite deadline exceeded")
        started = time.monotonic()
        try:
            process = subprocess.run(arguments, capture_output=True, timeout=min(60, remaining), check=False)
        except subprocess.TimeoutExpired as error:
            (self.artifacts / f"{stem}.timeout.json").write_text(json.dumps({
                "test": self._testMethodName, "passed": False, "timeout_seconds": min(60, remaining),
                "stdout": private_paths((error.stdout or b"").decode("utf-8", errors="replace")),
                "stderr": private_paths((error.stderr or b"").decode("utf-8", errors="replace")),
            }, indent=2), encoding="utf-8")
            self.fail("CLI subprocess timed out; no retry is permitted")
        stdout = process.stdout.decode("utf-8", errors="strict")
        stderr = process.stderr.decode("utf-8", errors="strict")
        try:
            saved_stdout = json.dumps(private_paths(json.loads(stdout)), ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            saved_stdout = private_paths(stdout)
        (self.artifacts / f"{stem}.stdout.txt").write_text(saved_stdout, encoding="utf-8")
        (self.artifacts / f"{stem}.stderr.txt").write_text(private_paths(stderr), encoding="utf-8")
        RECORDS.append({"test": self._testMethodName, "command": private_paths(arguments[1:]),
                        "exit_code": process.returncode, "elapsed_seconds": round(time.monotonic() - started, 3)})
        self.assertIsNotNone(expected, "Every process contract requires an exact expected exit code")
        self.assertEqual(process.returncode, expected, stderr)
        for message in error_contains:
            self.assertIn(message, stderr)
        if not structured:
            self.assertEqual(stdout, "", "Parser errors belong exclusively on stderr")
            return stdout
        self.assertFalse(stdout.startswith("\ufeff"), "stdout must be plain UTF-8 JSON without a BOM")
        report = json.loads(stdout)
        self.assertIsInstance(report, dict)
        self.assertEqual(report["result"], process.returncode)
        if command == "inventory":
            self.assertEqual(report["format"], "font_usage_inventory")
            self.assertEqual(report["schema_version"], 1)
            self.assertIs(type(report["complete"]), bool)
            self.assertIsInstance(report["diagnostics"], list)
            self.assertEqual(report["locale"], self.system_ui_locale)
            self.assertTrue(report["provider"])
            for font in report["fonts"]:
                self.assertIn(font["display_state"], {"normal", "missing_font", "missing_glyphs", "warning", "low_usage"})
                self.assertIs(type(font["usage_count"]), int)
                self.assertIs(type(font["is_low_usage"]), bool)
                self.assertIs(type(font["missing_font"]), bool)
                self.assertEqual(sum(source["usage_count"] for source in font["sources"]), font["usage_count"])
                self.assertEqual(len(font["ass_names"]), len(set(font["ass_names"])))
                self.assertIsInstance(font["reasons"], list)
                self.assertIsInstance(font["warnings"], list)
        return report

    def inventory(self, inputs=None, options=(), expected=0, private=True):
        return self.run_cli("inventory", [self.data] if inputs is None else inputs,
                            [*(self.private if private else []), *options], expected)

    def family(self, report, name=None):
        matches = [font for font in report["fonts"] if (name or self.expected["family"]) in font["ass_names"]]
        self.assertEqual(len(matches), 1, report["fonts"])
        return matches[0]

    def assert_count(self, report, recursive):
        expected = self.expected["recursive_sources" if recursive else "nonrecursive_sources"]
        self.assertTrue(report["complete"])
        self.assertEqual(report["recursive"], recursive)
        self.assertEqual(len(report["fonts"]), 1)
        row = self.family(report)
        self.assertEqual(row["usage_count"], sum(expected.values()))
        self.assertEqual({Path(source["input"]).name: source["usage_count"] for source in row["sources"]}, expected)
        self.assertEqual(row["missing_codepoints"], [])
        self.assertFalse(row["missing_font"])
        self.assertEqual(row["warnings"], [])
        self.assertEqual(len(report["files"]), len(expected))
        self.assertTrue(all(file["status"] == "ok" for file in report["files"]))
        return row

    def test_three_file_interval_deduplication_and_threshold_boundaries(self):
        for recursive in (False, True):
            for threshold in (0, 5, 6):
                with self.subTest(recursive=recursive, threshold=threshold):
                    report = self.inventory(options=[*(["--recursive"] if recursive else []), "--low-frequency-threshold", str(threshold)])
                    row = self.assert_count(report, recursive)
                    count = str(row["usage_count"])
                    state = self.expected["threshold_states"][str(threshold)][count]
                    self.assertEqual(row["display_state"], state)
                    self.assertEqual(row["is_low_usage"], state == "low_usage")
                    self.assertEqual([reason["code"] for reason in row["reasons"]], ["low_usage"] if state == "low_usage" else [])
                    self.assertEqual([reason["message"] for reason in row["reasons"]], ["Low usage"] if state == "low_usage" else [])

    def test_default_json_is_equivalent_to_explicit_json(self):
        implicit = self.inventory()
        explicit = self.inventory(options=["--json"])
        self.assertEqual(implicit, explicit)
        self.assertEqual(implicit["low_frequency_threshold"], 5)
        self.assertEqual(implicit["matcher"], "libass")

    def test_multiple_sources_deduplicate_and_ignore_input_order(self):
        first = self.inventory([self.data / "b.ASS", self.data, self.data / "a.ass"], ["--recursive"])
        second = self.inventory([self.data / "a.ass", self.data / "b.ASS", self.data], ["--recursive"])
        self.assert_count(first, True)
        self.assertEqual(first["fonts"], second["fonts"])
        self.assertEqual(first["files"], second["files"])

    def test_single_file_keeps_same_time_variants_once(self):
        report = self.inventory([self.data / "a.ass"])
        self.assertEqual(len(report["fonts"]), 1)
        self.assertEqual(self.family(report)["usage_count"], 2)
        self.assertEqual(len(report["files"]), 1)

    def test_requests_preserve_explicit_tags_style_variants_and_matching_evidence(self):
        row = self.family(self.inventory([self.data / "a.ass"]))
        requests = row["requests"]
        self.assertEqual({(request["effective_weight"], request["italic"]) for request in requests}, {(400, False), (700, False), (400, True)})
        self.assertTrue(any(request["raw_bold_tag"] == "1" and request["explicit_bold"] for request in requests))
        self.assertTrue(any(request["raw_italic_tag"] == "1" and request["explicit_italic"] for request in requests))
        self.assertTrue(any("Bold" in request["styles"] and request["effective_weight"] == 700 for request in requests))
        for request in requests:
            self.assertEqual(Path(request["input"]).name, "a.ass")
            self.assertEqual(request["ass_name"], self.expected["family"])
            self.assertIsInstance(request["raw_bold_tag"], str)
            self.assertIsInstance(request["raw_italic_tag"], str)
            self.assertTrue(request["lines"])
            self.assertTrue(request["paths"])
            self.assertFalse(request["memory_only"])
            self.assertEqual(request["matched_weight"], request["effective_weight"])
            self.assertEqual(request["matched_italic"], request["italic"])
            self.assertTrue(request["matched_name"])

    def test_confirmed_family_and_fullname_alias_merge_without_losing_raw_names(self):
        report = self.inventory([self.runtime / "aliases.ass"])
        self.assertEqual(len(report["fonts"]), 1)
        row = self.family(report)
        self.assertEqual(row["usage_count"], 1)
        self.assertEqual(set(row["ass_names"]), {self.expected["family"], self.expected["family"] + " Regular"})
        self.assertEqual(row["english_name"], self.expected["family"])
        self.assertTrue(row["display_name"].endswith("<" + self.expected["family"] + ">"))
        self.assertNotIn("null", row["display_name"])
        self.assertFalse(row["missing_font"])

    def test_fallback_face_preserves_primary_family_and_all_font_paths(self):
        expected = self.expected["fallback"]
        fonts = self.runtime / "fallback-fonts"
        inputs = self.runtime / "fallback-inputs"
        original_inputs = {path.name: path.read_bytes() for path in inputs.glob("*.ass")}
        options = ["--matcher", "libass", "--additional-fonts", str(fonts / expected["fallback_file"]),
                   "--additional-fonts", str(fonts / expected["primary_file"]), "--exclude-system-fonts",
                   "--low-frequency-threshold", "0"]
        report = self.run_cli("inventory", [inputs], options)
        self.assertTrue(report["complete"])
        self.assertEqual(len(report["fonts"]), 1, "A-only and AB must retain one primary family even when B uses a preceding catalog face")
        row = self.family(report)
        self.assertEqual(row["ass_names"], [self.expected["family"]])
        self.assertEqual(row["usage_count"], expected["usage_count"])
        self.assertEqual(len(row["sources"]), 2)
        self.assertEqual({Path(source["input"]).name: source["usage_count"] for source in row["sources"]}, expected["sources"])
        primary_language = self.system_ui_locale.replace("_", "-").split("-", 1)[0].casefold()
        self.assertEqual(row["localized_name"], expected["localized_name_by_primary_language"].get(primary_language, expected["localized_name_default"]))
        self.assertEqual(row["english_name"], expected["english_name"])
        self.assertEqual(row["display_name"], expected["display_name"])
        self.assertEqual(row["missing_codepoints"], [])
        self.assertFalse(row["missing_font"])
        self.assertEqual(row["display_state"], "normal")
        self.assertEqual(row["warnings"], [])
        self.assertEqual(len(row["requests"]), 2)
        by_input = {Path(request["input"]).name: request for request in row["requests"]}
        self.assertEqual(set(by_input), set(expected["sources"]))
        for request in by_input.values():
            self.assertEqual(request["ass_name"], self.expected["family"])
            self.assertEqual(request["matched_name"], self.expected["family"])
            self.assertEqual(request["matched_full_name"], expected["matched_full_name"])
            self.assertEqual(request["face_index"], expected["primary_face_index"])
            self.assertFalse(request["memory_only"])
        self.assertEqual({Path(path).name for path in by_input["a-only.ass"]["paths"]}, {expected["primary_file"]})
        self.assertEqual(len(by_input["a-only.ass"]["paths"]), 1)
        self.assertEqual({Path(path).name for path in by_input["ab-with-fallback.ass"]["paths"]}, {expected["primary_file"], expected["fallback_file"]})
        self.assertEqual(len(by_input["ab-with-fallback.ass"]["paths"]), 2)
        self.assertEqual({path.name: path.read_bytes() for path in inputs.glob("*.ass")}, original_inputs)

    def test_empty_directory_and_comment_only_file_are_distinct_success(self):
        empty = self.inventory([self.runtime / "empty"])
        comments = self.inventory([self.runtime / "comments.ass"])
        self.assertEqual(empty["fonts"], [])
        self.assertEqual(empty["files"], [])
        self.assertTrue(empty["complete"])
        self.assertEqual(comments["fonts"], [])
        self.assertEqual(len(comments["files"]), 1)
        self.assertEqual(comments["files"][0]["status"], "ok")
        self.assertTrue(comments["complete"])

    def test_exclusive_read_lock_publishes_partial_result_and_recovers(self):
        self.assertEqual(sys.platform, "win32", "The exclusive-share E2E requires Windows; it is not a portable or skipped assertion")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create = kernel32.CreateFileW
        create.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                           ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        create.restype = ctypes.c_void_p
        close = kernel32.CloseHandle
        close.argtypes = [ctypes.c_void_p]
        close.restype = ctypes.c_int
        handle = create(str((self.data / "b.ASS").resolve()), 0x80000000, 0, None, 3, 0x80, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value, f"Cannot acquire the real exclusive-read fixture lock: {ctypes.get_last_error()}")
        self.assertIsNotNone(handle)
        try:
            report = self.inventory(expected=5)
            self.assertFalse(report["complete"])
            row = self.family(report)
            self.assertEqual(row["usage_count"], 2)
            self.assertEqual({Path(source["input"]).name: source["usage_count"] for source in row["sources"]}, {"a.ass": 2})
            self.assertTrue(any(Path(file["input"]).name == "b.ASS" and file["status"] == "error" for file in report["files"]))
            self.assertTrue(any(Path(diagnostic["file"]).name == "b.ASS" for diagnostic in report["diagnostics"]))
        finally:
            self.assertTrue(close(handle), f"Exclusive-read fixture lock cleanup failed: {ctypes.get_last_error()}")
        self.assert_count(self.inventory(), False)

    def test_missing_font_preserves_raw_name_and_unknown_glyph_null(self):
        for filename, key in (("missing.ass", "missing_family"), ("short-missing.ass", "short_missing_family")):
            with self.subTest(family=key):
                family = self.expected[key]
                report = self.inventory([self.runtime / filename], private=False)
                row = self.family(report, family)
                self.assertTrue(report["complete"])
                self.assertTrue(row["missing_font"])
                self.assertEqual(row["display_state"], "missing_font")
                self.assertEqual(row["display_name"], family)
                self.assertIsNone(row["missing_codepoints"])
                self.assertIsNone(row["localized_name"])
                self.assertIsNone(row["english_name"])
                self.assertEqual(row["usage_count"], 1)
                self.assertEqual(row["warnings"], [])
                self.assertEqual([reason["code"] for reason in row["reasons"]], ["missing_font", "low_usage"])
                self.assertEqual([reason["message"] for reason in row["reasons"]], ["Not found", "Low usage"])
                self.assertEqual([(detail["code"], detail["severity"]) for detail in report["diagnostics"]],
                                 [("missing_font", "error"), ("noncanonical_font_variant", "info")])
                detail = report["diagnostics"][1]
                self.assertEqual(detail["message"], "realized font weight or style cannot be safely represented as Regular/Bold/Italic")
                self.assertEqual((detail["ass_name"], detail["requested_weight"], detail["requested_italic"], detail["lines"]),
                                 (family, 400, False, [11]))
                self.assertEqual(Path(detail["file"]).resolve(), (self.runtime / filename).resolve())

    def test_missing_glyphs_are_sorted_unique_and_override_low_usage(self):
        for threshold in (0, 5):
            report = self.inventory([self.runtime / "glyph.ass"], ["--low-frequency-threshold", str(threshold)])
            row = self.family(report)
            self.assertTrue(report["complete"])
            self.assertFalse(row["missing_font"])
            self.assertEqual(row["missing_codepoints"], self.expected["missing_codepoints"])
            self.assertEqual(row["display_state"], "missing_glyphs")
            self.assertEqual(row["usage_count"], 1, "A newline-only event has no actual font usage")
            glyph_diagnostics = [diagnostic for diagnostic in report["diagnostics"] if diagnostic["code"] == "missing_glyphs"]
            self.assertEqual(len(glyph_diagnostics), 1)
            self.assertEqual(glyph_diagnostics[0]["lines"], [self.glyph_source_line], "Only the event containing missing glyphs belongs in the diagnostic")
            self.assertEqual(row["is_low_usage"], threshold > 0)
            self.assertEqual([reason["code"] for reason in row["reasons"]], ["missing_glyphs", *(["low_usage"] if threshold > 0 else [])])
            self.assertEqual([reason["message"] for reason in row["reasons"]], [
                "Missing glyphs: B U+0042", *(["Low usage"] if threshold > 0 else [])])

    def test_variant_details_are_info_only_and_do_not_set_warning_state(self):
        variants = (
            ("noncanonical_font_variant", FIXTURES / "noncanonical-fonts", 600, False, "regular-600.ttf",
             "realized font weight or style cannot be safely represented as Regular/Bold/Italic"),
            ("implicit_variant_fallback", FONTS / "bold.ttf", 700, True, "bold.ttf",
             "font family resolved to a non-regular variant without an explicit ASS variant"),
        )
        for code, fonts, matched_weight, matched_bold, font_file, message in variants:
            for threshold in (0, 5):
                with self.subTest(detail=code, threshold=threshold):
                    options = ["--matcher", "libass", "--additional-fonts", str(fonts), "--exclude-system-fonts",
                               "--low-frequency-threshold", str(threshold)]
                    report = self.run_cli("inventory", [self.data / "b.ASS"], options)
                    row = self.family(report)
                    self.assertTrue(report["complete"])
                    self.assertEqual(row["usage_count"], 1)
                    self.assertFalse(row["missing_font"])
                    self.assertEqual(row["missing_codepoints"], [])
                    self.assertEqual(row["warnings"], [])
                    self.assertEqual(row["display_state"], "low_usage" if threshold else "normal")
                    self.assertEqual(row["is_low_usage"], threshold > 0)
                    self.assertEqual(row["reasons"], [{"code": "low_usage", "message": "Low usage"}] if threshold else [])
                    self.assertEqual(len(report["diagnostics"]), 1)
                    detail = report["diagnostics"][0]
                    self.assertEqual((detail["code"], detail["type"], detail["severity"], detail["message"]),
                                     (code, code, "info", message))
                    self.assertEqual((detail["ass_name"], detail["requested_weight"], detail["requested_italic"], detail["lines"], detail["codepoints"]),
                                     (self.expected["family"], 400, False, [11], []))
                    self.assertEqual(Path(detail["file"]).resolve(), (self.data / "b.ASS").resolve())
                    self.assertEqual(len(row["requests"]), 1)
                    request = row["requests"][0]
                    self.assertEqual((request["effective_weight"], request["italic"], request["explicit_bold"], request["explicit_italic"]),
                                     (400, False, False, False))
                    self.assertEqual((request["matched_weight"], request["matched_bold"], request["matched_italic"]),
                                     (matched_weight, matched_bold, False))
                    self.assertEqual((request["raw_bold_tag"], request["raw_italic_tag"], request["matched_name"]),
                                     ("", "", self.expected["family"]))
                    self.assertEqual([Path(path).name for path in request["paths"]], [font_file])

    def test_warning_and_red_warning_keep_all_reasons(self):
        cases = (
            ("warning.ass", "warning", [], "fake_bold", "Warning: synthetic bold"),
            ("red-warning.ass", "missing_glyphs", [66], "fake_bold", "Warning: synthetic bold"),
            ("warning-italic.ass", "warning", [], "fake_italic", "Warning: synthetic italic"),
            ("red-warning-italic.ass", "missing_glyphs", [66], "fake_italic", "Warning: synthetic italic"),
        )
        for filename, state, missing, warning, message in cases:
            for threshold in (0, 5):
                with self.subTest(source=filename, threshold=threshold):
                    options = ["--matcher", "libass", "--additional-fonts", str(FONTS / "regular.ttf"), "--exclude-system-fonts", "--low-frequency-threshold", str(threshold)]
                    report = self.run_cli("inventory", [self.runtime / filename], options)
                    row = self.family(report)
                    self.assertEqual(row["display_state"], state)
                    self.assertEqual(row["missing_codepoints"], missing)
                    self.assertEqual(row["usage_count"], 1)
                    self.assertEqual(row["is_low_usage"], threshold > 0)
                    self.assertEqual([(detail["code"], detail["severity"]) for detail in row["warnings"]], [(warning, "warning")])
                    self.assertEqual([reason["code"] for reason in row["reasons"]],
                                     [*(["missing_glyphs"] if missing else []), warning, *(["low_usage"] if threshold > 0 else [])])
                    self.assertEqual([reason["message"] for reason in row["reasons"]],
                                     [*(["Missing glyphs: B U+0042"] if missing else []), message, *(["Low usage"] if threshold > 0 else [])])
                    self.assertEqual([(detail["code"], detail["severity"]) for detail in report["diagnostics"]],
                                     [(warning, "warning"), *([("missing_glyphs", "error")] if missing else []), ("noncanonical_font_variant", "info")])
                    detail = report["diagnostics"][-1]
                    self.assertEqual(detail["message"], "realized font weight or style cannot be safely represented as Regular/Bold/Italic")
                    self.assertEqual((detail["requested_weight"], detail["requested_italic"], detail["lines"]),
                                     (700 if warning == "fake_bold" else 400, warning == "fake_italic", [11]))

    def test_ambiguous_match_remains_warning_with_red_and_low_usage_priority(self):
        fonts = self.runtime / "ambiguous-fonts"
        options = ["--matcher", "libass", "--additional-fonts", str(fonts), "--exclude-system-fonts"]
        legacy = self.run_cli("list", [self.data / "b.ASS"], [*options, "--json"])
        self.assertEqual(legacy["schema_version"], 2)
        self.assertEqual([(detail["type"], detail["severity"]) for detail in legacy["diagnostics"]], [("ambiguous_font_match", "warning")])
        analysis = legacy["files"][0]["font_usage"][0]["matched_font"]["match_analysis"]
        self.assertTrue(analysis["ambiguous"])
        self.assertEqual(len(analysis["candidates"]), 2)
        for source, missing in ((self.data / "b.ASS", []), (self.runtime / "ambiguous-red.ass", [66])):
            for threshold in (0, 5):
                with self.subTest(source=source.name, threshold=threshold):
                    report = self.run_cli("inventory", [source], [*options, "--low-frequency-threshold", str(threshold)])
                    row = self.family(report)
                    self.assertEqual(row["usage_count"], 1)
                    self.assertFalse(row["missing_font"])
                    self.assertEqual(row["missing_codepoints"], missing)
                    self.assertEqual(row["display_state"], "missing_glyphs" if missing else "warning")
                    self.assertEqual(row["is_low_usage"], threshold > 0)
                    self.assertEqual([(detail["code"], detail["severity"]) for detail in row["warnings"]], [("ambiguous_font_match", "warning")])
                    self.assertEqual([reason["code"] for reason in row["reasons"]],
                                     [*(["missing_glyphs"] if missing else []), "ambiguous_font_match", *(["low_usage"] if threshold > 0 else [])])
                    self.assertEqual([reason["message"] for reason in row["reasons"]],
                                     [*(["Missing glyphs: B U+0042"] if missing else []), "Warning: ambiguous font match", *(["Low usage"] if threshold > 0 else [])])
                    self.assertEqual([(detail["code"], detail["severity"]) for detail in report["diagnostics"]],
                                     [("ambiguous_font_match", "warning"), *([("missing_glyphs", "error")] if missing else [])])

    def test_partial_failures_preserve_valid_rows_and_exit_priority(self):
        report = self.inventory([self.data, self.runtime / "unsupported.ssa", self.runtime / "absent.ass", self.runtime / "broken.ass"], expected=5)
        self.assertFalse(report["complete"])
        self.assertEqual(self.family(report)["usage_count"], 3)
        self.assertTrue(report["diagnostics"])
        file_states = {Path(file["input"]).name: file["status"] for file in report["files"]}
        self.assertEqual(file_states["absent.ass"], "error")
        self.assertEqual(file_states["broken.ass"], "error")
        for name, code in (("unsupported.ssa", "invalid_argument"), ("absent.ass", "file_error"), ("broken.ass", "file_error")):
            diagnostics = [diagnostic for diagnostic in report["diagnostics"] if Path(diagnostic["file"]).name == name]
            self.assertTrue(diagnostics, "Each invalid input needs its own diagnostic: " + name)
            self.assertEqual({diagnostic["code"] for diagnostic in diagnostics}, {code})
            self.assertTrue(all(diagnostic["severity"] == "error" for diagnostic in diagnostics))

    def test_unsupported_explicit_extension_exits_one(self):
        report = self.inventory([self.runtime / "unsupported.ssa"], expected=1)
        self.assertFalse(report["complete"])
        self.assertEqual(report["fonts"], [])
        self.assertTrue(report["diagnostics"])

    def test_invalid_global_options_do_not_scan_otherwise_bad_input(self):
        report = self.inventory([self.runtime / "absent.ass"], ["--exclude-system-fonts"], expected=1, private=False)
        self.assertFalse(report["complete"])
        self.assertEqual(report["fonts"], [])
        self.assertEqual(report["files"], [])
        self.assertTrue(report["diagnostics"])

    def test_threshold_int_max_and_invalid_syntax(self):
        report = self.inventory(options=["--low-frequency-threshold", "2147483647"])
        self.assertEqual(report["low_frequency_threshold"], 2147483647)
        self.assertEqual(self.family(report)["display_state"], "low_usage")
        for value, message in (("-1", "decimal integer is outside the permitted range"),
                               ("1.5", "value must be a decimal integer"),
                               ("2147483648", "value must be a decimal integer"),
                               ("no-number", "value must be a decimal integer"),
                               ("", "value must be a decimal integer")):
            with self.subTest(value=value):
                self.run_cli("inventory", [self.data], ["--low-frequency-threshold", value], expected=64,
                             structured=False, error_contains=["--low-frequency-threshold", message])

    def test_inventory_rejects_write_only_options(self):
        for options in (["--to", str(self.runtime / "forbidden-copy")], ["--target", "english"], ["--strict"], ["--in-place"]):
            with self.subTest(options=options):
                self.run_cli("inventory", [self.data], options, expected=64, structured=False,
                             error_contains=[options[0], "not expected"])
        self.assertFalse((self.runtime / "forbidden-copy").exists())

    def test_legacy_commands_retain_schema_two_and_strict_exit(self):
        for command in ("list", "check", "validate"):
            report = self.run_cli(command, [self.data / "b.ASS"], [*self.private, "--json"])
            self.assertEqual(report["schema_version"], 2)
            self.assertNotIn("format", report)
        strict = self.run_cli("validate", [self.runtime / "missing.ass"], ["--json"], expected=20)
        self.assertEqual(strict["schema_version"], 2)
        self.assertFalse(strict["ok"])
        self.assertEqual([(detail["type"], detail["severity"], detail["message"]) for detail in strict["diagnostics"]], [
            ("missing_font", "error", "font request could not be resolved"),
            ("noncanonical_font_variant", "warning", "realized font weight or style cannot be safely represented as Regular/Bold/Italic")])
        normalized = self.run_cli("normalize", [self.runtime / "missing.ass"], ["--target", "english", "--json"])
        self.assertEqual(normalized["schema_version"], 2)
        self.assertEqual(normalized["command"], "normalize")
        self.assertEqual(normalized["summary"], {"catalog_available": True, "finding_count": 1, "safe_change_count": 0,
                                                 "scanned_name_count": 1, "unsafe_finding_count": 1})
        self.assertEqual(len(normalized["files"]), 1)
        self.assertEqual([(item["current_name"], item["recommended_name"], item["match"], item["reason"], item["safe_to_apply"])
                          for item in normalized["files"][0]["changes"]],
                         [(self.expected["missing_family"], "", "none", "unrecognized_family_name", False)])
        collected = self.run_cli("collect", [self.data / "b.ASS"], [*self.private, "--to", str(self.runtime / "copied-fonts"), "--json"])
        self.assertEqual(collected["schema_version"], 2)
        self.assertTrue(list((self.runtime / "copied-fonts").rglob("*.ttf")))
        variants = (
            ("noncanonical_font_variant", FIXTURES / "noncanonical-fonts", 600, False, "noncanonical_variant",
             "realized font weight or style cannot be safely represented as Regular/Bold/Italic"),
            ("implicit_variant_fallback", FONTS / "bold.ttf", 700, True, "implicit_variant_fallback",
             "font family resolved to a non-regular variant without an explicit ASS variant"),
        )
        for code, fonts, weight, bold, matched_flag, message in variants:
            for command in ("list", "check", "validate", "collect"):
                with self.subTest(legacy_command=command, detail=code):
                    options = ["--matcher", "libass", "--additional-fonts", str(fonts), "--exclude-system-fonts", "--json"]
                    if command in ("check", "collect"):
                        options.append("--strict")
                    destination = self.runtime / ("legacy-copy-" + code)
                    if command == "collect":
                        options.extend(["--to", str(destination)])
                    report = self.run_cli(command, [self.data / "b.ASS"], options, expected=0)
                    self.assertEqual(report["schema_version"], 2)
                    self.assertNotIn("format", report)
                    self.assertTrue(report["ok"])
                    self.assertEqual(len(report["files"]), 1)
                    self.assertEqual([(detail["type"], detail["severity"], detail["message"]) for detail in report["diagnostics"]],
                                     [(code, "warning", message)])
                    detail = report["diagnostics"][0]
                    self.assertEqual((detail["font"]["facename"], detail["font"]["requested_weight"], detail["font"]["italic"], detail["lines"]),
                                     (self.expected["family"], 400, False, [11]))
                    self.assertEqual(Path(detail["file"]).resolve(), (self.data / "b.ASS").resolve())
                    self.assertEqual(len(report["files"][0]["font_usage"]), 1)
                    matched = report["files"][0]["font_usage"][0]["matched_font"]
                    self.assertEqual((matched["weight"], matched["bold"], matched["italic"], matched["fake_bold"], matched["fake_italic"]),
                                     (weight, bold, False, False, False))
                    self.assertIs(matched[matched_flag], True)
                    for key in ("missing_font_count", "missing_glyph_font_count", "missing_style_count", "collection_failure_count", "fake_bold_count", "fake_italic_count"):
                        self.assertEqual(report["summary"][key], 0)
                    if command == "collect":
                        files = list(destination.rglob("*.ttf"))
                        self.assertEqual(len(files), 1)
                        expected_font = fonts / "regular-600.ttf" if fonts.is_dir() else fonts
                        self.assertEqual(fingerprint(files[0]), fingerprint(expected_font))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", help="newly built Aegisub executable, relative to repository root")
    parser.add_argument("artifacts", help="new artifact directory, relative to repository root")
    OPTIONS = parser.parse_args()
    for name in ("exe", "artifacts"):
        path = Path(getattr(OPTIONS, name))
        if path.is_absolute() or ".." in path.parts:
            parser.error(f"{name} must be repository-relative without parent traversal")
    if not Path("AGENTS.md").is_file() or not FIXTURES.is_dir():
        parser.error("run from the repository root")
    if not Path(OPTIONS.exe).is_file():
        parser.error("executable does not exist")
    if Path(OPTIONS.artifacts).exists():
        parser.error("artifact directory already exists; choose a fresh directory")
    DEADLINE = time.monotonic() + 900
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(InventoryE2E)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    summary = {"passed": result.wasSuccessful(), "tests_run": result.testsRun,
               "failures": [name.id() for name, _ in result.failures],
               "errors": [name.id() if hasattr(name, "id") else str(name) for name, _ in result.errors],
               "processes": RECORDS}
    artifacts = Path(OPTIONS.artifacts)
    if artifacts.is_dir():
        (artifacts / "result.json").write_text(json.dumps(private_paths(summary), ensure_ascii=False, indent=2), encoding="utf-8")
    sys.exit(0 if result.wasSuccessful() else 1)
