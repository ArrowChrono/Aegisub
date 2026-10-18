// Copyright (c) 2007, Alysson Souza e Silva (demi_alucard)
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

#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "project.h"
#include "subtitle_fps_choice.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/math_utils.h>

#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
wxString FormatColorRange(SourceFrameColorRange range) {
	switch (range) {
		case SourceFrameColorRange::Limited:
			return _("Limited");
		case SourceFrameColorRange::Full:
			return _("Full");
		default:
			return _("Unknown");
	}
}

wxString FormatColorMetadata(SourceFrameColorMetadata const& color) {
	wxArrayString parts;
	parts.push_back(fmt_wx("matrix=%s", to_wx(color.matrix.empty() ? std::string("Unknown") : color.matrix)));
	parts.push_back(fmt_wx("primaries=%s", to_wx(color.primaries.empty() ? std::string("Unknown") : color.primaries)));
	parts.push_back(fmt_wx("transfer=%s", to_wx(color.transfer.empty() ? std::string("Unknown") : color.transfer)));
	parts.push_back(fmt_wx("range=%s", FormatColorRange(color.range)));
	return wxJoin(parts, ',');
}
}

void ShowVideoDetailsDialog(agi::Context *c) {
	wxDialog d(c->GetUI().parent, -1, _("Video Details"));

	auto core = c->GetCore();
	auto provider = core.project->VideoProvider();
	auto width = provider->GetWidth();
	auto height = provider->GetHeight();
	auto framecount = provider->GetFrameCount();
	auto fps = provider->GetFPS();
	auto [ar_width, ar_height] = agi::util::reduce_ratio(width, height);

	auto fg = new wxFlexGridSizer(2, 5, 10);
	auto make_field = [&](wxString const& name, wxString const& value) {
		fg->Add(new wxStaticText(&d, -1, name), 0, wxALIGN_CENTRE_VERTICAL);
		fg->Add(new wxTextCtrl(&d, -1, value, wxDefaultPosition, wxSize(300,-1), wxTE_READONLY), 0, wxALIGN_CENTRE_VERTICAL | wxEXPAND);
	};
	make_field(_("File name:"), core.project->VideoName().wstring());
	// Prefer rational form (e.g. 24000/1001) so NTSC film is not mistaken for
	// the rounded decimal 23.976 used by some tools/providers.
	make_field(_("FPS:"), to_wx(FormatVideoFpsDescription(fps)));
	make_field(_("Resolution:"), fmt_wx("%dx%d (%d:%d)", width, height, ar_width, ar_height));
	make_field(_("Length:"), fmt_plural(framecount, "1 frame", "%d frames (%s)",
		framecount, agi::Time(fps.TimeAtFrame(framecount - 1)).GetAssFormatted(true)));
	make_field(_("Decoder:"), to_wx(provider->GetDecoderName()));
	make_field(_("Source output mode:"), to_wx(SourceFrameOutputModeName(provider->GetSelectedSourceMode())));
	auto source_format = provider->GetNativeFormatDescription();
	make_field(_("Source format:"), source_format.empty() ? _("Unavailable") : to_wx(source_format));
	make_field(_("Render color metadata:"), FormatColorMetadata(provider->GetColorMetadata()));
	make_field(_("Source color metadata:"), FormatColorMetadata(provider->GetRealColorMetadata()));
	if (provider->GetColorSpace() != provider->GetRealColorSpace()) {
		make_field(_("Matrix override:"), fmt_wx("%s -> %s",
			provider->GetRealColorSpace(),
			provider->GetColorSpace()));
	}
	if (!provider->GetWarning().empty())
		make_field(_("Warning:"), to_wx(provider->GetWarning()));

	auto video_sizer = new wxStaticBoxSizer(wxVERTICAL, &d, _("Video"));
	video_sizer->Add(fg);

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(video_sizer, 1, wxALL|wxEXPAND, 5);
	main_sizer->Add(d.CreateSeparatedButtonSizer(wxOK), 0, wxALL|wxEXPAND, 5);
	d.SetSizerAndFit(main_sizer);

	d.CenterOnParent();
	d.ShowModal();
}
