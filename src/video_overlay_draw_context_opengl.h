#pragma once

#include "video_overlay_draw_context.h"

class OpenGLWrapper;

class OpenGLWindowOverlayPass {
	public:
	class Canvas {
		friend class OpenGLWindowOverlayPass;
		double logical_width, logical_height, depth, max_matrix;
		Canvas(double width, double height, double projection_depth, double maximum)
			: logical_width(width), logical_height(height), depth(projection_depth), max_matrix(maximum) {}
	};

	private:
	friend class OpenGLVideoOverlayDrawContext;
	double logical_width, logical_height, depth, max_matrix;
	bool alpha_write;
	bool active = false;

	void Initialize(int max_clip_planes);
	void Restore(bool restore_batch_baseline = false) noexcept;

	public:
	static Canvas SetupCanvas(int physical_width, int physical_height,
							  double logical_width, double logical_height, double depth);
	explicit OpenGLWindowOverlayPass(Canvas const& canvas, bool alpha_write, int max_clip_planes);
	OpenGLWindowOverlayPass(int physical_width, int physical_height,
							double logical_width, double logical_height, double depth, bool alpha_write, int max_clip_planes);
	~OpenGLWindowOverlayPass() noexcept;
	OpenGLWindowOverlayPass(OpenGLWindowOverlayPass const&) = delete;
	OpenGLWindowOverlayPass& operator=(OpenGLWindowOverlayPass const&) = delete;
	OpenGLWindowOverlayPass(OpenGLWindowOverlayPass&&) = delete;
	OpenGLWindowOverlayPass& operator=(OpenGLWindowOverlayPass&&) = delete;
	void Finish();
};

class OpenGLVideoOverlayDrawContext : public VideoOverlayDrawContext {
	OpenGLWrapper& gl;
	OpenGLWindowOverlayPass *window_pass = nullptr;
	void ValidatePerspectiveTransform(VideoOverlayPerspectiveTransform const& transform, int width) const;
	void SubmitPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const> grid,
								  std::span<VideoOverlayPerspectiveVertex const> axes, std::span<VideoOverlayPerspectiveVertex const> arrows,
								  VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, agi::Color const& secondary,
								  int width, float max_vertex, std::span<float const> colours);

	public:
	explicit OpenGLVideoOverlayDrawContext(OpenGLWrapper& gl);
	OpenGLVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLWindowOverlayPass& pass);
	void SetLineColour(agi::Color const& colour, float alpha = 1.0f, int width = 1) override;
	void SetFillColour(agi::Color const& colour, float alpha = 1.0f) override;
	void SetInvert() override;
	void ClearInvert() override;
	void DrawPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const> grid,
								std::span<VideoOverlayPerspectiveVertex const> axes, std::span<VideoOverlayPerspectiveVertex const> arrows,
								VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, agi::Color const& secondary, int width) override;
	void DrawPreparedPerspectiveBatches(VideoOverlayPreparedPerspectiveBatches const& prepared,
										VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, int width) override;
	void DrawLine(Vector2D p1, Vector2D p2) override;
	void DrawLines(size_t dim, float const *lines, size_t n) override;
	void DrawLineStrip(Vector2D const *points, size_t n) override;
	void DrawRectangle(Vector2D p1, Vector2D p2) override;
	void DrawPolygon(Vector2D const *points, size_t n) override;
	void DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) override;
	void DrawCircle(Vector2D center, float radius) override;
	void DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) override;
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};

class OpenGLTextVideoOverlayDrawContext final : public OpenGLVideoOverlayDrawContext {
	VideoOverlayTextRenderer& text_renderer;

	public:
	OpenGLTextVideoOverlayDrawContext(OpenGLWrapper& gl, VideoOverlayTextRenderer& text);
	OpenGLTextVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLWindowOverlayPass& pass, VideoOverlayTextRenderer& text);
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};
