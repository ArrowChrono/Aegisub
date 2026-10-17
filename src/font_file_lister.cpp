// Copyright (c) 2012, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIRIMIRIM
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "font_file_lister.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "ass_style_resolution.h"
#include "font_collector_unicode.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <utility>

namespace {
constexpr size_t PendingCodepointFlushThreshold = 4096;

void Emit(FontCollectorEventSink const& sink, FontCollectorEvent event) {
	if (sink)
		sink(event);
}

std::string_view TextBlockView(AssDialogueBlock& block) {
	if (block.GetType() == AssBlockType::PLAIN)
		return static_cast<AssDialogueBlockPlain&>(block).text;
	if (block.GetType() == AssBlockType::DRAWING)
		return static_cast<AssDialogueBlockDrawing&>(block).text;
	return {};
}

template<class Callback>
bool ForEachAssTextCodepoint(std::string_view text, int wrap_style, Callback&& callback) {
	auto const *data = text.data();
	auto const size = text.size();
	for (size_t i = 0; i < size; ) {
		uint32_t value = 0;
		if (text[i] == '\t') {
			// libass treats literal tab characters as ordinary spaces.
			value = 0x20;
			++i;
		}
		else if (text[i] == '\\' && i + 1 < size) {
			char next = text[++i];
			if (next == 'N' || next == 'n') {
				++i;
				if (next == 'n' && wrap_style != 2)
					value = 0x20;
				else
					continue;
			}
			else if (next == 'h') {
				++i;
				value = 0xA0;
			}
			else if (next == '{' || next == '}') {
				++i;
				value = static_cast<unsigned char>(next);
			}
			else {
				value = '\\';
			}
		}
		else {
			auto const ch = static_cast<unsigned char>(data[i]);
			if (ch < 0x80) {
				value = ch;
				++i;
			}
			else {
				font_collector::unicode::Rune rune;
				int bytes_consumed = 0;
				font_collector::unicode::Rune::DecodeFromUtf8(
					data + i, size - i, rune, bytes_consumed);
				if (bytes_consumed == 0)
					bytes_consumed = 1;
				i += bytes_consumed;
				value = rune.Value();
			}
		}

		if (callback(value))
			return true;
	}

	return false;
}

bool TextContainsAnyCodepoint(std::string_view text, int wrap_style, std::vector<uint32_t> const& codepoints) {
	return ForEachAssTextCodepoint(text, wrap_style, [&](uint32_t value) {
		return std::binary_search(codepoints.begin(), codepoints.end(), value);
	});
}

std::vector<uint32_t> DecodeUniqueUtf8Codepoints(std::string_view text) {
	std::vector<uint32_t> codepoints;
	for (size_t pos = 0; pos < text.size(); ) {
		font_collector::unicode::Rune rune;
		int consumed = 0;
		font_collector::unicode::Rune::DecodeFromUtf8(text.data() + pos, text.size() - pos, rune, consumed);
		if (consumed <= 0) {
			++pos;
			continue;
		}
		pos += consumed;
		codepoints.push_back(rune.Value());
	}

	sort(begin(codepoints), end(codepoints));
	codepoints.erase(unique(codepoints.begin(), codepoints.end()), codepoints.end());
	return codepoints;
}

std::string EncodeCodepointsToUtf8(std::vector<uint32_t> const& codepoints) {
	std::string text;
	for (auto codepoint : codepoints) {
		font_collector::unicode::Rune rune;
		if (font_collector::unicode::Rune::TryCreate(codepoint, rune))
			font_collector::unicode::AppendRuneToUtf8(text, rune);
	}
	return text;
}

std::vector<uint32_t> IntersectSortedCodepoints(std::vector<uint32_t> const& left, std::vector<uint32_t> const& right) {
	std::vector<uint32_t> result;
	result.reserve(std::min(left.size(), right.size()));
	std::set_intersection(begin(left), end(left), begin(right), end(right), std::back_inserter(result));
	return result;
}

void MergeSortedCodepointsInto(std::vector<uint32_t>& destination, std::vector<uint32_t> const& source) {
	if (source.empty())
		return;

	if (destination.empty()) {
		destination = source;
		return;
	}

	std::vector<uint32_t> merged;
	merged.reserve(destination.size() + source.size());
	std::set_union(begin(destination), end(destination), begin(source), end(source), std::back_inserter(merged));
	destination.swap(merged);
}

bool IsFontAffectingTag(std::string_view name) {
	return name == "\\fn" || name == "\\b" || name == "\\i" ||
	       name == "\\fe" || name == "\\fs" || name == "\\r";
}

bool TransformContainsFontTag(AssOverrideTag const& tag) {
	if (tag.Name != "\\t" || tag.Params.size() <= 3 ||
	    tag.Params[3].omitted || tag.Params[3].empty)
		return false;

	auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>();
	if (!nested)
		return false;
	for (auto const& nested_tag : nested->Tags) {
		if (IsFontAffectingTag(nested_tag.Name) || TransformContainsFontTag(nested_tag))
			return true;
	}
	return false;
}

bool IsImplicitVariantFallback(
	aegisub::ass::AssFontRequest const& request,
	CollectionResult const& result) {
	auto const requested_regular = request.effective_weight == aegisub::ass::DefaultFontWeight;

	if (result.realized_status) {
		if (*result.realized_status != FontVariantStatus::Canonical)
			return false;

		auto const role = result.realized_role.value_or(FontVariantRole::Unknown);
		auto const realized_bold = role == FontVariantRole::Bold ||
		                           role == FontVariantRole::BoldItalic;
		auto const realized_italic = role == FontVariantRole::Italic ||
		                             role == FontVariantRole::BoldItalic;
		return (realized_bold && requested_regular && !request.has_explicit_bold) ||
		       (realized_italic && !request.italic && !request.has_explicit_italic);
	}

	if (result.matched_bold || result.matched_italic) {
		return (result.matched_bold && requested_regular && !request.has_explicit_bold) ||
		       (result.matched_italic && !request.italic && !request.has_explicit_italic);
	}

	// Preserve the signal of legacy/custom listers which cannot expose enough
	// realized-face metadata for the collector to derive it independently.
	return result.implicit_variant_fallback;
}

FontCollectorMatchedFont MatchedFont(CollectionResult const& result,
									 bool implicit_variant_fallback, std::string const& missing_text,
									 std::vector<uint32_t> const& missing_codepoints) {
	FontCollectorMatchedFont matched;
	matched.facename = result.matched_facename;
	matched.facename_full = result.matched_facename_full;
	matched.names = result.matched_names;
	matched.primary_path = result.primary_path;
	matched.primary_memory_data = result.primary_memory_data;
	matched.face_index = result.face_index;
	matched.weight = result.matched_weight;
	matched.bold = result.matched_bold;
	matched.italic = result.matched_italic;
	matched.paths = result.paths;
	matched.memory_fonts = result.memory_fonts;
	matched.path_source = result.path_source;
	matched.fake_bold = result.fake_bold;
	matched.fake_italic = result.fake_italic;
	matched.missing_text = missing_text;
	matched.missing_codepoints = missing_codepoints;
	matched.requested_weight = result.requested_weight;
	matched.backend_requested_weight = result.backend_requested_weight;
	matched.realized_role = result.realized_role;
	matched.realized_status = result.realized_status;
	matched.implicit_variant_fallback = implicit_variant_fallback;
	matched.noncanonical_variant = result.noncanonical_variant;
	matched.match_candidates = result.match_candidates;
	matched.match_ambiguous = result.match_ambiguous;
	return matched;
}

int SourceLineNumber(AssDialogue const& line, int dialogue_index) {
	return line.Row >= 0 ? line.Row + 1 : dialogue_index;
}

std::unique_ptr<IFontFileLister> CreateDefaultFontFileLister(FontCollectorEventSink& event_sink) {
	return std::make_unique<FontFileLister>(event_sink);
}
}

