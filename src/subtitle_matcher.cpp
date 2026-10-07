// Copyright (c) 2026
// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file subtitle_matcher.cpp
/// @see subtitle_matcher.h

#include "subtitle_matcher.h"

#include "ass_dialogue.h"

#include <libaegisub/exception.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/util.h>

#include <boost/locale/conversion.hpp>

#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace {
static const size_t bad_pos = -1;
static const MatchState bad_match{0, bad_pos};

auto get_dialogue_field(SearchReplaceSettings::Field field) -> decltype(&AssDialogueBase::Text) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return &AssDialogueBase::Text;
		case SearchReplaceSettings::Field::STYLE: return &AssDialogueBase::Style;
		case SearchReplaceSettings::Field::ACTOR: return &AssDialogueBase::Actor;
		case SearchReplaceSettings::Field::EFFECT: return &AssDialogueBase::Effect;
	}
	throw agi::InternalError("Bad field for search");
}

std::string const& get_field_text(const AssDialogue *diag, decltype(&AssDialogueBase::Text) field) {
	return (diag->*field).get();
}

typedef std::function<MatchState(const AssDialogue *, size_t)> matcher;

struct CpSpan {
	UChar32 cp = 0;
	size_t b0 = 0;
	size_t b1 = 0;
};

void append_utf8(std::string& out, UChar32 cp) {
	uint8_t buf[U8_MAX_LENGTH];
	int32_t i = 0;
	U8_APPEND_UNSAFE(buf, i, cp);
	out.append(reinterpret_cast<char const*>(buf), static_cast<size_t>(i));
}

std::vector<CpSpan> decode_utf8_spans(std::string const& s) {
	std::vector<CpSpan> out;
	out.reserve(s.size());
	auto const* data = reinterpret_cast<uint8_t const*>(s.data());
	int32_t i = 0;
	int32_t const len = static_cast<int32_t>(s.size());
	while (i < len) {
		int32_t const start = i;
		UChar32 c;
		U8_NEXT(data, i, len, c);
		if (c < 0)
			continue;
		out.push_back({c, static_cast<size_t>(start), static_cast<size_t>(i)});
	}
	return out;
}

UNormalizer2 const* nfc_instance() {
	UErrorCode err = U_ZERO_ERROR;
	return unorm2_getNFCInstance(&err);
}

UNormalizer2 const* nfd_instance() {
	UErrorCode err = U_ZERO_ERROR;
	return unorm2_getNFDInstance(&err);
}

/// NFD-decompose each code point, preserving the original byte span on every
/// output code point so reordering and composition can track identity.
std::vector<CpSpan> decompose_nfd(std::vector<CpSpan> const& cps) {
	auto const* nfd = nfd_instance();
	std::vector<CpSpan> out;
	out.reserve(cps.size() * 2);
	UChar decomp[32];
	for (auto const& cp : cps) {
		UErrorCode err = U_ZERO_ERROR;
		int32_t n = unorm2_getDecomposition(nfd, cp.cp, decomp, 32, &err);
		if (U_SUCCESS(err) && n > 0) {
			int32_t j = 0;
			while (j < n) {
				UChar32 c;
				U16_NEXT(decomp, j, n, c);
				out.push_back({c, cp.b0, cp.b1});
			}
		}
		else {
			out.push_back(cp);
		}
	}
	return out;
}

/// Canonical reorder: combining marks move with their original spans.
void reorder_canonical(std::vector<CpSpan>& cps) {
	for (size_t i = 1; i < cps.size(); ++i) {
		uint8_t const ccc = u_getCombiningClass(cps[i].cp);
		if (ccc == 0)
			continue;
		size_t j = i;
		while (j > 0) {
			uint8_t const prev = u_getCombiningClass(cps[j - 1].cp);
			if (prev == 0 || prev <= ccc)
				break;
			std::swap(cps[j], cps[j - 1]);
			--j;
		}
	}
}

