#include "align_video_fade.h"

#include "ass_compat.h"
#include "ass_dialogue.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace aegisub::align_video_fade {
namespace {

double Median(std::span<double const> values) {
	if (values.empty())
		return 0.0;

	std::array<double, 96> local{};
	std::vector<double> dynamic;
	std::span<double> sorted;
	if (values.size() <= local.size()) {
		std::copy(values.begin(), values.end(), local.begin());
		sorted = std::span<double>(local).first(values.size());
	}
	else {
		dynamic.assign(values.begin(), values.end());
		sorted = dynamic;
	}
	auto const middle = sorted.begin() + sorted.size() / 2;
	std::nth_element(sorted.begin(), middle, sorted.end());
	double result = *middle;
	if (sorted.size() % 2 == 0) {
		auto const lower = std::max_element(sorted.begin(), middle);
		result = (*lower + result) * 0.5;
	}
	return result;
}

double Huber(double residual, double delta) {
	double const magnitude = std::abs(residual);
	return magnitude <= delta
		? 0.5 * residual * residual
		: delta * (magnitude - 0.5 * delta);
}

double ModelLoss(
	std::span<double const> values,
	int start,
	int end,
	double gamma,
	std::span<double const> powers,
	double delta) {
	double loss = 0.0;
	for (int i = 0; i < static_cast<int>(values.size()); ++i) {
		double predicted = 0.0;
		if (i >= end)
			predicted = 1.0;
		else if (i > start) {
			double const position = static_cast<double>(i - start) / (end - start);
			predicted = powers.empty() ? std::pow(position, gamma) : powers[i - start - 1];
		}
		loss += Huber(values[i] - predicted, delta);
	}
	return loss / values.size();
}

double HardCutLoss(std::span<double const> values, int split, double delta) {
	double loss = 0.0;
	for (int i = 0; i < static_cast<int>(values.size()); ++i)
		loss += Huber(values[i] - (i >= split ? 1.0 : 0.0), delta);
	return loss / values.size();
}

bool IsFadeTag(AssOverrideTag const& tag) {
	return tag.Name == "\\fad" || tag.Name == "\\fade";
}

bool IsOverallAlphaTag(AssOverrideTag const& tag) {
	return tag.Name == "\\alpha";
}

bool TransformContainsOverallAlpha(AssOverrideTag const& tag) {
	if (tag.Name != "\\t" || tag.Params.size() <= 3 || tag.Params[3].omitted || tag.Params[3].empty)
		return false;

	auto const* nested = tag.Params[3].Get<AssDialogueBlockOverride*>();
	for (auto const& nested_tag : nested->Tags) {
		if (IsOverallAlphaTag(nested_tag) || TransformContainsOverallAlpha(nested_tag))
			return true;
	}
	return false;
}

void GatherOverallAlphaValues(AssDialogueBlockOverride const& block, std::vector<int>& values) {
	for (auto const& tag : block.Tags) {
		if (IsOverallAlphaTag(tag) && !tag.Params.empty() && !tag.Params[0].omitted && !tag.Params[0].empty)
			values.push_back(tag.Params[0].Get<int>());
		if (tag.Name == "\\t" && tag.Params.size() > 3 && !tag.Params[3].omitted && !tag.Params[3].empty)
			GatherOverallAlphaValues(*tag.Params[3].Get<AssDialogueBlockOverride*>(), values);
	}
}

bool RemoveOverallAlphaFromTransform(AssOverrideTag& tag) {
	if (tag.Name != "\\t" || tag.Params.size() <= 3 || tag.Params[3].omitted || tag.Params[3].empty)
		return true;

	auto* nested = tag.Params[3].Get<AssDialogueBlockOverride*>();
	for (auto it = nested->Tags.begin(); it != nested->Tags.end();) {
		if (IsOverallAlphaTag(*it)) {
			it = nested->Tags.erase(it);
			continue;
		}
		if (it->Name == "\\t" && !RemoveOverallAlphaFromTransform(*it)) {
			it = nested->Tags.erase(it);
			continue;
		}
		++it;
	}
	return !nested->Tags.empty();
}

AssDialogueBlockOverride* InitialOverride(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks) {
	if (!blocks.empty() && blocks.front()->GetType() == AssBlockType::OVERRIDE)
		return static_cast<AssDialogueBlockOverride*>(blocks.front().get());

	auto block = std::make_unique<AssDialogueBlockOverride>();
	auto* result = block.get();
	blocks.insert(blocks.begin(), std::move(block));
	return result;
}

// The fade representations ApplyAssFade may claim or replace: any \fad/\fade
// tag anywhere, and the leading overall-alpha \t animation of the initial
// override block -- but only while no other block carries one (an animation
// spanning later blocks is mid-line styling, not a line-wide fade).
// alpha_animation_block is the initial block while that leading animation is
// claimable, nullptr otherwise.
struct ClaimableFade {
	bool has_fade = false; // \fade anywhere
	bool has_fad = false;  // \fad anywhere
	AssDialogueBlockOverride *alpha_animation_block = nullptr;
};

ClaimableFade DetectClaimableFade(
	std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks) {
	ClaimableFade claim;
	bool alpha_animation_in_initial_block = false;
	bool alpha_animation_elsewhere = false;
	AssDialogueBlockOverride *initial = nullptr;
	if (!blocks.empty() && blocks.front()->GetType() == AssBlockType::OVERRIDE)
		initial = static_cast<AssDialogueBlockOverride *>(blocks.front().get());

	for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
		if (blocks[block_index]->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto const *block = static_cast<AssDialogueBlockOverride const *>(blocks[block_index].get());
		for (auto const& tag : block->Tags) {
			claim.has_fade = claim.has_fade || tag.Name == "\\fade";
			claim.has_fad = claim.has_fad || tag.Name == "\\fad";
			if (TransformContainsOverallAlpha(tag)) {
				if (block_index == 0)
					alpha_animation_in_initial_block = true;
				else
					alpha_animation_elsewhere = true;
			}
		}
	}
	if (alpha_animation_in_initial_block && !alpha_animation_elsewhere)
		claim.alpha_animation_block = initial;
	return claim;
}

// Erases the claimable representations in place: every \fad/\fade tag, and
// (inside alpha_animation_block only) the overall-alpha values plus the \t
// animations that carried them. Empty override blocks left behind are
// dropped.
void RemoveClaimableFade(
	std::vector<std::unique_ptr<AssDialogueBlock>>& blocks,
	AssDialogueBlockOverride *alpha_animation_block) {
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		auto *override_block = static_cast<AssDialogueBlockOverride *>(block.get());
		bool const normalize_alpha_animation = override_block == alpha_animation_block;

		for (auto it = override_block->Tags.begin(); it != override_block->Tags.end();) {
			if (IsFadeTag(*it) || (normalize_alpha_animation && IsOverallAlphaTag(*it))) {
				it = override_block->Tags.erase(it);
				continue;
			}
			if (normalize_alpha_animation && it->Name == "\\t" && !RemoveOverallAlphaFromTransform(*it)) {
				it = override_block->Tags.erase(it);
				continue;
			}
			++it;
		}
	}

