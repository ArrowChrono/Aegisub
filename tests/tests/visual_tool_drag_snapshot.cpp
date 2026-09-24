#include <main.h>

#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_tool_drag_snapshot.h"
#include "../../src/visual_tool_presentation.h"

#include <initializer_list>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {
constexpr unsigned long GridColour = 0x00332211;
constexpr unsigned long LinkColour = 0x00665544;

enum class Kind : std::uint8_t { Line,
								 Lines,
								 Rectangle,
								 Circle,
								 Triangle };

struct Stroke {
	unsigned long colour = 0;
	float alpha = 0;
	int width = 0;
};

struct Fill {
	unsigned long colour = 0;
	float alpha = 0;
};

struct Command {
	Kind kind;
	std::vector<Vector2D> points;
	float radius = 0;
	Stroke stroke;
	Fill fill;
};

class Recorder final : public VideoOverlayDrawContext {
	Stroke stroke;
	Fill fill;

	void Record(Kind kind, std::vector<Vector2D> points, float radius = 0) {
		commands.push_back({.kind = kind, .points = std::move(points), .radius = radius, .stroke = stroke, .fill = fill});
	}

	public:
	std::vector<Command> commands;
	int line_changes = 0;
	int fill_changes = 0;

	void SetLineColour(wxColour const& colour, float alpha, int width) override {
		stroke = {.colour = colour.GetRGB(), .alpha = alpha, .width = width};
		++line_changes;
	}
	void SetFillColour(wxColour const& colour, float alpha) override {
		fill = {.colour = colour.GetRGB(), .alpha = alpha};
		++fill_changes;
	}
	void DrawLine(Vector2D p1, Vector2D p2) override { Record(Kind::Line, {p1, p2}); }
	void DrawRectangle(Vector2D p1, Vector2D p2) override { Record(Kind::Rectangle, {p1, p2}); }
	void DrawCircle(Vector2D center, float radius) override { Record(Kind::Circle, {center}, radius); }
	void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) override { Record(Kind::Triangle, {p1, p2, p3}); }
	void DrawLines(size_t dim, float const *lines, size_t n) override {
		ASSERT_EQ(2u, dim);
		ASSERT_NE(nullptr, lines);
		std::vector<Vector2D> points;
		points.reserve(n);
		for (size_t i = 0; i < n; ++i)
			points.emplace_back(lines[i * 2], lines[i * 2 + 1]);
		Record(Kind::Lines, std::move(points));
	}
	void SetInvert() override { ADD_FAILURE() << "Unexpected invert"; }
	void ClearInvert() override { ADD_FAILURE() << "Unexpected clear invert"; }
	void DrawLineStrip(Vector2D const *, size_t) override { ADD_FAILURE() << "Unexpected line strip"; }
	void DrawPolygon(Vector2D const *, size_t) override { ADD_FAILURE() << "Unexpected polygon"; }
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override {
		ADD_FAILURE() << "Unexpected multi-polygon";
	}
	wxSize MeasureText(std::string const&, VideoOverlayTextStyle const&) override {
		ADD_FAILURE() << "Unexpected text measurement";
		return {};
	}
	void DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) override { ADD_FAILURE() << "Unexpected text"; }
};

VisualToolDragSnapshot Snapshot() {
	VisualToolDragSnapshot snapshot(std::make_shared<const VisualToolRenderContext>());
	snapshot.grid_colour = GridColour;
	snapshot.line_colour = LinkColour;
	return snapshot;
}

void ExpectCommand(Command const& command, Kind kind, std::initializer_list<Vector2D> points,
				   unsigned long fill, unsigned long line = GridColour, float alpha = 1.0f, int width = 1, float radius = 0) {
	EXPECT_EQ(kind, command.kind);
	ASSERT_EQ(points.size(), command.points.size());
	size_t index = 0;
	for (auto point : points) {
		EXPECT_FLOAT_EQ(point.X(), command.points[index].X());
		EXPECT_FLOAT_EQ(point.Y(), command.points[index].Y());
		++index;
	}
	EXPECT_EQ(line, command.stroke.colour);
	EXPECT_FLOAT_EQ(alpha, command.stroke.alpha);
	EXPECT_EQ(width, command.stroke.width);
	EXPECT_EQ(fill, command.fill.colour);
	EXPECT_FLOAT_EQ(0.3f, command.fill.alpha);
	EXPECT_FLOAT_EQ(radius, command.radius);
}
}