/// UAX #15 canonical composition, merging original spans of composed parts.
std::vector<CpSpan> compose_nfc(std::vector<CpSpan> const& nfd) {
	auto const* nfc = nfc_instance();
	std::vector<CpSpan> out;
	out.reserve(nfd.size());

	for (auto const& ch : nfd) {
		uint8_t const ccc = u_getCombiningClass(ch.cp);

		int starter = -1;
		for (int j = static_cast<int>(out.size()) - 1; j >= 0; --j) {
			if (u_getCombiningClass(out[static_cast<size_t>(j)].cp) == 0) {
				starter = j;
				break;
			}
		}

		bool blocked = (starter < 0);
		if (!blocked) {
			// Blocked if any character between the starter and the end of `out`
			// has ccc == 0 or ccc >= ccc(ch). (UAX #15)
			for (size_t j = static_cast<size_t>(starter) + 1; j < out.size(); ++j) {
				uint8_t const b = u_getCombiningClass(out[j].cp);
				if (b == 0 || b >= ccc) {
					blocked = true;
					break;
				}
			}
			// Starters (ccc==0) only compose when immediately adjacent.
			if (ccc == 0 && starter != static_cast<int>(out.size()) - 1)
				blocked = true;
		}

		if (!blocked) {
			UChar32 const composed = unorm2_composePair(nfc, out[static_cast<size_t>(starter)].cp, ch.cp);
			if (composed >= 0) {
				auto& s = out[static_cast<size_t>(starter)];
				s.cp = composed;
				s.b0 = std::min(s.b0, ch.b0);
				s.b1 = std::max(s.b1, ch.b1);
				continue;
			}
		}
		out.push_back(ch);
	}
	return out;
}

/// Maps byte offsets between original UTF-8 and NFC, including combining-mark
/// reordering. Built in linear time over code points (plus decomp table lookups).
struct nfc_offset_map {
	std::string normalized;
	/// For each NFC UTF-8 byte i, the original [begin,end) that produced it.
	std::vector<size_t> byte_orig_begin;
	std::vector<size_t> byte_orig_end;
	/// earliest_nfc_end[p] style for to_nfc: NFC length fully covered by original[0, L).
	/// to_nfc(orig_pos) = max NFC byte index whose contributing orig is fully before orig_pos...
	/// We store for each original byte boundary the corresponding NFC cursor.
	std::vector<size_t> orig_to_nfc; // size original.size()+1

	void clear() {
		normalized.clear();
		byte_orig_begin.clear();
		byte_orig_end.clear();
		orig_to_nfc.clear();
	}

	void build_identity(std::string const& original) {
		normalized = original;
		byte_orig_begin.resize(original.size());
		byte_orig_end.resize(original.size());
		for (size_t i = 0; i < original.size(); ++i) {
			byte_orig_begin[i] = i;
			byte_orig_end[i] = i + 1;
		}
		orig_to_nfc.resize(original.size() + 1);
		for (size_t i = 0; i <= original.size(); ++i)
			orig_to_nfc[i] = i;
	}