	blocks.erase(
		std::remove_if(blocks.begin(), blocks.end(), [](auto const& block) {
			return block->GetType() == AssBlockType::OVERRIDE && static_cast<AssDialogueBlockOverride const *>(block.get())->Tags.empty();
		}),
		blocks.end());
}

std::string AlphaTag(int alpha) {
	return "\\alpha" + AssCompat::FormatOverrideAlpha(alpha);
}

std::string TransformAlphaTag(int start, int end, int alpha) {
	return "\\t(" + std::to_string(start) + "," + std::to_string(end)
		+ "," + AlphaTag(alpha) + ")";
}

int RoundToCentisecond(int64_t milliseconds) {
	// Integer division truncates toward zero, so adjust negative values to
	// obtain floor((milliseconds + 5) / 10).
	int64_t const shifted = milliseconds + 5;
	int64_t quotient = shifted / 10;
	if (shifted < 0 && shifted % 10 != 0)
		--quotient;

	int64_t const rounded = quotient * 10;
	constexpr int64_t maximum = std::numeric_limits<int>::max() / 10 * 10;
	constexpr int64_t minimum = std::numeric_limits<int>::min() / 10 * 10;
	return static_cast<int>(std::clamp(rounded, minimum, maximum));
}

} // namespace

CurveFit FitVisibilityCurve(std::span<double const> samples, int plateau_samples) {
	CurveFit result;
	int const count = static_cast<int>(samples.size());
	if (plateau_samples < 2 || count < plateau_samples + 3)
		return result;

	int const outside_count = count - plateau_samples;
	int const baseline_count = std::min(3, outside_count);
	std::vector<double> outside_samples(
		samples.begin(), samples.begin() + outside_count);
	double off = *std::min_element(outside_samples.begin(), outside_samples.end());
	// When the scan reaches a real transparent plateau, use a small median to
	// suppress isolated readback noise. For short fades the first samples can
	// already be on the ramp, so retain the lower envelope instead.
	if (outside_count >= 5) {
		std::vector<double> leading(outside_samples.begin(), outside_samples.begin() + 5);
		std::sort(leading.begin(), leading.end());
		if (leading.back() - leading.front() <= 0.12)
			off = Median(std::span<double const>(leading));
	}
	double const on = Median(samples.last(plateau_samples));
	double const scale = on - off;
	if (scale <= 0.08 || off > 0.35)
		return result;

	std::vector<double> normalized;
	normalized.reserve(samples.size());
	for (double value : samples)
		normalized.push_back(std::clamp((value - off) / scale, -0.5, 1.5));

	std::vector<double> baseline_deviation;
	baseline_deviation.reserve(baseline_count);
	for (int i = 0; i < baseline_count; ++i)
		baseline_deviation.push_back(std::abs(normalized[i]));
	double const noise = 1.4826 * Median(baseline_deviation);
	if (noise > 0.50)
		return result;

	double const delta = std::clamp(3.0 * noise, 0.10, 0.25);
	double hard_loss = 1e30;
	for (int split = 1; split <= outside_count; ++split)
		hard_loss = std::min(hard_loss, HardCutLoss(normalized, split, delta));

	constexpr double gammas[] = { 0.5, 0.75, 1.0, 1.4, 2.0 };
	// Cover the scanner's bounded fade window without quadratic allocation
	// growth for callers supplying longer curves. Longer ramps use pow below.
	int const cached_length = std::min(outside_count, 240);
	std::vector<double> powers;
	powers.reserve(std::size(gammas) * cached_length * (cached_length - 1) / 2);
	for (int length = 2; length <= cached_length; ++length) {
		for (double gamma : gammas) {
			for (int position = 1; position < length; ++position)
				powers.push_back(std::pow(static_cast<double>(position) / length, gamma));
		}
	}

	double best_loss = 1e30;
	int best_start = -1;
	int best_end = -1;
	for (int end = 2; end <= outside_count; ++end) {
		for (int start = 0; start + 2 <= end; ++start) {
			int const length = end - start;
			for (size_t gamma_index = 0; gamma_index < std::size(gammas); ++gamma_index) {
				std::span<double const> model;
				if (length <= cached_length) {
					size_t const offset = std::size(gammas) * (length - 1) * (length - 2) / 2 + gamma_index * (length - 1);
					model = std::span<double const>(powers).subspan(offset, length - 1);
				}
				// Normalize both terms over the sampled window. Otherwise adding
				// transparent confirmation frames penalizes the same fade more.
				double const loss = ModelLoss(normalized, start, end, gammas[gamma_index], model, delta) + 0.0005 * length / count;
				if (loss < best_loss) {
					best_loss = loss;
					best_start = start;
					best_end = end;
				}
			}
		}
	}

	double const improvement = hard_loss - best_loss;
	double const relative_improvement = improvement / std::max(hard_loss, 1e-9);
	if (best_start < 0 || best_end - best_start < 2
		|| improvement < std::max(0.004, noise * noise * 0.2)
		|| relative_improvement < 0.20)
		return result;

	// The fitted zero point is deliberately tolerant of compression noise and
	// can sit inside the transparent lead-in. Report the first persistently
	// visible sample instead, which is the frame the aligned line should begin
	// on. The final plateau is supplied by the caller only after it has been
	// explicitly confirmed, so its first sample is the fade completion point.
	double const visible_threshold = std::clamp(3.0 * noise, 0.01, 0.10);
	int first_visible = -1;
	// Earlier background edges can briefly resemble the foreground. The
	// fitted onset constrains the search to the ramp that reaches the anchor.
	for (int i = best_start; i < outside_count; ++i) {
		if (normalized[i] < visible_threshold)
			continue;
		bool const persists = (i + 1 < count && normalized[i + 1] >= visible_threshold)
			|| (i + 2 < count && normalized[i + 2] >= visible_threshold);
		if (persists) {
			first_visible = i;
			break;
		}
	}
	if (first_visible < 0 || outside_count - first_visible < 2)
		return result;
	// A robust model can place its zero point just inside a faint ramp. Keep
	// up to two earlier visible samples only when they connect to that ramp;
	// a gap still separates an unrelated background edge from the text.
	int const earliest_visible = std::max(0, best_start - 2);
	while (first_visible > earliest_visible && normalized[first_visible - 1] >= visible_threshold)
		--first_visible;

	result.detected = true;
	result.outer_index = first_visible;
	result.inner_index = outside_count;
	result.confidence = std::clamp(
		relative_improvement * (1.0 - std::min(noise, 1.0)),
		0.0,
		1.0);
	return result;
}

