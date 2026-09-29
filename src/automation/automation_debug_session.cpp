// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "automation_debug_session.h"

#include "automation_json_utils.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace Automation4 {
namespace {
using namespace Automation4::json;

AutomationDebugVariable MakeScalarVariable(std::string name, std::string value, std::string type = "string")
{
	return { std::move(name), std::move(value), std::move(type) };
}

AutomationDebugVariable MakeIntArrayVariable(std::string name, std::vector<int> const& values)
{
	AutomationDebugVariable variable;
	variable.name = std::move(name);
	variable.value = "table[" + std::to_string(values.size()) + "]";
	variable.value_type = "table";
	for (size_t i = 0; i < values.size(); ++i)
		variable.children.push_back(MakeScalarVariable("[" + std::to_string(i + 1) + "]", std::to_string(values[i]), "integer"));
	return variable;
}

AutomationDebugVariable MakePreviewIntArrayVariable(std::string name, std::vector<int> const& values, size_t max_items = 16)
{
	AutomationDebugVariable variable;
	variable.name = std::move(name);
	variable.value = "table[" + std::to_string(values.size()) + "]";
	variable.value_type = "table";
	size_t const limit = std::min(values.size(), max_items);
	for (size_t i = 0; i < limit; ++i)
		variable.children.push_back(MakeScalarVariable("[" + std::to_string(i + 1) + "]", std::to_string(values[i]), "integer"));
	if (values.size() > limit)
		variable.children.push_back(MakeScalarVariable("...", std::to_string(values.size() - limit) + " more item(s)", "summary"));
	return variable;
}

AutomationDebugVariable MakeStringArrayVariable(std::string name, std::vector<std::string> const& values, size_t max_items = 16)
{
	AutomationDebugVariable variable;
	variable.name = std::move(name);
	variable.value = "table[" + std::to_string(values.size()) + "]";
	variable.value_type = "table";
	size_t const limit = std::min(values.size(), max_items);
	for (size_t i = 0; i < limit; ++i)
		variable.children.push_back(MakeScalarVariable("[" + std::to_string(i + 1) + "]", values[i], "string"));
	if (values.size() > limit)
		variable.children.push_back(MakeScalarVariable("...", std::to_string(values.size() - limit) + " more item(s)", "summary"));
	return variable;
}

void AddChild(AutomationDebugVariable& parent, std::string name, std::string value, std::string type = "string")
{
	parent.children.push_back(MakeScalarVariable(std::move(name), std::move(value), std::move(type)));
}

void AddOptionalStringChild(AutomationDebugVariable& parent, std::string const& name, std::optional<std::string> const& value)
{
	if (value && !value->empty())
		AddChild(parent, name, *value);
}

void AddOptionalIntChild(AutomationDebugVariable& parent, std::string const& name, std::optional<int> const& value)
{
	if (value)
		AddChild(parent, name, std::to_string(*value), "integer");
}

void AddOptionalBoolChild(AutomationDebugVariable& parent, std::string const& name, std::optional<bool> const& value)
{
	if (value)
		AddChild(parent, name, *value ? "true" : "false", "boolean");
}

void AddOptionalDoubleChild(AutomationDebugVariable& parent, std::string const& name, std::optional<double> const& value)
{
	if (value)
		AddChild(parent, name, std::to_string(*value), "number");
}

std::string BuildTimeRangeSummary(std::optional<int> const& start_time, std::optional<int> const& end_time)
{
	if (!start_time && !end_time)
		return {};
	std::ostringstream out;
	out << (start_time ? std::to_string(*start_time) : "?") << ".." << (end_time ? std::to_string(*end_time) : "?") << "ms";
	return out.str();
}

AutomationDebugVariable MakeTemplateLineVariable(char const* name, AutomationTemplateLineSnapshot const& snapshot)
{
	AutomationDebugVariable variable;
	variable.name = name;
	variable.value = snapshot.text.value_or(snapshot.line_class.value_or("line"));
	variable.value_type = "template-line";
	AddOptionalIntChild(variable, "index", snapshot.index);
	AddOptionalStringChild(variable, "class", snapshot.line_class);
	AddOptionalIntChild(variable, "layer", snapshot.layer);
	AddOptionalStringChild(variable, "style", snapshot.style);
	AddOptionalStringChild(variable, "actor", snapshot.actor);
	AddOptionalStringChild(variable, "effect", snapshot.effect);
	AddOptionalBoolChild(variable, "comment", snapshot.comment);
	AddOptionalStringChild(variable, "text", snapshot.text);
	auto time_range = BuildTimeRangeSummary(snapshot.start_time, snapshot.end_time);
	if (!time_range.empty())
		AddChild(variable, "time_range", time_range);
	AddOptionalIntChild(variable, "start_time", snapshot.start_time);
	AddOptionalIntChild(variable, "end_time", snapshot.end_time);
	return variable;
}

AutomationDebugVariable MakeTemplateSyllableVariable(char const* name, AutomationTemplateSyllableSnapshot const& snapshot)
{
	AutomationDebugVariable variable;
	variable.name = name;
	variable.value = snapshot.text.value_or("syllable");
	variable.value_type = "template-syllable";
	AddOptionalIntChild(variable, "index", snapshot.index);
	AddOptionalStringChild(variable, "text", snapshot.text);
	AddOptionalStringChild(variable, "text_stripped", snapshot.text_stripped);
	AddOptionalStringChild(variable, "inline_fx", snapshot.inline_fx);
	auto time_range = BuildTimeRangeSummary(snapshot.start_time, snapshot.end_time);
	if (!time_range.empty())
		AddChild(variable, "time_range", time_range);
	AddOptionalIntChild(variable, "start_time", snapshot.start_time);
	AddOptionalIntChild(variable, "end_time", snapshot.end_time);
	AddOptionalIntChild(variable, "duration", snapshot.duration);
	AddOptionalBoolChild(variable, "is_furi", snapshot.is_furi);
	AddOptionalDoubleChild(variable, "left", snapshot.left);
	AddOptionalDoubleChild(variable, "center", snapshot.center);
	AddOptionalDoubleChild(variable, "right", snapshot.right);
	AddOptionalDoubleChild(variable, "width", snapshot.width);
	AddOptionalDoubleChild(variable, "height", snapshot.height);
	return variable;
}

AutomationDebugVariable MakeTemplateHighlightVariable(char const* name, AutomationTemplateHighlightSnapshot const& snapshot)
{
	AutomationDebugVariable variable;
	variable.name = name;
	variable.value = BuildTimeRangeSummary(snapshot.start_time, snapshot.end_time);
	if (variable.value.empty())
		variable.value = "highlight";
	variable.value_type = "template-highlight";
	AddOptionalIntChild(variable, "index", snapshot.index);
	AddOptionalIntChild(variable, "start_time", snapshot.start_time);
	AddOptionalIntChild(variable, "end_time", snapshot.end_time);
	AddOptionalIntChild(variable, "duration", snapshot.duration);
	return variable;
}

AutomationDebugVariable MakeTemplateCharVariable(char const* name, AutomationTemplateCharSnapshot const& snapshot)
{
	AutomationDebugVariable variable;
	variable.name = name;
	variable.value = snapshot.text.value_or("char");
	variable.value_type = "template-char";
	AddOptionalIntChild(variable, "index", snapshot.index);
	AddOptionalStringChild(variable, "text", snapshot.text);
	return variable;
}

AutomationDebugVariable MakeTemplateIdentityVariable(AutomationTemplateIdentity const& identity)
{
	AutomationDebugVariable variable;
	variable.name = "identity";
	variable.value = identity.template_kind.value_or("template");
	variable.value_type = "template-identity";
	AddOptionalStringChild(variable, "owner_script", identity.owner_script);
	AddOptionalIntChild(variable, "template_debug_id", identity.template_debug_id);
	AddOptionalStringChild(variable, "template_kind", identity.template_kind);
	if (!identity.template_kinds.empty())
		variable.children.push_back(MakeStringArrayVariable("template_kinds", identity.template_kinds));
	AddOptionalStringChild(variable, "fragment_kind", identity.fragment_kind);
	AddOptionalStringChild(variable, "template_id", identity.template_id);
	AddOptionalIntChild(variable, "source_line_index", identity.source_line_index);
	if (!identity.source_line_indices.empty())
		variable.children.push_back(MakeIntArrayVariable("source_line_indices", identity.source_line_indices));
	return variable;
}

AutomationDebugVariable MakeTemplateSourceVariable(AutomationTemplateSource const& source)
{
	AutomationDebugVariable variable;
	variable.name = "source";
	variable.value = source.text.value_or(source.style.value_or("template-source"));
	variable.value_type = "template-source";
	AddOptionalStringChild(variable, "style", source.style);
	AddOptionalStringChild(variable, "effect", source.effect);
	AddOptionalStringChild(variable, "text", source.text);
	if (!source.fragments.empty()) {
		AutomationDebugVariable fragments;
		fragments.name = "fragments";
		fragments.value = "table[" + std::to_string(source.fragments.size()) + "]";
		fragments.value_type = "table";
		for (size_t i = 0; i < source.fragments.size(); ++i) {
			AutomationDebugVariable fragment;
			fragment.name = "[" + std::to_string(i + 1) + "]";
			fragment.value = source.fragments[i].fragment_kind.value_or("fragment");
			fragment.value_type = "template-fragment";
			AddOptionalIntChild(fragment, "source_line_index", source.fragments[i].source_line_index);
			AddOptionalStringChild(fragment, "fragment_kind", source.fragments[i].fragment_kind);
			AddOptionalStringChild(fragment, "text", source.fragments[i].text);
			AddOptionalStringChild(fragment, "effect", source.fragments[i].effect);
			fragments.children.push_back(std::move(fragment));
		}
		variable.children.push_back(std::move(fragments));
	}
	return variable;
}

AutomationDebugVariable MakeTemplateTargetVariable(AutomationTemplateTargetSnapshot const& target)
{
	AutomationDebugVariable variable;
	variable.name = "target";
	variable.value = target.scope_kind.value_or("template-target");
	variable.value_type = "template-target";
	AddOptionalStringChild(variable, "scope_kind", target.scope_kind);
	if (target.original_line)
		variable.children.push_back(MakeTemplateLineVariable("original_line", *target.original_line));
	if (target.line)
		variable.children.push_back(MakeTemplateLineVariable("line", *target.line));
	if (target.syllable)
		variable.children.push_back(MakeTemplateSyllableVariable("syllable", *target.syllable));
	if (target.base_syllable)
		variable.children.push_back(MakeTemplateSyllableVariable("base_syllable", *target.base_syllable));
	if (target.highlight)
		variable.children.push_back(MakeTemplateHighlightVariable("highlight", *target.highlight));
	if (target.character)
		variable.children.push_back(MakeTemplateCharVariable("character", *target.character));
	return variable;
}

AutomationDebugVariable MakeGeneratedLineVariable(char const* name, AutomationGeneratedLineSnapshot const& snapshot)
{
	AutomationDebugVariable variable;
	variable.name = name;
	variable.value = snapshot.text.value_or("generated-line");
	variable.value_type = "generated-line";
	AddOptionalIntChild(variable, "generated_index", snapshot.generated_index);
	AddOptionalStringChild(variable, "text", snapshot.text);
	AddOptionalStringChild(variable, "style", snapshot.style);
	AddOptionalIntChild(variable, "layer", snapshot.layer);
	AddOptionalStringChild(variable, "effect", snapshot.effect);
	auto time_range = BuildTimeRangeSummary(snapshot.start_time, snapshot.end_time);
	if (!time_range.empty())
		AddChild(variable, "time_range", time_range);
	AddOptionalIntChild(variable, "start_time", snapshot.start_time);
	AddOptionalIntChild(variable, "end_time", snapshot.end_time);
	AddOptionalIntChild(variable, "source_line_index", snapshot.source_line_index);
	AddOptionalIntChild(variable, "template_debug_id", snapshot.template_debug_id);
	AddOptionalStringChild(variable, "template_kind", snapshot.template_kind);
	AddOptionalStringChild(variable, "scope_kind", snapshot.scope_kind);
	AddOptionalIntChild(variable, "syllable_index", snapshot.syllable_index);
	AddOptionalIntChild(variable, "highlight_index", snapshot.highlight_index);
	AddOptionalIntChild(variable, "char_index", snapshot.char_index);
	return variable;
}

std::string BuildVideoSummary(AutomationRuntimeStateSnapshot const& runtime_snapshot)
{
	std::ostringstream out;
	if (!runtime_snapshot.context_snapshot.media.has_video)
		return "unloaded";
	out << runtime_snapshot.context_snapshot.media.video_width
		<< "x" << runtime_snapshot.context_snapshot.media.video_height
		<< " frame " << runtime_snapshot.context_snapshot.project.video_position;
	return out.str();
}

std::string BuildAudioSummary(AutomationRuntimeStateSnapshot const& runtime_snapshot)
{
	if (!runtime_snapshot.context_snapshot.media.has_audio)
		return "unloaded";
	if (!runtime_snapshot.context_snapshot.media.has_audio_selection)
		return "loaded";

	std::ostringstream out;
	out << runtime_snapshot.context_snapshot.media.audio_selection_begin
		<< ".." << runtime_snapshot.context_snapshot.media.audio_selection_end << "ms";
	return out.str();
}

std::vector<AutomationDebugScope> BuildScopes(std::optional<AutomationRuntimeStateSnapshot> const& runtime_snapshot)
{
	if (!runtime_snapshot)
		return {};

	std::vector<AutomationDebugScope> scopes;
	auto const& invocation = runtime_snapshot->invocation;
	auto const& selection = runtime_snapshot->context_snapshot.selection;
	auto const& media = runtime_snapshot->context_snapshot.media;
	auto const& project = runtime_snapshot->context_snapshot.project;

	AutomationDebugScope invocation_scope;
	invocation_scope.name = "Invocation";
	invocation_scope.variables.push_back(MakeScalarVariable("kind", ToString(invocation.kind)));
	invocation_scope.variables.push_back(MakeScalarVariable("feature_name", invocation.feature_name));
	invocation_scope.variables.push_back(MakeScalarVariable("has_project_context", runtime_snapshot->context_snapshot.has_project_context ? "true" : "false", "boolean"));
	invocation_scope.variables.push_back(MakeScalarVariable("allow_modify", invocation.capabilities.allow_modify ? "true" : "false", "boolean"));
	invocation_scope.variables.push_back(MakeScalarVariable("allow_undo", invocation.capabilities.allow_undo ? "true" : "false", "boolean"));
	invocation_scope.variables.push_back(MakeScalarVariable("allow_dialog", invocation.capabilities.allow_dialog ? "true" : "false", "boolean"));
	scopes.push_back(std::move(invocation_scope));

	AutomationDebugScope selection_scope;
	selection_scope.name = "Selection";
	selection_scope.variables.push_back(MakeScalarVariable("active_row", std::to_string(selection.active_row), "integer"));
	selection_scope.variables.push_back(MakeScalarVariable("selected_count", std::to_string(selection.selected_rows.size()), "integer"));
	selection_scope.variables.push_back(MakeIntArrayVariable("selected_rows", selection.selected_rows));
	scopes.push_back(std::move(selection_scope));

	AutomationDebugScope subtitles_scope;
	subtitles_scope.name = "Subtitles";
	int const line_count = project.info_count + project.style_count + project.event_count;
	subtitles_scope.variables.push_back(MakeScalarVariable("script_filename", project.script_filename.empty() ? "(unsaved)" : project.script_filename));
	subtitles_scope.variables.push_back(MakeScalarVariable("subtitle_file", project.subtitle_file));
	subtitles_scope.variables.push_back(MakeScalarVariable("active_row", std::to_string(project.active_row), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("line_count", std::to_string(line_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("info_count", std::to_string(project.info_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("style_count", std::to_string(project.style_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("event_count", std::to_string(project.event_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("dialogue_count", std::to_string(project.dialogue_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("comment_count", std::to_string(project.comment_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("attachment_count", std::to_string(project.attachment_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("extradata_count", std::to_string(project.extradata_count), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("play_res_x", std::to_string(project.play_res_x), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("play_res_y", std::to_string(project.play_res_y), "integer"));
	subtitles_scope.variables.push_back(MakeScalarVariable("automation_scripts", project.automation_scripts));
	subtitles_scope.variables.push_back(MakeScalarVariable("export_filters", project.export_filters));
	subtitles_scope.variables.push_back(MakeScalarVariable("export_encoding", project.export_encoding));
	subtitles_scope.variables.push_back(MakeScalarVariable("style_storage", project.style_storage));
	subtitles_scope.variables.push_back(MakeScalarVariable("scroll_position", std::to_string(project.scroll_position), "integer"));
	scopes.push_back(std::move(subtitles_scope));

	AutomationDebugScope misc_scope;
	misc_scope.name = "Miscellaneous APIs";

	AutomationDebugVariable video_api;
	video_api.name = "video";
	video_api.value = BuildVideoSummary(*runtime_snapshot);
	video_api.value_type = "video";
	AddChild(video_api, "has_video", media.has_video ? "true" : "false", "boolean");
	AddChild(video_api, "video_position", std::to_string(project.video_position), "integer");
	AddChild(video_api, "video_width", std::to_string(media.video_width), "integer");
	AddChild(video_api, "video_height", std::to_string(media.video_height), "integer");
	AddChild(video_api, "video_aspect_ratio", std::to_string(media.video_aspect_ratio), "number");
	AddChild(video_api, "video_aspect_ratio_type", std::to_string(media.video_aspect_ratio_type), "integer");
	AddChild(video_api, "video_zoom", std::to_string(project.video_zoom), "number");
	AddChild(video_api, "ar_mode", std::to_string(project.ar_mode), "integer");
	AddChild(video_api, "ar_value", std::to_string(project.ar_value), "number");
	AddChild(video_api, "has_timecodes", media.has_timecodes ? "true" : "false", "boolean");
	AddChild(video_api, "has_keyframes", media.has_keyframes ? "true" : "false", "boolean");
	AddChild(video_api, "keyframe_count", std::to_string(media.keyframes.size()), "integer");
	if (!media.keyframes.empty())
		video_api.children.push_back(MakePreviewIntArrayVariable("keyframes", media.keyframes));
	misc_scope.variables.push_back(std::move(video_api));

	AutomationDebugVariable audio_api;
	audio_api.name = "audio";
	audio_api.value = BuildAudioSummary(*runtime_snapshot);
	audio_api.value_type = "audio";
	AddChild(audio_api, "has_audio", media.has_audio ? "true" : "false", "boolean");
	AddChild(audio_api, "has_audio_selection", media.has_audio_selection ? "true" : "false", "boolean");
	AddChild(audio_api, "audio_selection_begin", std::to_string(media.audio_selection_begin), "integer");
	AddChild(audio_api, "audio_selection_end", std::to_string(media.audio_selection_end), "integer");
	misc_scope.variables.push_back(std::move(audio_api));

	AutomationDebugVariable files_api;
	files_api.name = "files";
	files_api.value = project.subtitle_file.empty() ? "(unsaved)" : project.subtitle_file;
	files_api.value_type = "files";
	AddChild(files_api, "subtitle_file", project.subtitle_file);
	AddChild(files_api, "audio_file", project.audio_file);
	AddChild(files_api, "video_file", project.video_file);
	AddChild(files_api, "timecodes_file", project.timecodes_file);
	AddChild(files_api, "keyframes_file", project.keyframes_file);
	misc_scope.variables.push_back(std::move(files_api));

	scopes.push_back(std::move(misc_scope));

	if (runtime_snapshot->template_debug) {
		AutomationDebugScope template_scope;
		template_scope.name = "Template";
		if (runtime_snapshot->template_debug->kind)
			template_scope.variables.push_back(MakeScalarVariable("kind", *runtime_snapshot->template_debug->kind));
		if (runtime_snapshot->template_debug->phase)
			template_scope.variables.push_back(MakeScalarVariable("phase", *runtime_snapshot->template_debug->phase));
		if (runtime_snapshot->template_debug->scope_kind)
			template_scope.variables.push_back(MakeScalarVariable("scope_kind", *runtime_snapshot->template_debug->scope_kind));
		if (runtime_snapshot->template_debug->expression)
			template_scope.variables.push_back(MakeScalarVariable("expression", *runtime_snapshot->template_debug->expression));
		if (runtime_snapshot->template_debug->parse_error)
			template_scope.variables.push_back(MakeScalarVariable("parse_error", *runtime_snapshot->template_debug->parse_error));
		if (runtime_snapshot->template_debug->runtime_error)
			template_scope.variables.push_back(MakeScalarVariable("runtime_error", *runtime_snapshot->template_debug->runtime_error));
		if (runtime_snapshot->template_debug->loop_index)
			template_scope.variables.push_back(MakeScalarVariable("loop_index", std::to_string(*runtime_snapshot->template_debug->loop_index), "integer"));
		if (runtime_snapshot->template_debug->loop_count)
			template_scope.variables.push_back(MakeScalarVariable("loop_count", std::to_string(*runtime_snapshot->template_debug->loop_count), "integer"));
		if (runtime_snapshot->template_debug->syllable_index)
			template_scope.variables.push_back(MakeScalarVariable("syllable_index", std::to_string(*runtime_snapshot->template_debug->syllable_index), "integer"));
		if (runtime_snapshot->template_debug->highlight_index)
			template_scope.variables.push_back(MakeScalarVariable("highlight_index", std::to_string(*runtime_snapshot->template_debug->highlight_index), "integer"));
		if (runtime_snapshot->template_debug->char_index)
			template_scope.variables.push_back(MakeScalarVariable("char_index", std::to_string(*runtime_snapshot->template_debug->char_index), "integer"));
		if (runtime_snapshot->template_debug->line_text)
			template_scope.variables.push_back(MakeScalarVariable("line_text", *runtime_snapshot->template_debug->line_text));
		if (runtime_snapshot->template_debug->syllable_text)
			template_scope.variables.push_back(MakeScalarVariable("syllable_text", *runtime_snapshot->template_debug->syllable_text));
		scopes.push_back(std::move(template_scope));

		if (runtime_snapshot->template_debug->identity) {
			AutomationDebugScope identity_scope;
			identity_scope.name = "Template Identity";
			identity_scope.variables.push_back(MakeTemplateIdentityVariable(*runtime_snapshot->template_debug->identity));
			scopes.push_back(std::move(identity_scope));
		}

		if (runtime_snapshot->template_debug->source) {
			AutomationDebugScope source_scope;
			source_scope.name = "Template Source";
			source_scope.variables.push_back(MakeTemplateSourceVariable(*runtime_snapshot->template_debug->source));
			scopes.push_back(std::move(source_scope));
		}

		if (runtime_snapshot->template_debug->target) {
			AutomationDebugScope target_scope;
			target_scope.name = "Template Target";
			target_scope.variables.push_back(MakeTemplateTargetVariable(*runtime_snapshot->template_debug->target));
			scopes.push_back(std::move(target_scope));
		}

		if (runtime_snapshot->template_debug->generated) {
			AutomationDebugScope generated_scope;
			generated_scope.name = "Generated Lines";
			if (runtime_snapshot->template_debug->generated->count)
				generated_scope.variables.push_back(MakeScalarVariable("count", std::to_string(*runtime_snapshot->template_debug->generated->count), "integer"));
			if (runtime_snapshot->template_debug->generated->last_line)
				generated_scope.variables.push_back(MakeGeneratedLineVariable("last_line", *runtime_snapshot->template_debug->generated->last_line));
			scopes.push_back(std::move(generated_scope));
		}
	}

	return scopes;
}

std::string SerializeLocation(AutomationDebugLocation const& location)
{
	JsonObjectBuilder builder;
	builder.AddRaw("source_path", JsonString(location.source_path));
	builder.AddRaw("source_kind", JsonString(location.source_kind));
	builder.AddRaw("display_name", JsonString(location.display_name));
	builder.AddRaw("line", JsonInteger(location.line));
	builder.AddRaw("column", JsonInteger(location.column));
	return builder.Build();
}

std::string SerializeVariable(AutomationDebugVariable const& variable)
{
	JsonObjectBuilder builder;
	builder.AddRaw("name", JsonString(variable.name));
	builder.AddRaw("value", JsonString(variable.value));
	builder.AddRaw("value_type", JsonString(variable.value_type));
	builder.AddRaw("children", SerializeObjectArray(variable.children, SerializeVariable));
	return builder.Build();
}

std::string SerializeScope(AutomationDebugScope const& scope)
{
	JsonObjectBuilder builder;
	builder.AddRaw("name", JsonString(scope.name));
	builder.AddRaw("variables", SerializeObjectArray(scope.variables, SerializeVariable));
	return builder.Build();
}

std::string SerializeFrame(AutomationDebugFrame const& frame)
{
	JsonObjectBuilder builder;
	builder.AddRaw("level", JsonInteger(frame.level));
	builder.AddRaw("kind", JsonString(frame.kind));
	builder.AddRaw("function_name", JsonString(frame.function_name));
	builder.AddRaw("location", SerializeLocation(frame.location));
	builder.AddRaw("locals", SerializeObjectArray(frame.locals, SerializeVariable));
	builder.AddRaw("upvalues", SerializeObjectArray(frame.upvalues, SerializeVariable));
	return builder.Build();
}

std::string SerializeRuntimeSnapshotSummary(AutomationRuntimeStateSnapshot const& snapshot)
{
	JsonObjectBuilder builder;
	builder.AddRaw("invocation_kind", JsonString(ToString(snapshot.invocation.kind)));
	builder.AddRaw("feature_name", JsonString(snapshot.invocation.feature_name));
	builder.AddRaw("active_row", JsonInteger(snapshot.context_snapshot.selection.active_row));
	builder.AddRaw("selected_rows", SerializeIntArray(snapshot.context_snapshot.selection.selected_rows));
	builder.AddRaw("script_filename", JsonString(snapshot.context_snapshot.project.script_filename));
	builder.AddRaw("project_active_row", JsonInteger(snapshot.context_snapshot.project.active_row));
	builder.AddRaw("video_position", JsonInteger(snapshot.context_snapshot.project.video_position));
	builder.AddRaw("has_video", JsonBool(snapshot.context_snapshot.media.has_video));
	builder.AddRaw("has_keyframes", JsonBool(snapshot.context_snapshot.media.has_keyframes));
	if (snapshot.template_debug) {
		JsonObjectBuilder template_builder;
		AddOptional(template_builder, "kind", snapshot.template_debug->kind, JsonString);
		AddOptional(template_builder, "phase", snapshot.template_debug->phase, JsonString);
		AddOptional(template_builder, "scope_kind", snapshot.template_debug->scope_kind, JsonString);
		AddOptional(template_builder, "loop_index", snapshot.template_debug->loop_index, JsonInteger<int>);
		AddOptional(template_builder, "loop_count", snapshot.template_debug->loop_count, JsonInteger<int>);
		AddOptional(template_builder, "syllable_index", snapshot.template_debug->syllable_index, JsonInteger<int>);
		AddOptional(template_builder, "highlight_index", snapshot.template_debug->highlight_index, JsonInteger<int>);
		AddOptional(template_builder, "char_index", snapshot.template_debug->char_index, JsonInteger<int>);
		if (snapshot.template_debug->identity) {
			auto const& identity = *snapshot.template_debug->identity;
			if (identity.template_debug_id)
				template_builder.AddRaw("template_debug_id", JsonInteger(*identity.template_debug_id));
			if (identity.template_kind)
				template_builder.AddRaw("template_kind", JsonString(*identity.template_kind));
			if (identity.source_line_index)
				template_builder.AddRaw("source_line_index", JsonInteger(*identity.source_line_index));
		}
		builder.AddRaw("template_debug", template_builder.Build());
	}
	else {
		builder.AddRaw("template_debug", JsonNull());
	}
	return builder.Build();
}

std::string SerializePauseRecord(AutomationDebugPauseRecord const& record)
{
	JsonObjectBuilder builder;
	builder.AddRaw("sequence", JsonInteger(record.sequence));
	builder.AddRaw("thread_id", JsonInteger(record.thread_id));
	builder.AddRaw("stack_depth", JsonInteger(record.stack_depth));
	builder.AddRaw("reason", JsonString(ToString(record.reason)));
	builder.AddRaw("location", SerializeLocation(record.location));
	builder.AddRaw("frames", SerializeObjectArray(record.frames, SerializeFrame));
	builder.AddRaw("scopes", SerializeObjectArray(record.scopes, SerializeScope));
	if (record.runtime_snapshot)
		builder.AddRaw("runtime", SerializeRuntimeSnapshotSummary(*record.runtime_snapshot));
	else
		builder.AddRaw("runtime", JsonNull());
	return builder.Build();
}

}

std::string ToString(AutomationDebugPauseReason reason)
{
	switch (reason) {
	case AutomationDebugPauseReason::Entry:
		return "entry";
	case AutomationDebugPauseReason::Breakpoint:
		return "breakpoint";
	case AutomationDebugPauseReason::Step:
		return "step";
	case AutomationDebugPauseReason::Pause:
		return "pause";
	}
	return "breakpoint";
}

std::string ToString(AutomationDebugSessionState state)
{
	switch (state) {
	case AutomationDebugSessionState::Created:
		return "created";
	case AutomationDebugSessionState::Prepared:
		return "prepared";
	case AutomationDebugSessionState::Running:
		return "running";
	case AutomationDebugSessionState::Paused:
		return "paused";
	case AutomationDebugSessionState::Completed:
		return "completed";
	case AutomationDebugSessionState::Detached:
		return "detached";
	}
	return "created";
}

AutomationDebugSession::AutomationDebugSession(AutomationDebugLaunchRequest request)
: request(std::move(request))
{
	breakpoints.SetBreakpoints(this->request.breakpoints);
	if (this->request.enabled)
		state = AutomationDebugSessionState::Prepared;
}

bool AutomationDebugSession::Enabled() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return request.enabled && attached;
}

void AutomationDebugSession::BumpStateVersion()
{
	++state_version;
	cv.notify_all();
}

void AutomationDebugSession::UpdateStateLocked(AutomationDebugSessionState next_state)
{
	if (state == next_state)
		return;
	state = next_state;
	BumpStateVersion();
}

void AutomationDebugSession::SetTarget(AutomationDebugTarget target)
{
	std::lock_guard<std::mutex> lock(mutex);
	this->target = std::move(target);
	BumpStateVersion();
}

AutomationDebugTarget AutomationDebugSession::GetTarget() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return target;
}

void AutomationDebugSession::SetSourceRegistry(std::shared_ptr<LuaWorkspaceSourceRegistry> registry)
{
	std::scoped_lock lock(mutex);
	source_registry = std::move(registry);
}

std::shared_ptr<LuaWorkspaceSourceRegistry> AutomationDebugSession::GetSourceRegistry() const
{
	std::scoped_lock lock(mutex);
	return source_registry;
}

void AutomationDebugSession::SetBreakpoints(std::vector<AutomationDebugBreakpoint> values)
{
	std::lock_guard<std::mutex> lock(mutex);
	breakpoints.SetBreakpoints(std::move(values));
	BumpStateVersion();
}

std::vector<AutomationDebugBreakpoint> AutomationDebugSession::GetBreakpoints() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return breakpoints.GetBreakpoints();
}

void AutomationDebugSession::BeginInvocation(AutomationInvocation const&)
{
	std::lock_guard<std::mutex> lock(mutex);
	invocation_active = true;
	entry_pause_pending = request.stop_on_entry;
	pending_step_pauses = 0;
	step_mode = StepMode::None;
	step_depth = 0;
	pause_requested = false;
	pause_command_queued = false;
	resume_action = AutomationDebugResumeAction::Continue;
	UpdateStateLocked(AutomationDebugSessionState::Running);
}

void AutomationDebugSession::EndInvocation()
{
	std::lock_guard<std::mutex> lock(mutex);
	invocation_active = false;
	entry_pause_pending = false;
	pending_step_pauses = 0;
	step_mode = StepMode::None;
	step_depth = 0;
	pause_requested = false;
	pause_command_queued = false;
	current_pause.reset();
	if (attached)
		UpdateStateLocked(AutomationDebugSessionState::Prepared);
	else
		UpdateStateLocked(AutomationDebugSessionState::Detached);
}

void AutomationDebugSession::MarkCompleted(int exit_code, std::string message)
{
	std::lock_guard<std::mutex> lock(mutex);
	completion_exit_code = exit_code;
	completion_message = std::move(message);
	invocation_active = false;
	entry_pause_pending = false;
	pending_step_pauses = 0;
	step_mode = StepMode::None;
	step_depth = 0;
	pause_requested = false;
	pause_command_queued = false;
	current_pause.reset();
	UpdateStateLocked(attached ? AutomationDebugSessionState::Completed : AutomationDebugSessionState::Detached);
}

void AutomationDebugSession::RequestPause()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!attached || state == AutomationDebugSessionState::Completed || state == AutomationDebugSessionState::Detached)
		return;
	pause_requested = true;
	BumpStateVersion();
}

void AutomationDebugSession::Detach()
{
	std::lock_guard<std::mutex> lock(mutex);
	attached = false;
	pause_requested = false;
	pause_command_queued = true;
	resume_action = AutomationDebugResumeAction::Detach;
	if (state != AutomationDebugSessionState::Completed)
		UpdateStateLocked(AutomationDebugSessionState::Detached);
	else
		cv.notify_all();
}

bool AutomationDebugSession::Resume(AutomationDebugResumeAction action)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!current_pause || state != AutomationDebugSessionState::Paused)
		return false;

	switch (action) {
	case AutomationDebugResumeAction::Continue:
		step_mode = StepMode::None;
		step_depth = 0;
		attached = true;
		break;
	case AutomationDebugResumeAction::StepIn:
		step_mode = StepMode::Into;
		step_depth = current_pause->stack_depth;
		attached = true;
		break;
	case AutomationDebugResumeAction::Next:
		step_mode = StepMode::Over;
		step_depth = current_pause->stack_depth;
		attached = true;
		break;
	case AutomationDebugResumeAction::StepOut:
		step_mode = StepMode::Out;
		step_depth = current_pause->stack_depth;
		attached = true;
		break;
	case AutomationDebugResumeAction::Detach:
		step_mode = StepMode::None;
		step_depth = 0;
		attached = false;
		break;
	}

	pause_requested = false;
	step_thread_id = current_pause->thread_id;
	step_location = current_pause->location;
	step_over_nested_call = false;
	pause_command_queued = true;
	resume_action = action;
	cv.notify_all();
	return true;
}

AutomationDebugStateSnapshot AutomationDebugSession::GetStateSnapshot() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return {
		state_version,
		state,
		invocation_active,
		attached,
		pause_requested,
		target,
		current_pause,
		completion_message,
		completion_exit_code
	};
}

AutomationDebugStateSnapshot AutomationDebugSession::WaitForStateChange(size_t after_version) const
{
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait(lock, [&] {
		return state_version != after_version;
	});
	return {
		state_version,
		state,
		invocation_active,
		attached,
		pause_requested,
		target,
		current_pause,
		completion_message,
		completion_exit_code
	};
}

bool AutomationDebugSession::PauseMatchesStepMode(AutomationDebugLocation const& location, size_t stack_depth, AutomationDebugThreadContext const& thread) {
	if ((step_mode == StepMode::Over || step_mode == StepMode::Out) && thread.id != step_thread_id)
		return thread.is_suspended && thread.is_suspended(step_thread_id);
	switch (step_mode) {
	case StepMode::None:
		return false;
	case StepMode::Into:
		return true;
	case StepMode::Over:
		if (stack_depth > step_depth) {
			step_over_nested_call = true;
			return false;
		}
		if (std::exchange(step_over_nested_call, false) && stack_depth == step_depth &&
			location.source_path == step_location.source_path && location.line == step_location.line && location.column == step_location.column)
			return false;
		return true;
	case StepMode::Out:
		return stack_depth < step_depth;
	}
	return false;
}

bool AutomationDebugSession::HandleHookPause(
	AutomationDebugLocation location,
	size_t stack_depth,
	std::function<AutomationDebugCapturedState()> const& capture_state,
	AutomationDebugThreadContext const& thread) {
	AutomationDebugPauseReason reason = AutomationDebugPauseReason::Breakpoint;
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!request.enabled || !attached || !invocation_active || location.line <= 0)
			return true;

		if (pause_requested) {
			pause_requested = false;
			reason = AutomationDebugPauseReason::Pause;
		}
		else if (entry_pause_pending) {
			entry_pause_pending = false;
			if (request.auto_step_count > 0)
				pending_step_pauses = request.auto_step_count;
			reason = AutomationDebugPauseReason::Entry;
		}
		else if (breakpoints.Matches(location.source_path, location.line)) {
			if (request.auto_step_count > 0)
				pending_step_pauses = request.auto_step_count;
			reason = AutomationDebugPauseReason::Breakpoint;
		}
		else if (PauseMatchesStepMode(location, stack_depth, thread)) {
			step_mode = StepMode::None;
			step_depth = 0;
			reason = AutomationDebugPauseReason::Step;
		}
		else if (pending_step_pauses > 0) {
			--pending_step_pauses;
			reason = AutomationDebugPauseReason::Step;
		}
		else {
			return true;
		}
	}

	if (capture_state) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!request.enabled || !attached || !invocation_active || state == AutomationDebugSessionState::Completed)
			return true;
	}
	auto captured = capture_state ? capture_state() : AutomationDebugCapturedState{};

	std::unique_lock<std::mutex> lock(mutex);
	if (!request.enabled || !attached || !invocation_active || state == AutomationDebugSessionState::Completed)
		return true;

	++pause_count;
	switch (reason) {
	case AutomationDebugPauseReason::Entry:
		++entry_pause_count;
		break;
	case AutomationDebugPauseReason::Breakpoint:
		++breakpoint_pause_count;
		break;
	case AutomationDebugPauseReason::Step:
		++step_pause_count;
		break;
	case AutomationDebugPauseReason::Pause:
		++manual_pause_count;
		break;
	}

	AutomationDebugPauseRecord record;
	record.sequence = pause_count;
	record.thread_id = thread.id;
	record.stack_depth = stack_depth;
	record.reason = reason;
	record.location = std::move(location);
	record.frames = std::move(captured.frames);
	record.runtime_snapshot = std::move(captured.runtime_snapshot);
	record.scopes = std::move(captured.scopes);
	auto runtime_scopes = BuildScopes(record.runtime_snapshot);
	for (auto& scope : runtime_scopes)
		record.scopes.push_back(std::move(scope));
	current_pause = record;

	if (request.max_pauses != 0 && pauses.size() >= request.max_pauses) {
		pauses.pop_front();
		++dropped_pause_count;
	}
	pauses.push_back(record);

	UpdateStateLocked(AutomationDebugSessionState::Paused);
	if (request.nonblocking) {
		current_pause.reset();
		UpdateStateLocked(AutomationDebugSessionState::Running);
		return true;
	}

	cv.wait(lock, [&] {
		return pause_command_queued || !attached || state == AutomationDebugSessionState::Completed;
	});

	auto const action = resume_action;
	pause_command_queued = false;
	if (action == AutomationDebugResumeAction::Detach || !attached) {
		current_pause.reset();
		UpdateStateLocked(AutomationDebugSessionState::Detached);
		return true;
	}

	current_pause.reset();
	UpdateStateLocked(AutomationDebugSessionState::Running);
	return true;
}

size_t AutomationDebugSession::PauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return pause_count;
}

size_t AutomationDebugSession::EntryPauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return entry_pause_count;
}

size_t AutomationDebugSession::BreakpointPauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return breakpoint_pause_count;
}

size_t AutomationDebugSession::StepPauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return step_pause_count;
}

size_t AutomationDebugSession::ManualPauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return manual_pause_count;
}

size_t AutomationDebugSession::DroppedPauseCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return dropped_pause_count;
}

size_t AutomationDebugSession::BreakpointCount() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return breakpoints.Count();
}

std::vector<AutomationDebugPauseRecord> AutomationDebugSession::GetPauses() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return { pauses.begin(), pauses.end() };
}

bool AutomationDebugSession::WriteTraceFile(agi::fs::path const& path, std::string& error) const
{
	auto pause_records = GetPauses();

	return WriteJsonLinesFile(
		path,
		pause_records,
		SerializePauseRecord,
		"could not open automation debug trace file for writing",
		error);
}
}