	void build(std::string const& original) {
		clear();
		if (original.empty()) {
			orig_to_nfc = {0};
			return;
		}

		// Fast path: already NFC.
		std::string const icu_nfc = boost::locale::normalize(original);
		if (icu_nfc == original) {
			build_identity(original);
			return;
		}

		auto cps = decode_utf8_spans(original);
		cps = decompose_nfd(cps);
		reorder_canonical(cps);
		cps = compose_nfc(cps);

		// Encode NFC and record per-byte original spans.
		normalized.clear();
		normalized.reserve(icu_nfc.size());
		byte_orig_begin.reserve(icu_nfc.size());
		byte_orig_end.reserve(icu_nfc.size());

		for (auto const& cp : cps) {
			size_t const before = normalized.size();
			append_utf8(normalized, cp.cp);
			for (size_t i = before; i < normalized.size(); ++i) {
				byte_orig_begin.push_back(cp.b0);
				byte_orig_end.push_back(cp.b1);
			}
		}

		// Prefer ICU/boost NFC when our composition disagrees (Hangul edge cases
		// etc.); fall back to identity mapping rather than wrong ranges.
		if (normalized != icu_nfc) {
			build_identity(original);
			normalized = icu_nfc;
			// Identity is wrong for mapping but safer than corrupt offsets when
			// we cannot trust our compose. Re-run with prefix map as last resort:
			// still better than mid-sequence for pure composition without reorder.
			// Actually identity on original bytes with normalized haystack is the
			// old bug. Use icu string with end-of-string mapping only:
			byte_orig_begin.assign(normalized.size(), 0);
			byte_orig_end.assign(normalized.size(), original.size());
			for (size_t i = 0; i < normalized.size(); ++i) {
				byte_orig_begin[i] = 0;
				byte_orig_end[i] = original.size();
			}
			orig_to_nfc.assign(original.size() + 1, 0);
			orig_to_nfc.back() = normalized.size();
			// Crude but won't split UTF-8; search still works on NFC haystack.
			// Fill linear interpolation by code unit count as weak fallback:
			if (!original.empty() && !normalized.empty()) {
				for (size_t o = 0; o <= original.size(); ++o)
					orig_to_nfc[o] = o * normalized.size() / original.size();
				for (size_t i = 0; i < normalized.size(); ++i) {
					byte_orig_begin[i] = i * original.size() / normalized.size();
					byte_orig_end[i] = std::min(original.size(),
						(i + 1) * original.size() / normalized.size() + (original.size() > 0 ? 1 : 0));
					if (byte_orig_end[i] <= byte_orig_begin[i])
						byte_orig_end[i] = std::min(original.size(), byte_orig_begin[i] + 1);
				}
			}
			return;
		}

		// orig_to_nfc[o]: first NFC byte not entirely before original offset o.
		// Single forward scan — O(n) total (nfc_pos only advances).
		orig_to_nfc.assign(original.size() + 1, 0);
		size_t nfc_pos = 0;
		for (size_t o = 0; o <= original.size(); ++o) {
			while (nfc_pos < normalized.size() && byte_orig_end[nfc_pos] <= o)
				++nfc_pos;
			orig_to_nfc[o] = nfc_pos;
		}
	}

	size_t to_nfc(size_t orig_pos) const {
		if (orig_to_nfc.empty())
			return 0;
		if (orig_pos >= orig_to_nfc.size())
			return orig_to_nfc.back();
		return orig_to_nfc[orig_pos];
	}

	/// Map exclusive NFC range [nfc_s, nfc_e) to original [begin, end).
	std::pair<size_t, size_t> to_orig_range(size_t nfc_s, size_t nfc_e) const {
		if (normalized.empty())
			return {0, 0};
		nfc_s = std::min(nfc_s, normalized.size());
		nfc_e = std::min(nfc_e, normalized.size());
		if (nfc_s >= nfc_e) {
			size_t o = nfc_s < byte_orig_begin.size() ? byte_orig_begin[nfc_s]
			         : (byte_orig_end.empty() ? 0 : byte_orig_end.back());
			return {o, o};
		}
		size_t b0 = byte_orig_begin[nfc_s];
		size_t b1 = byte_orig_end[nfc_s];
		for (size_t i = nfc_s + 1; i < nfc_e; ++i) {
			b0 = std::min(b0, byte_orig_begin[i]);
			b1 = std::max(b1, byte_orig_end[i]);
		}
		return {b0, b1};
	}

	size_t to_orig(size_t nfc_pos) const {
		if (nfc_pos >= normalized.size())
			return byte_orig_end.empty() ? 0 : byte_orig_end.back();
		// Cursor at nfc_pos: original cursor after all bytes before nfc_pos.
		if (nfc_pos == 0)
			return 0;
		return byte_orig_end[nfc_pos - 1];
	}
};

