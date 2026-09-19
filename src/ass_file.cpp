// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
// Copyright (c) 2026, MIR
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

#include "ass_file.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_info.h"
#include "ass_style_resolution.h"
#include "ass_style.h"
#include "subtitle_grid_folding.h"
#include "transient_font_set.h"

#include <algorithm>
#include <filesystem>
#include <cassert>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>
#include <unordered_map>
#include <unordered_set>

namespace {
std::pair<char const*, char const*> resolution_keys(ScriptResolutionType type) {
	switch (type) {
	case ScriptResolutionType::PlayRes:
		return {"PlayResX", "PlayResY"};
	case ScriptResolutionType::LayoutRes:
		return {"LayoutResX", "LayoutResY"};
	case ScriptResolutionType::None:
		break;
	}
	return {"", ""};
}

bool get_resolution(AssFile const& file, ScriptResolutionType type, int &sw, int &sh) {
	auto keys = resolution_keys(type);
	sw = file.GetScriptInfoAsInt(keys.first);
	sh = file.GetScriptInfoAsInt(keys.second);
	if (sw == 0 && sh == 0)
		return false;
	if (sw == 0)
		sw = sh == 1024 ? 1280 : sh * 4 / 3;
	else if (sh == 0)
		sh = sw == 1280 ? 1024 : sw * 3 / 4;
	return true;
}

void LogTransientFontSetDebug(char const* action, std::shared_ptr<const TransientFontSet> const& set) {
	if (!set || set->empty())
		return;

	for (size_t i = 0; i < set->fonts.size(); ++i) {
		auto const& font = set->fonts[i];
		LOG_D("subtitle/fonts/transient") << action << ": " << font.original_name
			<< agi::format(" (%u/%u, %u bytes%s%s, generation %u)",
				static_cast<unsigned>(i + 1),
				static_cast<unsigned>(set->fonts.size()),
				static_cast<unsigned>(font.bytes.size()),
				font.mime_type.empty() ? "" : ", mime=",
				font.mime_type.empty() ? "" : font.mime_type.c_str(),
				static_cast<unsigned>(set->generation));
	}
}
}

AssFile::AssFile() { }

AssFile::~AssFile() {
	Styles.clear_and_dispose([](AssStyle *e) { delete e; });
	Events.clear_and_dispose([](AssDialogue *e) { delete e; });
}

void AssFile::LoadDefault(bool include_dialogue_line) {
	AssFileLoadDefaultOptions options;
	options.include_dialogue_line = include_dialogue_line;
	LoadDefault(options);
}

void AssFile::LoadDefault(AssFileLoadDefaultOptions const& options) {
	transient_fonts.reset();

	Info.emplace_back("Title", "Default Aegisub file");
	Info.emplace_back("ScriptType", "v4.00+");
	Info.emplace_back("WrapStyle", "0");
	Info.emplace_back("ScaledBorderAndShadow", "yes");
	if (options.set_resolution)
		SetResolution(ScriptResolutionType::None, options.resolution_width, options.resolution_height);
	Info.emplace_back("YCbCr Matrix", "None");

	// Add default style
	Styles.push_back(*new AssStyle);

	if (options.include_dialogue_line)
		Events.push_back(*new AssDialogue);
}

AssFile::AssFile(const AssFile &from)
: Info(from.Info)
, Attachments(from.Attachments)
, Extradata(from.Extradata)
, Properties(from.Properties)
, transient_fonts(from.transient_fonts)
, next_extradata_id(from.next_extradata_id)
{
	Styles.clone_from(from.Styles,
		[](AssStyle const& e) { return new AssStyle(e); },
		[](AssStyle *e) { delete e; });
	Events.clone_from(from.Events,
		[](AssDialogue const& e) { return new AssDialogue(e); },
		[](AssDialogue *e) { delete e; });
}