TEST(visual_tool_drag_snapshot, empty_snapshot_emits_no_primitives_on_repeated_draw) {
	auto const snapshot = Snapshot();
	Recorder recorder;
	snapshot.Draw(recorder);
	snapshot.Draw(recorder);
	EXPECT_TRUE(recorder.commands.empty());
	EXPECT_EQ(2, recorder.line_changes);
	EXPECT_EQ(0, recorder.fill_changes);
}

TEST(visual_tool_drag_snapshot, real_marker_geometry_retains_each_fill_and_outline_style) {
	auto snapshot = Snapshot();
	snapshot.features = {
		{.type = DRAG_BIG_SQUARE, .pos = {40, 50}, .parent = std::nullopt, .fill_colour = 0x00123456},
		{.type = DRAG_BIG_CIRCLE, .pos = {80, 90}, .parent = std::nullopt, .fill_colour = 0x00234567},
		{.type = DRAG_BIG_TRIANGLE, .pos = {120, 130}, .parent = std::nullopt, .fill_colour = 0x00345678}};
	Recorder recorder;
	snapshot.Draw(recorder);
	ASSERT_EQ(10u, recorder.commands.size());
	EXPECT_EQ(1, recorder.line_changes);
	EXPECT_EQ(3, recorder.fill_changes);
	ExpectCommand(recorder.commands[0], Kind::Rectangle, {{34, 44}, {46, 56}}, 0x00123456);
	ExpectCommand(recorder.commands[1], Kind::Line, {{40, 38}, {40, 62}}, 0x00123456);
	ExpectCommand(recorder.commands[2], Kind::Line, {{28, 50}, {52, 50}}, 0x00123456);
	ExpectCommand(recorder.commands[3], Kind::Circle, {{80, 90}}, 0x00234567, GridColour, 1.0f, 1, 6);
	ExpectCommand(recorder.commands[4], Kind::Line, {{80, 78}, {80, 102}}, 0x00234567);
	ExpectCommand(recorder.commands[5], Kind::Line, {{68, 90}, {92, 90}}, 0x00234567);
	ExpectCommand(recorder.commands[6], Kind::Triangle, {{111, 124}, {129, 124}, {120, 140}}, 0x00345678);
	ExpectCommand(recorder.commands[7], Kind::Line, {{120, 130}, {120, 114}}, 0x00345678);
	ExpectCommand(recorder.commands[8], Kind::Line, {{120, 130}, {106, 138}}, 0x00345678);
	ExpectCommand(recorder.commands[9], Kind::Line, {{120, 130}, {134, 138}}, 0x00345678);
}

TEST(visual_tool_drag_snapshot, copied_parent_and_marker_values_survive_source_mutation_and_repeat) {
	auto snapshot = Snapshot();
	Vector2D parent(10, 20);
	VisualToolDragSnapshot::Feature source{.type = DRAG_BIG_CIRCLE, .pos = {40, 60}, .parent = parent, .fill_colour = 0x00445566};
	snapshot.features.push_back(source);
	parent = {500, 600};
	source.pos = {700, 800};
	source.parent = parent;
	source.fill_colour = 0;
	Recorder recorder;
	for (int draw = 0; draw < 2; ++draw) {
		recorder.commands.clear();
		snapshot.Draw(recorder);
		ASSERT_EQ(5u, recorder.commands.size());
		ExpectCommand(recorder.commands[0], Kind::Circle, {{40, 60}}, 0x00445566, GridColour, 1.0f, 1, 6);
		ExpectCommand(recorder.commands[1], Kind::Line, {{40, 48}, {40, 72}}, 0x00445566);
		ExpectCommand(recorder.commands[2], Kind::Line, {{28, 60}, {52, 60}}, 0x00445566);
		ExpectCommand(recorder.commands[3], Kind::Line, {{16, 28}, {28, 44}}, 0x00445566, LinkColour, 0.8f, 2);
		ExpectCommand(recorder.commands[4], Kind::Triangle, {{34, 52}, {24.8f, 46.4f}, {31.2f, 41.6f}}, 0x00445566, LinkColour, 0.8f, 2);
	}
}