FontCollector::FontCollector(FontCollectorEventSink event_sink)
: event_sink(std::move(event_sink))
, owned_lister(CreateDefaultFontFileLister(this->event_sink))
, lister(owned_lister.get())
{
}

FontCollector::FontCollector(FontCollectorEventSink event_sink, std::unique_ptr<IFontFileLister> lister)
: event_sink(std::move(event_sink))
, owned_lister(std::move(lister))
, lister(owned_lister.get())
{
}

FontCollector::FontCollector(FontCollectorEventSink event_sink, IFontFileLister& lister)
: event_sink(std::move(event_sink))
, lister(&lister)
{
}

FontCollector::StyleInfo FontCollector::MakeStyleInfo(AssStyle const& style) const {
	StyleInfo info;
	info.event_style = aegisub::ass::MakeAssFontStyleBaseline(style);
	info.request.family = info.event_style.family;
	info.request.effective_weight = info.event_style.weight;
	info.request.italic = info.event_style.italic;
	info.request.charset = info.event_style.charset;
	info.request.height = info.event_style.height;
	return info;
}

void FontCollector::RecordMissingStyle(MissingStyleLines& missing_style_lines, std::string const& name, int line_index) {
	auto& lines = missing_style_lines[name];
	if (line_index > 0 && (lines.empty() || lines.back() != line_index))
		lines.push_back(line_index);

	if (line_index <= 0 && lines.empty())
		lines.push_back(line_index);
}