void AssFile::swap(AssFile& from) throw() {
	Info.swap(from.Info);
	Styles.swap(from.Styles);
	Events.swap(from.Events);
	Attachments.swap(from.Attachments);
	Extradata.swap(from.Extradata);
	std::swap(Properties, from.Properties);
	std::swap(transient_fonts, from.transient_fonts);
	std::swap(next_extradata_id, from.next_extradata_id);
	folding.swap(from.folding);
}

AssFile& AssFile::operator=(AssFile from) {
	swap(from);
	return *this;
}

EntryList<AssDialogue>::iterator AssFile::iterator_to(AssDialogue& line) {
	using l = EntryList<AssDialogue>;
	bool in_list = !l::node_algorithms::inited(l::value_traits::to_node_ptr(line));
	return in_list ? Events.iterator_to(line) : Events.end();
}

SubtitleGridFolding& AssFile::Folding() {
	if (!folding) {
		folding = std::make_unique<SubtitleGridFolding>();
		folding->Rebuild(*this);
	}
	return *folding;
}

void AssFile::InsertAttachment(agi::fs::path const& filename) {
	AssEntryGroup group = AssEntryGroup::GRAPHIC;

	auto ext = agi::util::strings::to_lower_copy(agi::fs::PathToString(filename.extension()));
	if (ext == ".ttf" || ext == ".ttc" || ext == ".pfb")
		group = AssEntryGroup::FONT;

	Attachments.emplace_back(filename, group);
}

void AssFile::SetTransientFonts(std::shared_ptr<const TransientFontSet> fonts) {
	if (transient_fonts == fonts)
		return;

	auto describe = [](std::shared_ptr<const TransientFontSet> const& set) {
		if (!set)
			return std::string("none");
		return agi::format("%u font(s), generation %u",
			static_cast<unsigned>(set->fonts.size()),
			static_cast<unsigned>(set->generation));
	};

	if (transient_fonts && fonts) {
		LOG_I("subtitle/fonts/transient") << "Replacing transient font set: "
			<< describe(transient_fonts) << " -> " << describe(fonts);
		LogTransientFontSetDebug("Previous transient font", transient_fonts);
		LogTransientFontSetDebug("New transient font", fonts);
	}
	else if (fonts) {
		LOG_I("subtitle/fonts/transient") << "Installing transient font set: " << describe(fonts);
		LogTransientFontSetDebug("Installed transient font", fonts);
	}
	else if (transient_fonts) {
		LOG_I("subtitle/fonts/transient") << "Clearing transient font set: " << describe(transient_fonts);
		LogTransientFontSetDebug("Cleared transient font", transient_fonts);
	}

	transient_fonts = std::move(fonts);
}

std::string AssFile::GetScriptInfo(std::string const& key) const {
	for (auto const& info : Info) {
		if (agi::util::strings::iequals(key, info.Key()))
			return info.Value();
	}

	return "";
}

int AssFile::GetScriptInfoAsInt(std::string const& key) const {
	return atoi(GetScriptInfo(key).c_str());
}

void AssFile::SetScriptInfo(std::string const& key, std::string const& value) {
	for (auto it = Info.begin(); it != Info.end(); ++it) {
		if (agi::util::strings::iequals(key, it->Key())) {
			if (value.empty())
				Info.erase(it);
			else
				it->SetValue(value);
			return;
		}
	}

	if (!value.empty())
		Info.emplace_back(key, value);
}

void AssFile::GetLayoutResolution(int &lw, int &lh) const {
	lw = GetScriptInfoAsInt("LayoutResX");
	lh = GetScriptInfoAsInt("LayoutResY");
}

void AssFile::GetResolution(int &sw, int &sh) const {
	GetResolution(GetPreferredResolutionType(), sw, sh);
}

void AssFile::GetResolution(ScriptResolutionType preferred, int &sw, int &sh) const {
	GetResolutionType(preferred, sw, sh);
}

ScriptResolutionType AssFile::GetResolutionType(int &sw, int &sh) const {
	return GetResolutionType(GetPreferredResolutionType(), sw, sh);
}