class noop_accessor {
	boost::flyweight<std::string> AssDialogueBase::*field;
	size_t nfc_view_start = 0;
	std::string haystack;
	bool do_normalize;
	nfc_offset_map nfc_map;
	std::string cached_raw;

public:
	noop_accessor(SearchReplaceSettings::Field f, bool normalize)
	: field(get_dialogue_field(f)), do_normalize(normalize) { }

	void ensure_map(std::string const& raw) {
		if (!do_normalize) {
			haystack = raw;
			return;
		}
		if (cached_raw == raw && !nfc_map.normalized.empty()) {
			haystack = nfc_map.normalized;
			return;
		}
		cached_raw = raw;
		nfc_map.build(raw);
		haystack = nfc_map.normalized;
	}

	void prepare(const AssDialogue *d) {
		ensure_map(get_field_text(d, field));
	}

	std::string const& full_haystack() const { return haystack; }

	size_t orig_to_search(size_t orig_start) const {
		return do_normalize ? nfc_map.to_nfc(orig_start) : orig_start;
	}

	agi::util::strings::view get_view(const AssDialogue *d, size_t orig_start) {
		prepare(d);
		nfc_view_start = orig_to_search(orig_start);
		return agi::util::strings::subview(haystack, nfc_view_start);
	}

	std::string get_string(const AssDialogue *d, size_t s) {
		return std::string(get_view(d, s));
	}

	/// `abs_s`/`abs_e` are offsets in full_haystack().
	MatchState make_match_state_abs(size_t abs_s, size_t abs_e) {
		MatchState ms;
		ms.search_end = abs_e;
		if (do_normalize) {
			auto range = nfc_map.to_orig_range(abs_s, abs_e);
			ms.start = range.first;
			ms.end = range.second;
		}
		else {
			ms.start = abs_s;
			ms.end = abs_e;
		}
		return ms;
	}

	MatchState make_match_state(size_t s, size_t e) {
		return make_match_state_abs(nfc_view_start + s, nfc_view_start + e);
	}
};

class skip_tags_accessor {
	boost::flyweight<std::string> AssDialogueBase::*field;
	agi::util::tagless_find_helper helper;
	std::string haystack;
	std::string stripped;
	size_t plain_view_start = 0;
	bool do_normalize;
	nfc_offset_map nfc_map;
	std::string cached_raw;

public:
	skip_tags_accessor(SearchReplaceSettings::Field f, bool normalize)
	: field(get_dialogue_field(f)), do_normalize(normalize) { }

	void ensure_map(std::string const& raw) {
		if (!do_normalize) {
			haystack = raw;
			return;
		}
		if (cached_raw == raw && !nfc_map.normalized.empty()) {
			haystack = nfc_map.normalized;
			return;
		}
		cached_raw = raw;
		nfc_map.build(raw);
		haystack = nfc_map.normalized;
	}

	void prepare(const AssDialogue *d) {
		ensure_map(get_field_text(d, field));
		// Full tagless surface so lookbehind can see plain text before the cursor.
		stripped = helper.strip_tags(haystack, 0);
	}

	std::string const& full_haystack() const { return stripped; }

	/// Map original-field cursor into the full tag-stripped string.
	size_t orig_to_search(size_t orig_start) const {
		size_t hay = do_normalize ? nfc_map.to_nfc(orig_start) : orig_start;
		hay = std::min(hay, haystack.size());
		agi::util::tagless_find_helper tmp;
		return tmp.strip_tags(haystack.substr(0, hay), 0).size();
	}

	agi::util::strings::view get_view(const AssDialogue *d, size_t orig_start) {
		prepare(d);
		plain_view_start = orig_to_search(orig_start);
		return agi::util::strings::subview(stripped, plain_view_start);
	}

	std::string get_string(const AssDialogue *d, size_t s) {
		return std::string(get_view(d, s));
	}