void FontCollector::EmitMissingStyles(FileAnalysis& analysis) {
	for (auto const& [name, lines] : analysis.missing_style_lines) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::StyleMissing;
		event.style = name;
		event.lines.reserve(lines.size());
		for (int line : lines) {
			if (line > 0)
				event.lines.push_back(line);
		}
		Emit(analysis.event_sink, std::move(event));
		++analysis.missing;
	}
}

template<class Callback>
bool FontCollector::ForEachLineTextSpan(AssFile const& file, AssDialogue const& line, int line_index, int wrap_style,
                                        Callback&& callback, MissingStyleLines *missing_style_lines) {
	auto *initial_style = aegisub::ass_style_resolution::ResolveEventStyle(file, line.Style);
	if (!initial_style) {
		if (missing_style_lines)
			RecordMissingStyle(*missing_style_lines, line.Style, line_index);
		return false;
	}

	auto const event_style = aegisub::ass::MakeAssFontStyleBaseline(*initial_style);
	aegisub::ass::AssFontStateEvaluator font_state(event_style);

	bool overriden = false;
	int active_wrap_style = wrap_style;
	int active_drawing_level = 0;

	aegisub::ass::AssFontResetStyleResolver resolve_reset_style =
		[&](std::string_view style_name) -> std::optional<aegisub::ass::AssFontStyleBaseline> {
			auto *reset_style = aegisub::ass_style_resolution::ResolveResetStyle(
				file, std::string(style_name));
			if (!reset_style) {
				if (missing_style_lines)
					RecordMissingStyle(*missing_style_lines, std::string(style_name), line_index);
				return std::nullopt;
			}
			return aegisub::ass::MakeAssFontStyleBaseline(*reset_style);
		};

	auto process_transform_controls = [&](auto&& self, std::vector<AssOverrideTag> const& tags) -> void {
		for (auto const& tag : tags) {
			if (tag.Name == "\\q") {
				auto value = tag.Params[0].Get(wrap_style);
				active_wrap_style = value >= 0 && value <= 3 ? value : wrap_style;
			}
			else if (tag.Name == "\\p") {
				auto value = tag.Params[0].Get(0);
				active_drawing_level = value > 0 ? value : 0;
			}
			else if (tag.Name == "\\t" && tag.Params.size() > 3 &&
			         !tag.Params[3].omitted && !tag.Params[3].empty) {
				if (auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
					self(self, nested->Tags);
			}
		}
	};

	auto process_tags = [&](std::vector<AssOverrideTag> const& tags) {
		for (auto const& tag : tags) {
			if (tag.Name == "\\q") {
				auto value = tag.Params[0].Get(wrap_style);
				active_wrap_style = value >= 0 && value <= 3 ? value : wrap_style;
				continue;
			}
			if (tag.Name == "\\p") {
				auto value = tag.Params[0].Get(0);
				active_drawing_level = value > 0 ? value : 0;
				continue;
			}

			font_state.ApplyTag(tag, resolve_reset_style);
			if (tag.Name == "\\t" && tag.Params.size() > 3 &&
			    !tag.Params[3].omitted && !tag.Params[3].empty) {
				if (auto *nested = tag.Params[3].Get<AssDialogueBlockOverride*>())
					process_transform_controls(process_transform_controls, nested->Tags);
			}
			if (tag.Name == "\\r")
				overriden = false;
			else if (font_state.IsValid() &&
			         (IsFontAffectingTag(tag.Name) || TransformContainsFontTag(tag)))
				overriden = true;
		}
	};

	for (auto& block : line.ParseTags()) {
		switch (block->GetType()) {
		case AssBlockType::OVERRIDE:
			process_tags(static_cast<AssDialogueBlockOverride&>(*block).Tags);
			break;
		case AssBlockType::PLAIN:
		case AssBlockType::DRAWING: {
			if (active_drawing_level != 0)
				break;
			auto text = TextBlockView(*block);

			if (text.empty() || !font_state.IsValid())
				continue;

			StyleInfo style;
			style.request = font_state.Request();
			style.event_style = event_style;
			if (callback(style, overriden, active_wrap_style, text))
				return true;
			break;
		}
		case AssBlockType::COMMENT:
			break;
		}
	}

	return true;
}

void FontCollector::AddCodepoint(UsageData& data, uint32_t codepoint) {
	if (codepoint < 256) {
		data.latin1_codepoints[codepoint / 64] |= uint64_t(1) << (codepoint % 64);
		return;
	}

	data.pending_codepoints.push_back(codepoint);
	if (data.pending_codepoints.size() >= PendingCodepointFlushThreshold)
		MergePendingCodepoints(data);
}

void FontCollector::AppendTextCodepoints(std::string_view text, int wrap_style, UsageData& data) {
	ForEachAssTextCodepoint(text, wrap_style, [&](uint32_t value) {
		AddCodepoint(data, value);
		return false;
	});
}

void FontCollector::MergePendingCodepoints(UsageData& data) {
	if (data.pending_codepoints.empty())
		return;

	auto& pending = data.pending_codepoints;
	sort(begin(pending), end(pending));
	pending.erase(unique(begin(pending), end(pending)), end(pending));

	if (data.codepoints.empty()) {
		data.codepoints.swap(pending);
		pending.clear();
		return;
	}

	std::vector<uint32_t> merged;
	merged.reserve(data.codepoints.size() + pending.size());
	std::set_union(begin(data.codepoints), end(data.codepoints), begin(pending), end(pending), std::back_inserter(merged));
	data.codepoints.swap(merged);
	pending.clear();
}

void FontCollector::FinalizeUsageCodepoints(UsageData& data) {
	MergePendingCodepoints(data);

	size_t latin1_count = 0;
	for (auto bits : data.latin1_codepoints) {
		while (bits) {
			latin1_count += bits & 1;
			bits >>= 1;
		}
	}

	if (!latin1_count)
		return;

	std::vector<uint32_t> merged;
	merged.reserve(latin1_count + data.codepoints.size());
	for (size_t word = 0; word < data.latin1_codepoints.size(); ++word) {
		auto bits = data.latin1_codepoints[word];
		for (size_t bit = 0; bits && bit < 64; ++bit) {
			if (bits & (uint64_t(1) << bit))
				merged.push_back(static_cast<uint32_t>(word * 64 + bit));
		}
	}

	merged.insert(merged.end(), data.codepoints.begin(), data.codepoints.end());
	data.codepoints.swap(merged);
	data.latin1_codepoints = {};
}

FontCollector::ResolvedUsage FontCollector::ResolveMergedUsage(StyleInfo const& style, std::vector<uint32_t> const& codepoints) {
	ResolvedUsage resolved;
	if (codepoints.empty())
		return resolved;

	auto& res = resolved.result;
	res = lister->GetFontPaths(style.request, codepoints);
	if (!res.backend_requested_weight)
		res.backend_requested_weight = style.request.effective_weight;
	if (!res.noncanonical_variant && res.matched_weight != 0 &&
	    res.matched_weight != 400 && res.matched_weight != 700)
		res.noncanonical_variant = true;
	if (res.realized_status && *res.realized_status != FontVariantStatus::Canonical)
		res.noncanonical_variant = true;
	for (auto& path : res.paths)
		path.make_preferred();

	resolved.missing_codepoints = DecodeUniqueUtf8Codepoints(res.missing);
	return resolved;
}

void FontCollector::ApplyResolvedFontUsage(FileAnalysis& analysis, StyleInfo const& style, UsageData& data,
                                           ResolvedUsage const& resolved, std::vector<MissingGlyphQuery>& missing_queries) {
	if (data.codepoints.empty()) return;

	auto const& request = style.request;
	auto const requested_weight = request.effective_weight;
	auto const& res = resolved.result;
	auto const implicit_variant_fallback = IsImplicitVariantFallback(request, res);
	auto missing_codepoints = IntersectSortedCodepoints(data.codepoints, resolved.missing_codepoints);
	auto missing_text = EncodeCodepointsToUtf8(missing_codepoints);

	if (analysis.details) {
		auto& usage = analysis.details->fonts.emplace_back();
		usage.ass_facename = request.family;
		usage.ass_bold = aegisub::ass::LegacyAssBoldArgument(request);
		usage.ass_italic = request.italic;
		usage.ass_effective_weight = request.effective_weight;
		usage.ass_charset = request.charset;
		usage.ass_height = request.height;
		usage.ass_raw_bold_tag = request.raw_bold_tag;
		usage.ass_raw_italic_tag = request.raw_italic_tag;
		usage.ass_raw_charset_tag = request.raw_charset_tag;
		usage.ass_raw_height_tag = request.raw_height_tag;
		usage.ass_has_explicit_family = request.has_explicit_family;
		usage.ass_has_explicit_bold = request.has_explicit_bold;
		usage.ass_has_explicit_italic = request.has_explicit_italic;
		usage.ass_has_explicit_charset = request.has_explicit_charset;
		usage.ass_has_explicit_height = request.has_explicit_height;
		usage.baseline_facename = style.event_style.family;
		usage.baseline_weight = style.event_style.weight;
		usage.baseline_italic = style.event_style.italic;
		usage.baseline_charset = style.event_style.charset;
		usage.baseline_height = style.event_style.height;
		usage.codepoints = data.codepoints;
		usage.styles = data.styles;
		usage.lines = data.lines;
		usage.override_lines = data.override_lines;
		usage.event_times = data.event_times;
		usage.matched = MatchedFont(res, implicit_variant_fallback, missing_text, missing_codepoints);
		if (res.glyphless_family) {
			auto const& family = *res.glyphless_family;
			auto glyphless = std::make_shared<FontCollectorMatchedFont>(
				MatchedFont(family, IsImplicitVariantFallback(request, family), missing_text, missing_codepoints));
			usage.matched.glyphless_family = glyphless;
			if (!missing_codepoints.empty()) {
				MissingGlyphQuery query;
				query.style = style;
				query.missing_codepoints = missing_codepoints;
				query.internal_target = std::move(glyphless);
				missing_queries.push_back(std::move(query));
			}
		}
	}

	auto make_event = [&](FontCollectorEventType type) {
		FontCollectorEvent event;
		event.type = type;
		event.face = request.family;
		event.requested_weight = requested_weight;
		event.requested_italic = request.italic ? 1 : 0;
		return event;
	};

	if (res.paths.empty() && res.memory_fonts.empty()) {
		Emit(analysis.event_sink, make_event(FontCollectorEventType::FontMissing));
		PrintUsage(analysis.event_sink, data);
		++analysis.missing;
	}
	else {
		for (auto& elem : res.paths) {
			if (std::find(begin(analysis.results), end(analysis.results), elem) == end(analysis.results)) {
				auto event = make_event(FontCollectorEventType::FontFound);
				event.path = elem;
				event.message = res.path_source;
				Emit(analysis.event_sink, std::move(event));
				analysis.results.push_back(elem);
			}
		}
		for (auto const& memory_font : res.memory_fonts) {
			if (!memory_font.data || memory_font.data->empty())
				continue;
			auto event = make_event(FontCollectorEventType::FontFound);
			event.message = "memory";
			Emit(analysis.event_sink, std::move(event));
		}

		if (res.fake_bold)
			Emit(analysis.event_sink, make_event(FontCollectorEventType::FakeBold));
		if (res.fake_italic)
			Emit(analysis.event_sink, make_event(FontCollectorEventType::FakeItalic));

		if (!missing_codepoints.empty()) {
			MissingGlyphQuery query;
			query.style = style;
			query.missing_codepoints = missing_codepoints;
			query.event = make_event(FontCollectorEventType::MissingGlyphs);
			query.event.message = missing_text;
			query.event.count = static_cast<int>(query.missing_codepoints.size());
			query.usage = &data;
			missing_queries.push_back(std::move(query));
			++analysis.missing_glyphs;
		}
		else if (res.fake_bold || res.fake_italic)
			PrintUsage(analysis.event_sink, data);
	}
}

void FontCollector::CollectMissingGlyphLines(AssFile const *file, int wrap_style, std::vector<MissingGlyphQuery>& queries) {
	if (queries.empty())
		return;

	std::map<StyleInfo, size_t> query_indices;
	for (size_t i = 0; i < queries.size(); ++i) {
		if (!queries[i].missing_codepoints.empty())
			query_indices.emplace(queries[i].style, i);
	}

	if (query_indices.empty())
		return;

	int index = 0;
	for (auto const& diag : file->Events) {
		++index;
		auto const source_line = SourceLineNumber(diag, index);
		if (diag.Comment)
			continue;

		ForEachLineTextSpan(*file, diag, source_line, wrap_style, [&](StyleInfo const& style, bool, int span_wrap_style, std::string_view text) {
			auto query_it = query_indices.find(style);
			if (query_it == end(query_indices))
				return false;

			auto& query = queries[query_it->second];
			if (TextContainsAnyCodepoint(text, span_wrap_style, query.missing_codepoints) &&
			    (query.matching_lines.empty() || query.matching_lines.back() != source_line))
				query.matching_lines.push_back(source_line);
			return false;
		}, nullptr);
	}
}

void FontCollector::StoreMissingGlyphLines(FontCollectorDetails *details, std::vector<MissingGlyphQuery> const& queries) {
	if (!details || queries.empty())
		return;

	for (auto const& query : queries) {
		if (query.internal_target) {
			query.internal_target->missing_lines = query.matching_lines;
			continue;
		}
		if (!query.usage)
			continue;

		for (auto& usage : details->fonts) {
			if (usage.ass_facename == query.style.request.family &&
			    usage.ass_effective_weight == query.style.request.effective_weight &&
			    usage.ass_italic == query.style.request.italic &&
			    usage.ass_charset == query.style.request.charset &&
			    usage.ass_height == query.style.request.height &&
			    usage.ass_raw_bold_tag == query.style.request.raw_bold_tag &&
			    usage.ass_raw_italic_tag == query.style.request.raw_italic_tag &&
			    usage.ass_raw_charset_tag == query.style.request.raw_charset_tag &&
			    usage.ass_raw_height_tag == query.style.request.raw_height_tag &&
			    usage.ass_has_explicit_family == query.style.request.has_explicit_family &&
			    usage.ass_has_explicit_bold == query.style.request.has_explicit_bold &&
			    usage.ass_has_explicit_italic == query.style.request.has_explicit_italic &&
			    usage.ass_has_explicit_charset == query.style.request.has_explicit_charset &&
			    usage.ass_has_explicit_height == query.style.request.has_explicit_height &&
			    usage.baseline_facename == query.style.event_style.family &&
			    usage.baseline_weight == query.style.event_style.weight &&
			    usage.baseline_italic == query.style.event_style.italic &&
			    usage.baseline_charset == query.style.event_style.charset &&
			    usage.baseline_height == query.style.event_style.height) {
				usage.matched.missing_lines = query.matching_lines;
				break;
			}
		}
	}
}

FontCollector::FileAnalysis FontCollector::AnalyzeFile(FontCollectorBatchSource const& source) {
	FileAnalysis analysis;
	analysis.file = source.file;
	analysis.event_sink = source.event_sink ? source.event_sink : event_sink;
	analysis.details = source.details;
	if (analysis.details)
		analysis.details->fonts.clear();

	FontCollectorEvent event;
	event.type = FontCollectorEventType::ParsingFile;
	Emit(analysis.event_sink, std::move(event));

	if (!analysis.file)
		return analysis;

	for (auto const& style : analysis.file->Styles) {
		StyleInfo info = MakeStyleInfo(style);
		analysis.used_styles[info].styles.push_back(style.name);
	}

	analysis.wrap_style = analysis.file->GetScriptInfoAsInt("WrapStyle");
	int index = 0;
	for (auto const& diag : analysis.file->Events) {
		++index;
		ProcessDialogueLine(analysis, &diag, SourceLineNumber(diag, index));
	}

	EmitMissingStyles(analysis);
	for (auto& style : analysis.used_styles)
		FinalizeUsageCodepoints(style.second);
	if (analysis.details)
		analysis.details->fonts.reserve(analysis.used_styles.size());

	return analysis;
}

void FontCollector::ProcessDialogueLine(FileAnalysis& analysis, const AssDialogue *line, int index) {
	if (line->Comment) return;

	ForEachLineTextSpan(*analysis.file, *line, index, analysis.wrap_style, [&](StyleInfo const& style, bool overriden, int span_wrap_style, std::string_view text) {
		bool const has_codepoint = ForEachAssTextCodepoint(text, span_wrap_style, [](uint32_t) { return true; });
		auto& usage = analysis.used_styles[style];
		auto const time = std::pair<int, int>(line->Start, line->End);
		if (has_codepoint && (usage.event_times.empty() || usage.event_times.back() != time))
			usage.event_times.push_back(time);
		if (usage.lines.empty() || usage.lines.back() != index)
			usage.lines.push_back(index);
		if (overriden) {
			auto& lines = usage.override_lines;
			if (lines.empty() || lines.back() != index)
				lines.push_back(index);
		}

		AppendTextCodepoints(text, span_wrap_style, usage);
		return false; }, &analysis.missing_style_lines);
}

void FontCollector::PrintUsage(FontCollectorEventSink const& sink, UsageData const& data) {
	PrintUsage(sink, data, data.lines);
}

void FontCollector::PrintUsage(FontCollectorEventSink const& sink, UsageData const& data, std::vector<int> const& lines) {
	FontCollectorEvent event;
	event.type = FontCollectorEventType::Usage;
	event.styles = data.styles;
	event.lines = lines;
	Emit(sink, std::move(event));
}

std::vector<agi::fs::path> FontCollector::GetFontPaths(const AssFile *file, FontCollectorDetails *details) {
	FontCollectorBatchSource source;
	source.file = file;
	source.event_sink = event_sink;
	source.details = details;
	auto results = GetFontPaths(std::vector<FontCollectorBatchSource>{source});
	return results.empty() ? std::vector<agi::fs::path>() : std::move(results.front());
}

std::vector<std::vector<agi::fs::path>> FontCollector::GetFontPaths(std::vector<FontCollectorBatchSource> const& sources) {
	std::vector<FileAnalysis> analyses;
	analyses.reserve(sources.size());
	for (auto const& source : sources)
		analyses.push_back(AnalyzeFile(source));

	for (auto& analysis : analyses) {
		FontCollectorEvent event;
		event.type = FontCollectorEventType::SearchingForFontFiles;
		Emit(analysis.event_sink, std::move(event));
	}

	struct MergedMatchUsage {
		StyleInfo representative;
		std::vector<uint32_t> codepoints;
	};

	std::map<StyleInfo, FontFileListerMatchKey> match_keys;
	std::map<FontFileListerMatchKey, MergedMatchUsage> merged_matches;
	for (auto const& analysis : analyses) {
		for (auto const& [style, data] : analysis.used_styles) {
			if (data.codepoints.empty())
				continue;

			auto [key_it, inserted] = match_keys.try_emplace(style);
			if (inserted)
				key_it->second = lister->GetMatchKey(style.request);

			auto [match_it, new_match] = merged_matches.try_emplace(key_it->second);
			if (new_match)
				match_it->second.representative = style;
			MergeSortedCodepointsInto(match_it->second.codepoints, data.codepoints);
		}
	}

	std::map<FontFileListerMatchKey, ResolvedUsage> resolved;
	for (auto const& [key, usage] : merged_matches) {
		if (!usage.codepoints.empty())
			resolved.emplace(key, ResolveMergedUsage(usage.representative, usage.codepoints));
	}

	std::vector<std::vector<agi::fs::path>> paths_by_file;
	paths_by_file.reserve(analyses.size());
	for (auto& analysis : analyses) {
		std::vector<MissingGlyphQuery> missing_queries;
		for (auto& [style, data] : analysis.used_styles) {
			auto key_it = match_keys.find(style);
			if (key_it == end(match_keys))
				continue;
			auto resolved_it = resolved.find(key_it->second);
			if (resolved_it != end(resolved))
				ApplyResolvedFontUsage(analysis, style, data, resolved_it->second, missing_queries);
		}

		if (analysis.file)
			CollectMissingGlyphLines(analysis.file, analysis.wrap_style, missing_queries);
		StoreMissingGlyphLines(analysis.details, missing_queries);
		for (auto const& query : missing_queries) {
			if (!query.usage)
				continue;
			Emit(analysis.event_sink, query.event);
			PrintUsage(analysis.event_sink, *query.usage, query.matching_lines);
		}

		FontCollectorEvent event;
		event.type = FontCollectorEventType::SearchComplete;
		Emit(analysis.event_sink, std::move(event));

		event = FontCollectorEvent();
		if (analysis.missing == 0) {
			event.type = FontCollectorEventType::AllFontsFound;
			Emit(analysis.event_sink, std::move(event));
		}
		else {
			event.type = FontCollectorEventType::FontsMissing;
			event.count = analysis.missing;
			Emit(analysis.event_sink, std::move(event));
		}
		if (analysis.missing_glyphs != 0) {
			event = FontCollectorEvent();
			event.type = FontCollectorEventType::FontsMissingGlyphs;
			event.count = analysis.missing_glyphs;
			Emit(analysis.event_sink, std::move(event));
		}

		paths_by_file.push_back(analysis.results);
	}

	return paths_by_file;
}

bool FontCollector::StyleInfo::operator<(StyleInfo const& rgt) const {
	return std::tie(
		request.family,
		request.effective_weight,
		request.italic,
		request.charset,
		request.height,
		request.raw_bold_tag,
		request.raw_italic_tag,
		request.raw_charset_tag,
		request.raw_height_tag,
		request.has_explicit_family,
		request.has_explicit_bold,
		request.has_explicit_italic,
		request.has_explicit_charset,
		request.has_explicit_height,
		event_style.family,
		event_style.weight,
		event_style.italic,
		event_style.charset,
		event_style.height) <
		std::tie(
			rgt.request.family,
			rgt.request.effective_weight,
			rgt.request.italic,
			rgt.request.charset,
			rgt.request.height,
			rgt.request.raw_bold_tag,
			rgt.request.raw_italic_tag,
			rgt.request.raw_charset_tag,
			rgt.request.raw_height_tag,
			rgt.request.has_explicit_family,
			rgt.request.has_explicit_bold,
			rgt.request.has_explicit_italic,
			rgt.request.has_explicit_charset,
			rgt.request.has_explicit_height,
			rgt.event_style.family,
			rgt.event_style.weight,
			rgt.event_style.italic,
			rgt.event_style.charset,
			rgt.event_style.height);
}