ScriptResolutionType AssFile::GetResolutionType(ScriptResolutionType primary, int &sw, int &sh) const {
	if (get_resolution(*this, primary, sw, sh))
		return primary;

	auto secondary = primary == ScriptResolutionType::PlayRes ? ScriptResolutionType::LayoutRes : ScriptResolutionType::PlayRes;
	if (get_resolution(*this, secondary, sw, sh))
		return secondary;

	sw = 384;
	sh = 288;
	return ScriptResolutionType::None;
}

ScriptResolutionType AssFile::GetResolutionType() const {
	int sw, sh;
	return GetResolutionType(sw, sh);
}

ScriptResolutionType AssFile::GetResolutionType(ScriptResolutionType preferred) const {
	int sw, sh;
	return GetResolutionType(preferred, sw, sh);
}

ScriptResolutionType AssFile::GetPreferredResolutionType() const {
	return ScriptResolutionType::PlayRes;
}

void AssFile::SetResolution(ScriptResolutionType type, int w, int h) {
	(void)type;

	for (auto resolution_type : { ScriptResolutionType::PlayRes, ScriptResolutionType::LayoutRes }) {
		auto keys = resolution_keys(resolution_type);
		SetScriptInfo(keys.first, std::to_string(w));
		SetScriptInfo(keys.second, std::to_string(h));
	}
}

std::vector<std::string> AssFile::GetStyles() const {
	std::vector<std::string> styles;
	for (auto& style : Styles)
		styles.push_back(style.name);
	return styles;
}

AssStyle *AssFile::GetStyle(std::string const& name) {
	return aegisub::ass_style_resolution::ResolveEventStyle(*this, name);
}

int AssFile::Commit(std::string const& desc, int type, int amend_id, AssDialogue *single_line) {
	if (!single_line)
		return Commit(desc, type, amend_id, nullptr, {});

	AssDialogue const *changed_lines[] = {single_line};
	return Commit(desc, type, amend_id, single_line, changed_lines);
}

int AssFile::Commit(std::string const& desc, int type, int amend_id, AssDialogue *single_line,
	AssDialogueCommitSpan changed_lines) {
	// Exact changed-lines metadata is authoritative when supplied.
	single_line = changed_lines.size() == 1 && single_line == changed_lines.front() ? single_line : nullptr;

	if (type == COMMIT_NEW || (type & COMMIT_DIAG_ADDREM) || (type & COMMIT_ORDER)) {
		int i = 0;
		for (auto& event : Events)
			event.Row = i++;
	}

	if (type == COMMIT_NEW || (type & (COMMIT_DIAG_ADDREM | COMMIT_ORDER | COMMIT_EXTRADATA))) {
		if (Folding().PrepareCommit(*this, type)) {
			type |= COMMIT_FOLD;
			single_line = nullptr;
			changed_lines = {};
		}
	}

	PushState({desc, &amend_id, single_line, type, changed_lines});

	AnnounceCommitDetails({type, single_line, changed_lines});
	AnnounceCommit(type, single_line);

	return amend_id;
}

bool AssFile::CompStart(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.Start < rgt.Start;
}
bool AssFile::CompEnd(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.End < rgt.End;
}
bool AssFile::CompStyle(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.Style < rgt.Style;
}
bool AssFile::CompActor(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.Actor < rgt.Actor;
}
bool AssFile::CompEffect(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.Effect < rgt.Effect;
}
bool AssFile::CompLayer(AssDialogue const& lft, AssDialogue const& rgt) {
	return lft.Layer < rgt.Layer;
}

void AssFile::Sort(CompFunc comp, std::set<AssDialogue*> const& limit) {
	Sort(Events, comp, limit);
}