PlateauReference BuildPlateauReference(std::span<double const> platform_samples) {
	PlateauReference result;
	if (platform_samples.size() < 2)
		return result;
	double const platform_level = Median(platform_samples);
	if (platform_level <= 0.08)
		return result;

	std::vector<double> deviations;
	deviations.reserve(platform_samples.size());
	for (double sample : platform_samples)
		deviations.push_back(std::abs(sample - platform_level));
	double const robust_noise = 1.4826 * Median(deviations);
	auto const [platform_minimum, platform_maximum] = std::minmax_element(
		platform_samples.begin(),
		platform_samples.end());
	double const platform_spread = *platform_maximum - *platform_minimum;
	result.valid = true;
	result.level = platform_level;
	result.level_band = std::max(
		std::abs(platform_level) * 0.005,
		3.0 * robust_noise);
	result.spread_limit = std::max(
		std::abs(platform_level) * 0.01,
		platform_spread * 1.5);
	return result;
}

int FindConfirmedPlateauStart(
	std::span<double const> samples,
	PlateauReference const& platform,
	int confirmation_samples) {
	if (!platform.valid
		|| confirmation_samples < 2
		|| static_cast<int>(samples.size()) < confirmation_samples)
		return -1;

	for (int start = 0;
		start + confirmation_samples <= static_cast<int>(samples.size());
		++start) {
		auto const window = samples.subspan(start, confirmation_samples);
		auto const [minimum, maximum] = std::minmax_element(window.begin(), window.end());
		double const level = Median(window);
		if (*minimum >= platform.level - platform.level_band
			&& level <= platform.level + platform.level_band
			&& *maximum - *minimum <= platform.spread_limit)
			return start;
	}
	return -1;
}

