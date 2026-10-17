"""Generate the CC0 B-only fallback fixture from the existing A-only font."""

from pathlib import Path

from fontTools.ttLib import TTFont


SOURCE = Path("tests/fixtures/gdi-fonts/regular.ttf")
OUTPUT = Path("tests/fixtures/font-usage-inventory/fallback-fonts/00-fallback-b.ttf")
FAMILY = "Aegisub Aardvark Fallback Fixture"


def generate():
    with TTFont(SOURCE, recalcTimestamp=False) as font:
        font["glyf"].glyphs["B"] = font["glyf"].glyphs.pop("A")
        font["hmtx"].metrics["B"] = font["hmtx"].metrics.pop("A")
        font.setGlyphOrder([".notdef", "space", "B"])
        for table in font["cmap"].tables:
            if table.isUnicode():
                table.cmap = {32: "space", 66: "B"}
        names = {
            1: FAMILY,
            3: "AegisubInventoryFallbackFixture-Regular",
            4: FAMILY + " Regular",
            6: "AegisubInventoryFallbackFixture-Regular",
            16: FAMILY,
        }
        for record in list(font["name"].names):
            if record.nameID in names:
                font["name"].setName(names[record.nameID], record.nameID,
                                     record.platformID, record.platEncID, record.langID)
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        font.save(OUTPUT)


if __name__ == "__main__":
    generate()