TEST(visual_tool_drag_snapshot, start_square_ignores_parent_and_origin_batches_exact_dashes) {
	auto snapshot = Snapshot();
	snapshot.features = {
		{.type = DRAG_BIG_SQUARE, .pos = {90, 20}, .parent = Vector2D(10, 20), .fill_colour = 0x00556677},
		{.type = DRAG_BIG_TRIANGLE, .pos = {50, 20}, .parent = Vector2D(10, 20), .fill_colour = 0x00667788}};
	Recorder recorder;
	snapshot.Draw(recorder);
	ASSERT_EQ(8u, recorder.commands.size());
	ExpectCommand(recorder.commands[0], Kind::Rectangle, {{84, 14}, {96, 26}}, 0x00556677);
	ExpectCommand(recorder.commands[1], Kind::Line, {{90, 8}, {90, 32}}, 0x00556677);
	ExpectCommand(recorder.commands[2], Kind::Line, {{78, 20}, {102, 20}}, 0x00556677);
	ExpectCommand(recorder.commands[3], Kind::Triangle, {{41, 14}, {59, 14}, {50, 30}}, 0x00667788);
	ExpectCommand(recorder.commands[4], Kind::Line, {{50, 20}, {50, 4}}, 0x00667788);
	ExpectCommand(recorder.commands[5], Kind::Line, {{50, 20}, {36, 28}}, 0x00667788);
	ExpectCommand(recorder.commands[6], Kind::Line, {{50, 20}, {64, 28}}, 0x00667788);
	ExpectCommand(recorder.commands[7], Kind::Lines, {{20, 20}, {26, 20}, {32, 20}, {38, 20}}, 0x00667788, LinkColour, 0.5f, 2);
}

TEST(visual_tool_drag_snapshot, arrow_cutoff_preserves_below_at_and_above_boundary) {
	struct Case {
		int x;
		size_t commands;
		int end_x;
		int tip_x;
	};
	for (auto const test : {Case{.x = 39, .commands = 3, .end_x = 0, .tip_x = 0}, Case{.x = 40, .commands = 5, .end_x = 20, .tip_x = 30}, Case{.x = 41, .commands = 5, .end_x = 21, .tip_x = 31}}) {
		SCOPED_TRACE(test.x);
		auto snapshot = Snapshot();
		snapshot.features = {{.type = DRAG_BIG_CIRCLE, .pos = {test.x, 20}, .parent = Vector2D(10, 20), .fill_colour = 0x00778899}};
		Recorder recorder;
		snapshot.Draw(recorder);
		ASSERT_EQ(test.commands, recorder.commands.size());
		if (test.commands == 5) {
			ExpectCommand(recorder.commands[3], Kind::Line, {{20, 20}, {test.end_x, 20}}, 0x00778899, LinkColour, 0.8f, 2);
			ExpectCommand(recorder.commands[4], Kind::Triangle, {{test.tip_x, 20}, {test.end_x, 24}, {test.end_x, 16}}, 0x00778899, LinkColour, 0.8f, 2);
		}
	}
}

TEST(visual_tool_drag_snapshot, dashed_cutoff_has_no_empty_batch_and_keeps_short_final_segment) {
	struct Case {
		int x;
		size_t commands;
		int line_changes;
	};
	for (auto const test : {Case{.x = 29, .commands = 4, .line_changes = 1}, Case{.x = 30, .commands = 4, .line_changes = 2}, Case{.x = 31, .commands = 5, .line_changes = 2}}) {
		SCOPED_TRACE(test.x);
		auto snapshot = Snapshot();
		snapshot.features = {{.type = DRAG_BIG_TRIANGLE, .pos = {test.x, 20}, .parent = Vector2D(10, 20), .fill_colour = 0x008899AA}};
		Recorder recorder;
		snapshot.Draw(recorder);
		ASSERT_EQ(test.commands, recorder.commands.size());
		EXPECT_EQ(test.line_changes, recorder.line_changes);
		if (test.commands == 5)
			ExpectCommand(recorder.commands[4], Kind::Lines, {{20, 20}, {21, 20}}, 0x008899AA, LinkColour, 0.5f, 2);
	}
}