	MatchState make_match_state_abs(size_t abs_s, size_t abs_e) {
		MatchState ms;
		ms.search_end = abs_e;
		size_t s = abs_s;
		size_t e = abs_e;
		helper.map_range(s, e);
		if (do_normalize) {
			auto range = nfc_map.to_orig_range(s, e);
			ms.start = range.first;
			ms.end = range.second;
		}
		else {
			ms.start = s;
			ms.end = e;
		}
		return ms;
	}

	MatchState make_match_state(size_t s, size_t e) {
		return make_match_state_abs(plain_view_start + s, plain_view_start + e);
	}
};

using utf8_iterator = std::string::const_iterator;
using u32_iterator = boost::u8_to_u32_iterator<utf8_iterator, UChar32>;
using u32_match = boost::match_results<u32_iterator>;

std::string format_regex_match(u32_match const& source, boost::u32regex const& regex,
                               std::string const& format, bool match_only) {
	auto result = source;
	if (match_only) {
		auto const match_begin = result[0].first;
		auto const match_end = result[0].second;
		// match_results exposes stored sub-matches as const references even on a
		// mutable result. This is a private copy, so narrowing its context is safe.
		auto& prefix = const_cast<u32_match::value_type&>(result.prefix());
		auto& suffix = const_cast<u32_match::value_type&>(result.suffix());
		prefix.first = prefix.second = match_begin;
		prefix.matched = false;
		suffix.first = suffix.second = match_end;
		suffix.matched = false;
	}

	std::vector<UChar32> u32_format;
	u32_format.assign(
		u32_iterator(format.cbegin(), format.cbegin(), format.cend()),
		u32_iterator(format.cend(), format.cbegin(), format.cend()));

	std::string output;
	boost::utf8_output_iterator<std::back_insert_iterator<std::string>> out(std::back_inserter(output));
	auto const* format_begin = u32_format.empty() ? static_cast<UChar32 const*>(nullptr) : u32_format.data();
	auto const* format_end = u32_format.empty() ? static_cast<UChar32 const*>(nullptr)
	                                           : u32_format.data() + u32_format.size();
	boost::BOOST_REGEX_DETAIL_NS::regex_format_imp(
		out, result, format_begin, format_end, boost::format_default, regex.get_traits());
	return output;
}

void set_regex_replacements(MatchState& ms, u32_match const& result,
                            boost::u32regex const& regex, std::string const& format) {
	ms.has_regex_replacement = true;
	// Short-circuit an empty format: format_regex_match does two UTF-32
	// traversals per call, and a Find report (or any search with no replace
	// string) never consumes the result. Avoiding the work matters on the
	// per-keystroke recompute path, where the enumerator runs once per edited
	// line per keystroke.
	if (format.empty()) {
		ms.match_only_replacement.clear();
		ms.search_context_replacement.clear();
		return;
	}
	ms.match_only_replacement = format_regex_match(result, regex, format, true);
	ms.search_context_replacement = format_regex_match(result, regex, format, false);
}

std::string prepare_search_text(SearchReplaceSettings const& settings) {
	if (!settings.use_unicode_escapes)
		return settings.find;

	std::string expanded;
	if (!agi::util::strings::expand_unicode_codepoint_escapes(settings.find, expanded))
		throw agi::InvalidInputException("Invalid Unicode escape. Use \\uXXXX, \\UXXXXXXXX, u+XXXX, or U+XXXX.");
	return expanded;
}