int FindConfirmedPlateauStart(
	std::span<double const> samples,
	std::span<double const> platform_samples,
	int confirmation_samples) {
	if (static_cast<int>(platform_samples.size()) < confirmation_samples)
		return -1;
	return FindConfirmedPlateauStart(
		samples,
		BuildPlateauReference(platform_samples),
		confirmation_samples);
}

AssFadeTiming BuildAssFadeTiming(
	agi::vfr::Framerate const& timecodes,
	int first_visible_frame,
	int last_visible_frame,
	int fade_in_full_frame,
	int fade_out_full_frame,
	bool fade_in_detected,
	bool fade_out_detected) {
	AssFadeTiming result;
	result.start_ms = timecodes.CheckedTimeAtFrame(first_visible_frame, agi::vfr::START);
	result.end_ms = timecodes.CheckedTimeAtFrame(last_visible_frame, agi::vfr::END);
	int64_t const duration_ms = std::max<int64_t>(int64_t{result.end_ms} - result.start_ms, 0);
	if (duration_ms > std::numeric_limits<int>::max())
		throw std::overflow_error("Aligned subtitle duration is outside the supported range");
	if (fade_in_detected) {
		int64_t const full_sample_ms = timecodes.CheckedTimeAtFrame(fade_in_full_frame, agi::vfr::EXACT);
		result.fade_in_ms = static_cast<int>(std::clamp(full_sample_ms - result.start_ms, int64_t{0}, duration_ms));
	}
	if (fade_out_detected) {
		int64_t const full_sample_ms = timecodes.CheckedTimeAtFrame(fade_out_full_frame, agi::vfr::EXACT);
		result.fade_out_ms = static_cast<int>(std::clamp(
			result.end_ms - full_sample_ms,
			int64_t{0},
			duration_ms - result.fade_in_ms));
	}
	return result;
}

