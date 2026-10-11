// Copyright (c) 2006, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "automation_text_extents.h"

#include "ass_style.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/scope_exit.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>

namespace Automation4 {

bool CalculateAutomationTextExtents(AssStyle const& style, std::string const& text,
									double& width, double& height, double& descent, double& extlead) {
	width = height = descent = extlead = 0;

	double fontsize = style.fontsize * 64;
	double spacing = style.spacing * 64;

	auto dc = CreateCompatibleDC(nullptr);
	if (!dc) return false;
	auto release_dc = agi::make_scope_exit([dc] { DeleteDC(dc); });

	SetMapMode(dc, MM_TEXT);

	LOGFONTW lf = {};
	lf.lfHeight = static_cast<LONG>(fontsize);
	lf.lfWeight = style.bold ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = style.italic;
	lf.lfUnderline = style.underline;
	lf.lfStrikeOut = style.strikeout;
	lf.lfCharSet = static_cast<BYTE>(style.encoding);
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	wcsncpy(lf.lfFaceName, agi::charset::ConvertW(style.font).c_str(), 31);

	auto font = CreateFontIndirectW(&lf);
	if (!font) return false;

	auto old_font = SelectObject(dc, font);
	auto release_font = agi::make_scope_exit([dc, font, old_font] {
		SelectObject(dc, old_font);
		DeleteObject(font);
	});

	std::wstring wtext(agi::charset::ConvertW(text));
	if (spacing != 0) {
		width = 0;
		for (auto c : wtext) {
			SIZE sz;
			GetTextExtentPoint32W(dc, &c, 1, &sz);
			width += sz.cx + spacing;
			height = sz.cy;
		}
	}
	else {
		SIZE sz;
		GetTextExtentPoint32W(dc, &wtext[0], static_cast<int>(wtext.size()), &sz);
		width = sz.cx;
		height = sz.cy;
	}

	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	descent = tm.tmDescent;
	extlead = tm.tmExternalLeading;

	width = style.scalex / 100 * width / 64;
	height = style.scaley / 100 * height / 64;
	descent = style.scaley / 100 * descent / 64;
	extlead = style.scaley / 100 * extlead / 64;

	return true;
}

}
