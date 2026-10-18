#include <main.h>

#include "../../src/subtitle_fps_choice.h"
#include "../../src/translation_service.h"

#include <libaegisub/exception.h>
#include <libaegisub/scope_exit.h>

TEST(subtitle_fps_choice, includes_video_choice_for_loaded_cfr_video) {
	auto fps = agi::vfr::Framerate(25, 1);

	auto model = BuildSubtitleFpsChoiceModel(true, false, fps);

	EXPECT_TRUE(model.includes_video_choice);
	EXPECT_FALSE(model.show_smpte);
	EXPECT_EQ(12u, model.choices.size());
}

TEST(subtitle_fps_choice, build_request_uses_stable_request_id_and_choices) {
	auto fps = agi::vfr::Framerate(25, 1);
	auto model = BuildSubtitleFpsChoiceModel(true, false, fps);

	auto request = BuildSubtitleFpsChoiceRequest(model);

	EXPECT_EQ("subtitle_fps_choice.selection", request.request_id);
	EXPECT_EQ(model.choices, request.choices);
	EXPECT_FALSE(request.title.empty());
	EXPECT_FALSE(request.message.empty());
}

TEST(subtitle_fps_choice, omits_video_choice_for_vfr_when_not_allowed) {
	auto fps = agi::vfr::Framerate({0, 40, 90});

	auto model = BuildSubtitleFpsChoiceModel(false, true, fps);

	EXPECT_FALSE(model.includes_video_choice);
	EXPECT_TRUE(model.show_smpte);
	EXPECT_EQ(12u, model.choices.size());
}

TEST(subtitle_fps_choice, selecting_video_choice_returns_input_fps) {
	auto fps = agi::vfr::Framerate(24000, 1001);
	auto model = BuildSubtitleFpsChoiceModel(true, true, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 0, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_NEAR(fps.FPS(), selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, selecting_dropframe_choice_returns_dropframe_ntsc) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, true, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 5, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_TRUE(selected.NeedsDropFrames());
	EXPECT_NEAR(30000.0 / 1001.0, selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, selecting_last_non_smpte_choice_returns_120fps) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, false, fps);

	auto selected = ResolveSubtitleFpsChoiceSelection(model, 10, fps);

	EXPECT_TRUE(selected.IsLoaded());
	EXPECT_NEAR(120.0, selected.FPS(), 1e-9);
}

TEST(subtitle_fps_choice, rejects_out_of_bounds_choice) {
	auto fps = agi::vfr::Framerate();
	auto model = BuildSubtitleFpsChoiceModel(false, false, fps);

	EXPECT_THROW(ResolveSubtitleFpsChoiceSelection(model, 99, fps), agi::InternalError);
}

namespace {
class VideoDescriptionTranslations final : public TranslationService {
	public:
	[[nodiscard]] std::string Translate(std::string const& msgid) const override {
		return msgid == "VFR, avg %.3f fps" ? "平均帧率 %.3f（VFR）" : msgid;
	}
	[[nodiscard]] std::string TranslatePlural(std::string const& single, std::string const& plural, unsigned long n) const override {
		return n == 1 ? single : plural;
	}
};
}

TEST(subtitle_fps_choice, video_description_translates_vfr_template_before_formatting) {
	VideoDescriptionTranslations translations;
	auto const *previous = TranslationContext::CurrentThreadServiceOverride();
	auto restore = agi::make_scope_exit([previous] {
		if (previous)
			TranslationContext::Set(previous);
		else
			TranslationContext::Reset();
	});
	TranslationContext::Set(&translations);
	auto const fps = agi::vfr::Framerate({0, 400, 1000, 1500});

	EXPECT_EQ("平均帧率 2.000（VFR）", FormatVideoFpsDescription(fps));
	EXPECT_EQ("VFR, avg 2.000 fps", fps.FPSDescription());
}

TEST(subtitle_fps_choice, video_description_preserves_technical_cfr_and_empty_formats) {
	EXPECT_EQ("0.000 fps", FormatVideoFpsDescription(agi::vfr::Framerate()));
	EXPECT_EQ("24000/1001 (23.976 fps)", FormatVideoFpsDescription(agi::vfr::Framerate(24000, 1001)));
	EXPECT_EQ("23.976 fps", FormatVideoFpsDescription(agi::vfr::Framerate(23.976)));
}