TEST(visual_tool_drag_snapshot, completed_final_cannot_draw_released_colour_for_next_press) {
	VisualToolPresentation presentation;
	auto released = std::make_shared<VisualToolDragSnapshot>(presentation.Context());
	released->grid_colour = GridColour;
	released->features = {{.type = DRAG_BIG_SQUARE, .pos = {40, 50}, .parent = std::nullopt, .fill_colour = 0x00112233}};
	ASSERT_TRUE(presentation.Begin(12, released));
	ASSERT_TRUE(presentation.OnFinalPresented(12));
	auto pressed = std::make_shared<VisualToolDragSnapshot>(presentation.Context());
	pressed->grid_colour = GridColour;
	pressed->features = {{.type = DRAG_BIG_SQUARE, .pos = {40, 50}, .parent = std::nullopt, .fill_colour = 0x00445566}};
	ASSERT_TRUE(presentation.Begin(13, pressed));
	EXPECT_FALSE(presentation.OnFinalPresented(12));
	auto const selected = presentation.Select(released);
	ASSERT_NE(nullptr, selected);
	Recorder recorder;
	selected->Draw(recorder);
	ASSERT_EQ(3u, recorder.commands.size());
	ExpectCommand(recorder.commands[0], Kind::Rectangle, {{34, 44}, {46, 56}}, 0x00445566);
	ExpectCommand(recorder.commands[1], Kind::Line, {{40, 38}, {40, 62}}, 0x00445566);
	ExpectCommand(recorder.commands[2], Kind::Line, {{28, 50}, {52, 50}}, 0x00445566);
}

TEST(visual_tool_drag_snapshot, shared_feature_draw_keeps_small_geometry_and_skips_invalid_features) {
	Recorder recorder;
	recorder.SetLineColour(wxColour(GridColour), 1.0f, 1);
	recorder.SetFillColour(wxColour(0x00123456UL), 0.3f);
	VisualDraggableFeature square;
	square.type = DRAG_SMALL_SQUARE;
	square.pos = {25, 35};
	square.Draw(recorder);
	ASSERT_EQ(1u, recorder.commands.size());
	ExpectCommand(recorder.commands[0], Kind::Rectangle, {{22, 32}, {28, 38}}, 0x00123456);

	recorder.SetLineColour(wxColour(LinkColour), 0.8f, 2);
	recorder.SetFillColour(wxColour(0x00654321UL), 0.3f);
	VisualDraggableFeature circle;
	circle.type = DRAG_SMALL_CIRCLE;
	circle.pos = {60, 70};
	circle.Draw(recorder);
	ASSERT_EQ(2u, recorder.commands.size());
	ExpectCommand(recorder.commands[1], Kind::Circle, {{60, 70}}, 0x00654321, LinkColour, 0.8f, 2, 3);

	VisualDraggableFeature invalid_position;
	invalid_position.type = DRAG_SMALL_CIRCLE;
	invalid_position.Draw(recorder);
	VisualDraggableFeature no_shape;
	no_shape.pos = {100, 110};
	no_shape.Draw(recorder);
	ASSERT_EQ(2u, recorder.commands.size());
	EXPECT_EQ(2, recorder.line_changes);
	EXPECT_EQ(2, recorder.fill_changes);
	ExpectCommand(recorder.commands[0], Kind::Rectangle, {{22, 32}, {28, 38}}, 0x00123456);
	ExpectCommand(recorder.commands[1], Kind::Circle, {{60, 70}}, 0x00654321, LinkColour, 0.8f, 2, 3);
}