template<typename Accessor>
matcher get_matcher(SearchReplaceSettings const& settings, Accessor&& a,
                    std::optional<size_t> search_after_empty = std::nullopt) {
	std::string search_text = prepare_search_text(settings);
	std::string prepared_find = settings.match_case ? std::move(search_text)
	                                                : boost::locale::normalize(search_text);

	if (settings.use_regex) {
		int flags = boost::u32regex::perl;
		if (!settings.match_case)
			flags |= boost::u32regex::icase;

		auto regex = boost::make_u32regex(prepared_find, flags);

		return [=](const AssDialogue *diag, size_t start) mutable -> MatchState {
			// Search the full NFC/tagless surface with match_prev_avail so
			// lookbehind still sees text before the original cursor (suffix
			// strings would lose that context). Bound retries by field length.
			const auto& raw = (diag->*get_dialogue_field(settings.field)).get();
			size_t search_at = start;
			size_t const limit = raw.size() + 1;
			while (search_at <= limit) {
				a.prepare(diag);
				auto const& full = a.full_haystack();
				size_t const from = search_after_empty.value_or(a.orig_to_search(search_at));
				if (from > full.size())
					return bad_match;

				auto const base_it = full.cbegin();
				auto const start_it = base_it + static_cast<std::ptrdiff_t>(from);
				auto const end_it = full.cend();
				u32_match result;
				u32_iterator const base(base_it, base_it, end_it);
				u32_iterator const search_begin(start_it, base_it, end_it);
				u32_iterator const search_end(end_it, base_it, end_it);
				// 6-arg form: `base_it` is the sequence start so lookbehind can
				// read characters before `from`. Flags mark mid-field starts.
				auto mflags = from > 0
					? (boost::match_not_bol | boost::match_not_bob | boost::match_prev_avail)
					: boost::match_default;
				if (search_after_empty)
					mflags |= boost::regex_constants::match_not_initial_null;
				if (!boost::regex_search(search_begin, search_end, result, regex, mflags, base))
					return bad_match;

				size_t const abs_s = static_cast<size_t>(result[0].first.base() - base_it);
				size_t const abs_e = static_cast<size_t>(result[0].second.base() - base_it);
				MatchState ms = a.make_match_state_abs(abs_s, abs_e);

				if (search_after_empty || ms.start >= start) {
					set_regex_replacements(ms, result, regex, settings.replace_with);
					return ms;
				}

				// Stale hit (entirely before cursor in original space). Advance
				// the original search cursor past it and try again.
				if (ms.end > search_at)
					search_at = ms.end;
				else
					++search_at;
				if (ms.end == ms.start && search_at == ms.end)
					++search_at;
			}
			return bad_match;
		};
	}

	bool full_match_only = settings.exact_match;
	bool match_case = settings.match_case;
	std::string look_for = std::move(prepared_find);
#ifdef AEGISUB_USE_STRINGZILLA
	agi::util::strings::utf8_icase_searcher icase_searcher{agi::util::strings::view(look_for)};
#endif

	return [=](const AssDialogue *diag, size_t start) mutable -> MatchState {
		const auto& raw = (diag->*get_dialogue_field(settings.field)).get();
		size_t search_at = start;
		size_t const limit = raw.size() + 1;
		while (search_at <= limit) {
			const auto str = a.get_view(diag, search_at);
			const agi::util::strings::view look_for_view(look_for);
			MatchState ms = bad_match;

			if (full_match_only) {
				if (match_case) {
					if (str == look_for_view)
						ms = a.make_match_state(0, str.size());
				}
				else {
#ifdef AEGISUB_USE_STRINGZILLA
					const auto match = icase_searcher.find(str);
					if (match && match.offset == 0 && match.length == str.size())
						ms = a.make_match_state(0, str.size());
#else
					const auto pos = agi::util::ifind(std::string(str), look_for);
					if (pos.first == 0 && pos.second == str.size())
						ms = a.make_match_state(pos.first, pos.second);
#endif
				}
			}
			else if (match_case) {
				const auto pos = agi::util::strings::find(str, look_for_view);
				if (pos != agi::util::strings::npos)
					ms = a.make_match_state(pos, pos + look_for_view.size());
			}
			else {
#ifdef AEGISUB_USE_STRINGZILLA
				const auto match = icase_searcher.find(str);
				if (match)
					ms = a.make_match_state(match.offset, match.offset + match.length);
#else
				const auto pos = agi::util::ifind(std::string(str), look_for);
				if (pos.first != bad_pos)
					ms = a.make_match_state(pos.first, pos.second);
#endif
			}

			if (!ms)
				return bad_match;
			if (ms.start >= start)
				return ms;

			if (ms.end > search_at)
				search_at = ms.end;
			else
				++search_at;
			if (ms.end == ms.start && search_at == ms.end)
				++search_at;
		}
		return bad_match;
	};
}

