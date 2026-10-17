# Non-canonical weight fixture

`regular-600.ttf` is derived from `../../gdi-fonts/regular.ttf`, covered by the
adjacent copied CC0 license. Its only semantic change is
`OS/2.usWeightClass`: 400 to 600. Names, glyphs, outlines, style flags, and
source timestamps are unchanged; FontTools recalculates the required table
checksums and `head.checkSumAdjustment` when saving.

The inventory E2E loads only this directory, excludes system fonts, and requests
the regular family. The expected realized weight is 600 without synthetic bold
or italic; non-canonical metadata must remain informational rather than yellow.

Regenerate from the repository root with installed FontTools:

```python
from fontTools.ttLib import TTFont
font = TTFont("tests/fixtures/gdi-fonts/regular.ttf", recalcTimestamp=False)
font["OS/2"].usWeightClass = 600
font.save("tests/fixtures/font-usage-inventory/noncanonical-fonts/regular-600.ttf", reorderTables=False)
font.close()
```

FontTools is required only to regenerate this committed asset, not to run tests.
