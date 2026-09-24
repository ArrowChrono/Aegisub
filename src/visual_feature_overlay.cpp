#include "visual_feature.h"

#include "video_overlay_draw_context.h"

bool VisualDraggableFeature::IsMouseOver(Vector2D mouse_pos) const {
	if (!pos)
		return false;

	Vector2D delta = mouse_pos - pos;

	switch (type) {
		case DRAG_BIG_SQUARE:
			return fabs(delta.X()) < 6 && fabs(delta.Y()) < 6;

		case DRAG_BIG_CIRCLE:
			return delta.SquareLen() < 36;

		case DRAG_BIG_TRIANGLE: {
			if (delta.Y() < -10 || delta.Y() > 6)
				return false;
			float dy = delta.Y() - 6;
			return 16 * delta.X() + 9 * dy < 0 && 16 * delta.X() - 9 * dy > 0;
		}

		case DRAG_SMALL_SQUARE:
			return fabs(delta.X()) < 3 && fabs(delta.Y()) < 3;

		case DRAG_SMALL_CIRCLE:
			return delta.SquareLen() < 9;

		default:
			return false;
	}
}

void VisualDraggableFeature::Draw(VideoOverlayDrawContext& context) const {
	if (!pos)
		return;

	switch (type) {
		case DRAG_BIG_SQUARE:
			context.DrawRectangle(pos - 6, pos + 6);
			context.DrawLine(pos - Vector2D(0, 12), pos + Vector2D(0, 12));
			context.DrawLine(pos - Vector2D(12, 0), pos + Vector2D(12, 0));
			break;

		case DRAG_BIG_CIRCLE:
			context.DrawCircle(pos, 6);
			context.DrawLine(pos - Vector2D(0, 12), pos + Vector2D(0, 12));
			context.DrawLine(pos - Vector2D(12, 0), pos + Vector2D(12, 0));
			break;

		case DRAG_BIG_TRIANGLE:
			context.DrawTriangle(pos - Vector2D(9, 6), pos + Vector2D(9, -6), pos + Vector2D(0, 10));
			context.DrawLine(pos, pos + Vector2D(0, -16));
			context.DrawLine(pos, pos + Vector2D(-14, 8));
			context.DrawLine(pos, pos + Vector2D(14, 8));
			break;

		case DRAG_SMALL_SQUARE:
			context.DrawRectangle(pos - 3, pos + 3);
			break;

		case DRAG_SMALL_CIRCLE:
			context.DrawCircle(pos, 3);
			break;
		default:
			break;
	}
}