size_t advance_search_utf8(std::string const& text, size_t pos) {
	if (pos >= text.size())
		return text.size() + 1;
	size_t next = pos + 1;
	while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80)
		++next;
	return next;
}

template<typename Accessor>
std::vector<MatchState> enumerate_with_accessor(AssDialogue const& line,
                                                SearchReplaceSettings const& settings,
                                                std::string const& prepared_find,
                                                boost::u32regex const* regex,
                                                Accessor& a) {
	std::vector<MatchState> out;

	a.prepare(&line);
	auto const& full = a.full_haystack();
	size_t search_pos = 0;
	bool previous_match_empty = false;

	if (regex) {
		while (search_pos <= full.size()) {
			auto const base_it = full.cbegin();
			auto const start_it = base_it + static_cast<std::ptrdiff_t>(search_pos);
			auto const end_it = full.cend();
			u32_match result;
			u32_iterator const base(base_it, base_it, end_it);
			u32_iterator const search_begin(start_it, base_it, end_it);
			u32_iterator const search_end(end_it, base_it, end_it);
			auto mflags = search_pos > 0
				? (boost::match_not_bol | boost::match_not_bob | boost::match_prev_avail)
				: boost::match_default;
			// Match Boost's regex_iterator: after an empty match, restart at that
			// match's end and reject only another empty match at the same position.
			// regex_search may then find a non-empty match there or a later match.
			if (previous_match_empty)
				mflags |= boost::regex_constants::match_not_initial_null;

			if (!boost::regex_search(search_begin, search_end, result, *regex, mflags, base))
				break;

			size_t const abs_s = static_cast<size_t>(result[0].first.base() - base_it);
			size_t const abs_e = static_cast<size_t>(result[0].second.base() - base_it);
			MatchState ms = a.make_match_state_abs(abs_s, abs_e);
			set_regex_replacements(ms, result, *regex, settings.replace_with);
			search_pos = ms.search_end;
			previous_match_empty = abs_e == abs_s;
			out.push_back(std::move(ms));
		}
		return out;
	}

	// Literal path: walk the searchable surface left-to-right.
	bool const full_match_only = settings.exact_match;
	bool const match_case = settings.match_case;
#ifdef AEGISUB_USE_STRINGZILLA
	agi::util::strings::utf8_icase_searcher icase_searcher{agi::util::strings::view(prepared_find)};
#endif
	while (search_pos <= full.size()) {
		// View from search_pos on the full surface (already NFC/tagless).
		auto const str = agi::util::strings::subview(full, search_pos);
		const agi::util::strings::view look_for_view(prepared_find);
		MatchState ms = bad_match;
		size_t rel_s = 0, rel_e = 0;
		bool found = false;

		if (full_match_only) {
			if (match_case) {
				if (str == look_for_view) {
					rel_s = 0;
					rel_e = str.size();
					found = true;
				}
			}
			else {
#ifdef AEGISUB_USE_STRINGZILLA
				const auto match = icase_searcher.find(str);
				if (match && match.offset == 0 && match.length == str.size()) {
					rel_s = 0;
					rel_e = str.size();
					found = true;
				}
#else
				const auto pos = agi::util::ifind(std::string(str), prepared_find);
				if (pos.first == 0 && pos.second == str.size()) {
					rel_s = pos.first;
					rel_e = pos.second;
					found = true;
				}
#endif
			}
		}
		else if (match_case) {
			const auto pos = agi::util::strings::find(str, look_for_view);
			if (pos != agi::util::strings::npos) {
				rel_s = pos;
				rel_e = pos + look_for_view.size();
				found = true;
			}
		}
		else {
#ifdef AEGISUB_USE_STRINGZILLA
			const auto match = icase_searcher.find(str);
			if (match) {
				rel_s = match.offset;
				rel_e = match.offset + match.length;
				found = true;
			}
#else
			const auto pos = agi::util::ifind(std::string(str), prepared_find);
			if (pos.first != bad_pos) {
				rel_s = pos.first;
				rel_e = pos.second;
				found = true;
			}
#endif
		}

		if (!found)
			break;

		// not_initial_null for literals: skip zero-length (shouldn't happen)
		if (previous_match_empty && rel_s == 0 && rel_e == 0) {
			search_pos = advance_search_utf8(full, search_pos);
			previous_match_empty = false;
			continue;
		}

		ms = a.make_match_state_abs(search_pos + rel_s, search_pos + rel_e);
		out.push_back(ms);

		if (rel_e > rel_s) {
			search_pos = ms.search_end;
			previous_match_empty = false;
		}
		else if (!previous_match_empty) {
			previous_match_empty = true;
		}
		else {
			search_pos = advance_search_utf8(full, search_pos);
			previous_match_empty = false;
		}
	}
	return out;
}