void AssFile::Sort(EntryList<AssDialogue> &lst, CompFunc comp, std::set<AssDialogue*> const& limit) {
	if (limit.empty()) {
		lst.sort(comp);
		return;
	}

	// The membership test below runs once per line in the whole file, so it is
	// the dominant cost of a partial sort on a large script. std::set::count
	// walks a red-black tree per probe; copying the (already pointer-ordered)
	// set into a flat vector lets us binary search contiguous memory instead.
	std::vector<AssDialogue *> selected(limit.begin(), limit.end());
	auto is_selected = [&selected](AssDialogue *line) {
		return std::binary_search(selected.begin(), selected.end(), line);
	};

	// Sort each selected block separately, leaving everything else untouched
	size_t remaining = selected.size();
	for (auto begin = lst.begin(); begin != lst.end() && remaining > 0; ++begin) {
		if (!is_selected(&*begin)) continue;

		// begin is known selected, so start scanning the block after it
		auto end = std::next(begin);
		size_t block_size = 1;
		while (end != lst.end() && is_selected(&*end)) {
			++end;
			++block_size;
		}
		// Stop scanning once every selected line has been accounted for
		remaining -= block_size;

		// A single-line block is already sorted; splicing it would be pure overhead
		if (block_size > 1) {
			// sort doesn't support only sorting a sublist, so move them to a temp list
			EntryList<AssDialogue> tmp;
			tmp.splice(tmp.begin(), lst, begin, end);
			tmp.sort(comp);
			lst.splice(end, tmp);
		}

		begin = std::prev(end);
	}
}

uint32_t AssFile::AddExtradata(std::string const& key, std::string const& value) {
	for (auto const& data : Extradata) {
		// perform brute-force deduplication by simple key and value comparison
		if (key == data.key && value == data.value) {
			return data.id;
		}
	}
	Extradata.push_back(ExtradataEntry{next_extradata_id, key, value});
	return next_extradata_id++; // return old value, then post-increment
}

namespace {
struct extradata_id_cmp {
	bool operator()(ExtradataEntry const& e, uint32_t id) {
		return e.id < id;
	}
	bool operator()(uint32_t id, ExtradataEntry const& e) {
		return id < e.id;
	}
};

template<typename ExtradataType, typename Func>
void enumerate_extradata(ExtradataType&& extradata, std::vector<uint32_t> const& id_list, Func&& f) {
	auto begin = extradata.begin(), end = extradata.end();
	for (auto id : id_list) {
		auto it = lower_bound(begin, end, id, extradata_id_cmp{});
		if (it != end) {
			f(*it);
			begin = it;
		}
	}
}

template<typename K, typename V>
using reference_map = std::unordered_map<std::reference_wrapper<const K>, V, std::hash<K>, std::equal_to<K>>;
}

std::vector<ExtradataEntry> AssFile::GetExtradata(std::vector<uint32_t> const& id_list) const {
	std::vector<ExtradataEntry> result;
	enumerate_extradata(Extradata, id_list, [&](ExtradataEntry const& e) {
		result.push_back(e);
	});
	return result;
}

void AssFile::CleanExtradata() {
	if (Extradata.empty()) return;

	std::unordered_set<uint32_t> ids_used;
	for (auto& line : Events) {
		if (line.ExtradataIds.get().empty()) continue;

		// Find the ID for each unique key in the line
		reference_map<std::string, uint32_t> keys_used;
		enumerate_extradata(Extradata, line.ExtradataIds.get(), [&](ExtradataEntry const& e) {
			keys_used[e.key] = e.id;
		});

		for (auto const& used : keys_used)
			ids_used.insert(used.second);

		// If any keys were duplicated or missing, update the id list
		if (keys_used.size() != line.ExtradataIds.get().size()) {
			std::vector<uint32_t> ids;
			ids.reserve(keys_used.size());
			for (auto const& used : keys_used)
				ids.push_back(used.second);
			std::sort(begin(ids), end(ids));
			line.ExtradataIds = std::move(ids);
		}
	}

	if (ids_used.size() != Extradata.size()) {
		// Erase all no-longer-used extradata entries
		Extradata.erase(std::remove_if(begin(Extradata), end(Extradata), [&](ExtradataEntry const& e) {
			return !ids_used.count(e.id);
		}), end(Extradata));
	}
}