AssFadeTiming RoundAssFadeTimingToCentiseconds(AssFadeTiming timing) {
	int64_t const start = std::max<int64_t>(timing.start_ms, 0);
	int64_t const end = std::max<int64_t>(timing.end_ms, start);
	int64_t const duration = end - start;
	int64_t const fade_in = std::clamp<int64_t>(timing.fade_in_ms, 0, duration);
	int64_t const fade_out = std::clamp<int64_t>(
		timing.fade_out_ms,
		0,
		duration - fade_in);

	int const rounded_start = RoundToCentisecond(start);
	int const rounded_fade_in_end = RoundToCentisecond(start + fade_in);
	int const rounded_fade_out_start = RoundToCentisecond(end - fade_out);
	int const rounded_end = RoundToCentisecond(end);

	return {
		rounded_start,
		rounded_end,
		rounded_fade_in_end - rounded_start,
		rounded_end - rounded_fade_out_start
	};
}

AssFadeUpdate ApplyAssFade(
	std::string const& text,
	int duration_ms,
	int fade_in_ms,
	int fade_out_ms) {
	duration_ms = std::max(duration_ms, 0);
	fade_in_ms = std::clamp(fade_in_ms, 0, duration_ms);
	fade_out_ms = std::clamp(fade_out_ms, 0, duration_ms - fade_in_ms);

	AssDialogue line;
	line.Text = text;
	auto blocks = line.ParseTags();

	auto const claim = DetectClaimableFade(blocks);
	AssFadeEncoding encoding = claim.has_fade
								   ? AssFadeEncoding::Fade
							   : claim.has_fad
								   ? AssFadeEncoding::Fad
							   : claim.alpha_animation_block
								   ? AssFadeEncoding::AlphaTransform
								   : AssFadeEncoding::Fad;

	std::vector<int> alpha_values;
	if (encoding == AssFadeEncoding::AlphaTransform)
		GatherOverallAlphaValues(*claim.alpha_animation_block, alpha_values);

	RemoveClaimableFade(blocks, claim.alpha_animation_block);

	auto* initial = InitialOverride(blocks);
	std::vector<AssOverrideTag> inserted;
	if (encoding == AssFadeEncoding::Fade) {
		inserted.emplace_back(
			"\\fade(255,0,255,0," + std::to_string(fade_in_ms) + ","
			+ std::to_string(duration_ms - fade_out_ms) + ","
			+ std::to_string(duration_ms) + ")");
	}
	else if (encoding == AssFadeEncoding::AlphaTransform) {
		int const middle_alpha = alpha_values.empty()
			? 0
			: *std::min_element(alpha_values.begin(), alpha_values.end());
		inserted.emplace_back(AlphaTag(fade_in_ms > 0 ? 255 : middle_alpha));
		if (fade_in_ms > 0)
			inserted.emplace_back(TransformAlphaTag(0, fade_in_ms, middle_alpha));
		if (fade_out_ms > 0)
			inserted.emplace_back(TransformAlphaTag(
				duration_ms - fade_out_ms,
				duration_ms,
				255));
	}
	else {
		inserted.emplace_back(
			"\\fad(" + std::to_string(fade_in_ms) + ","
			+ std::to_string(fade_out_ms) + ")");
	}

	initial->Tags.insert(
		initial->Tags.begin(),
		std::make_move_iterator(inserted.begin()),
		std::make_move_iterator(inserted.end()));
	line.UpdateText(blocks);
	return { line.Text.get(), encoding };
}

std::string StripClaimableFade(std::string const& text) {
	AssDialogue line;
	line.Text = text;
	auto blocks = line.ParseTags();

	auto const claim = DetectClaimableFade(blocks);
	if (!claim.has_fade && !claim.has_fad && !claim.alpha_animation_block)
		return text;

	RemoveClaimableFade(blocks, claim.alpha_animation_block);
	line.UpdateText(blocks);
	return line.Text.get();
}

} // namespace aegisub::align_video_fade
