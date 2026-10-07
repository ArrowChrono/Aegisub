// Copyright (c) 2005, Rodrigo Braz Monteiro
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

#include "export_framerate_file.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "export_framerate_transform.h"

#include <libaegisub/of_type_adaptor.h>

namespace {
class FramerateTransformer {
	AssDialogue *line = nullptr;
	int newStart = 0;
	int newEnd = 0;
	int oldAssStart = 0;
	int oldAssEnd = 0;
	int newAssStart = 0;
	int newAssEnd = 0;
	int newK = 0;
	int oldK = 0;

	agi::vfr::Framerate const& Input;
	agi::vfr::Framerate const& Output;
	static void TransformTimeTags(std::string const& name, AssOverrideParameter *curParam, void *userdata);

	public:
	FramerateTransformer(agi::vfr::Framerate const& source, agi::vfr::Framerate const& destination)
		: Input(destination), Output(source) {}
	void TransformFrameRate(AssFile *subs);
};

void FramerateTransformer::TransformTimeTags(std::string const& name, AssOverrideParameter *curParam, void *curData) {
	(void)name;
	VariableDataType type = curParam->GetType();
	if (type != VariableDataType::INT && type != VariableDataType::FLOAT)
		return;

	FramerateTransformer *instance = static_cast<FramerateTransformer *>(curData);
	int parVal = curParam->Get<int>();
	AssFramerateTransform const transform{
		instance->newStart,
		instance->newEnd,
		instance->oldAssStart,
		instance->oldAssEnd,
		instance->newAssStart,
		instance->newAssEnd};

	switch (curParam->classification) {
		case AssParameterClass::RELATIVE_TIME_START: {
			int value = TransformRelativeStartTagTimeForExport(
				instance->Output,
				instance->Input,
				transform,
				parVal);

			// An end time of 0 is actually the end time of the line, so ensure
			// nonzero is never converted to 0
			// Needed here rather than the end case because start/end here mean
			// which end of the line the time is relative to, not whether it's
			// the start or end time (compare \move and \fad)
			if (value == 0 && parVal != 0)
				value = 1;
			curParam->Set(value);
			break;
		}
		case AssParameterClass::RELATIVE_TIME_END:
			curParam->Set(TransformRelativeEndTagTimeForExport(
				instance->Output,
				instance->Input,
				transform,
				parVal));
			break;
		case AssParameterClass::KARAOKE_START: {
			curParam->Set(TransformKaraokeStartForExport(
				instance->Output,
				instance->Input,
				transform,
				parVal,
				instance->oldK,
				instance->newK));
			break;
		}
		case AssParameterClass::KARAOKE: {
			curParam->Set(TransformKaraokeDurationForExport(
				instance->Output,
				instance->Input,
				transform,
				parVal,
				instance->oldK,
				instance->newK));
			break;
		}
		default:
			return;
	}
}

void FramerateTransformer::TransformFrameRate(AssFile *subs) {
	if (!Input.IsLoaded() || !Output.IsLoaded())
		return;
	for (auto& curDialogue : subs->Events) {
		line = &curDialogue;
		newK = 0;
		oldK = 0;
		auto const transform = BuildAssFramerateTransform(Output, Input, curDialogue.Start, curDialogue.End);
		newStart = transform.new_start_ms;
		newEnd = transform.new_end_ms;
		oldAssStart = transform.old_ass_start_ms;
		oldAssEnd = transform.old_ass_end_ms;
		newAssStart = transform.new_ass_start_ms;
		newAssEnd = transform.new_ass_end_ms;

		auto blocks = line->ParseTags();
		for (auto block : blocks | agi::of_type<AssDialogueBlockOverride>())
			block->ProcessParameters(TransformTimeTags, this);
		curDialogue.Start = newStart;
		curDialogue.End = newEnd;
		curDialogue.UpdateText(blocks);
	}
}

}

void TransformAssFileFramerateForExport(AssFile& subs,
										agi::vfr::Framerate const& source,
										agi::vfr::Framerate const& destination) {
	FramerateTransformer(source, destination).TransformFrameRate(&subs);
}