template<typename Accessor>
SubtitleMatchEnumerator get_match_enumerator(SearchReplaceSettings const& settings, Accessor a) {
	std::string search_text = prepare_search_text(settings);
	std::string prepared_find = settings.match_case ? std::move(search_text)
	                                                : boost::locale::normalize(search_text);

	if (settings.use_regex) {
		int flags = boost::u32regex::perl;
		if (!settings.match_case)
			flags |= boost::u32regex::icase;
		auto regex = boost::make_u32regex(prepared_find, flags);
		return [settings, prepared_find = std::move(prepared_find),
		        regex = std::move(regex), a = std::move(a)](AssDialogue const& line) mutable {
			return enumerate_with_accessor(line, settings, prepared_find, &regex, a);
		};
	}

	return [settings, prepared_find = std::move(prepared_find),
	        a = std::move(a)](AssDialogue const& line) mutable {
		return enumerate_with_accessor(line, settings, prepared_find, nullptr, a);
	};
}

} // namespace

std::function<MatchState(const AssDialogue *, size_t)>
MakeSubtitleMatcher(SearchReplaceSettings const& settings) {
	const bool normalize = !settings.match_case;
	if (settings.skip_tags)
		return get_matcher(settings, skip_tags_accessor(settings.field, normalize));
	return get_matcher(settings, noop_accessor(settings.field, normalize));
}

std::function<MatchState(const AssDialogue *, size_t)>
MakeSubtitleMatcher(SearchReplaceSettings const& settings, size_t search_after_empty) {
	const bool normalize = !settings.match_case;
	if (settings.skip_tags)
		return get_matcher(settings, skip_tags_accessor(settings.field, normalize), search_after_empty);
	return get_matcher(settings, noop_accessor(settings.field, normalize), search_after_empty);
}

SubtitleMatchEnumerator
MakeSubtitleMatchEnumerator(SearchReplaceSettings const& settings) {
	const bool normalize = !settings.match_case;
	if (settings.skip_tags)
		return get_match_enumerator(settings, skip_tags_accessor(settings.field, normalize));
	return get_match_enumerator(settings, noop_accessor(settings.field, normalize));
}

std::vector<MatchState>
EnumerateLineMatches(AssDialogue const& line, SearchReplaceSettings const& settings) {
	return MakeSubtitleMatchEnumerator(settings)(line);
}

std::string ExpandSubtitleMatchReplacement(MatchState const& ms,
                                           SearchReplaceSettings const& settings,
                                           SubtitleMatchReplacementScope scope) {
	if (!ms.has_regex_replacement)
		return settings.replace_with;
	return scope == SubtitleMatchReplacementScope::MATCH_ONLY
		? ms.match_only_replacement
		: ms.search_context_replacement;
}
