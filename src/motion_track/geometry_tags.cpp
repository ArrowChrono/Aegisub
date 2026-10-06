#include "geometry_tags.h"

#include "../ass_tag_scanner.h"

#include <libaegisub/ass/drawing.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace aegisub::motion_track {
namespace {

namespace drawing = agi::ass::drawing;
using ass_tag_scanner::RawTag;
using drawing::PathData;
using drawing::PathVerb;
using drawing::Point;

constexpr int output_scale = 6;
constexpr double output_factor = 32.0;
constexpr double output_quantization_error = 0.00035; // sqrt(2) / (2 * 64 * 32)
constexpr size_t max_curve_points = 65536;

struct HomogeneousPoint {
	double x, y, w;
};

HomogeneousPoint Midpoint(HomogeneousPoint a, HomogeneousPoint b) {
	return {.x = (a.x + b.x) / 2, .y = (a.y + b.y) / 2, .w = (a.w + b.w) / 2};
}

double DistanceToSegment(Point p, Point a, Point b) {
	double const dx = b.x - a.x, dy = b.y - a.y;
	double const length = dx * dx + dy * dy;
	double const t = length > 0
						 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0, 1.0)
						 : 0;
	return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

std::string Number(double value) {
	if (std::abs(value) < 0.0000005)
		value = 0;
	std::array<char, 64> buffer{};
	auto const converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
										 value, std::chars_format::fixed, 6);
	std::string result(buffer.data(), converted.ptr);
	while (result.back() == '0')
		result.pop_back();
	if (result.back() == '.')
		result.pop_back();
	return result;
}

class GeometryTransformer {
	std::array<double, 9> matrix;
	double tolerance;
	GeometryTagResult& result;
	bool affine;
	bool identity;
	size_t clip_count = 0;
	bool converted_rectangle = false;

	bool Fail(std::string message) {
		result.success = false;
		result.error = std::move(message);
		return false;
	}

	[[nodiscard]] HomogeneousPoint Transform(Point p) const {
		return {.x = matrix[0] * p.x + matrix[1] * p.y + matrix[2],
				.y = matrix[3] * p.x + matrix[4] * p.y + matrix[5],
				.w = matrix[6] * p.x + matrix[7] * p.y + matrix[8]};
	}

	bool Project(HomogeneousPoint p, Point& out) {
		if (!std::isfinite(p.w) || std::abs(p.w) < 1e-12)
			return Fail("Clip or origin reaches the projective horizon.");
		out = {.x = p.x / p.w, .y = p.y / p.w};
		// Drawing coordinates are converted to signed fixed-point integers by
		// renderers. Reject unrepresentable output instead of saturating it.
		constexpr double max_coordinate = std::numeric_limits<std::int32_t>::max() / (64.0 * output_factor);
		if (!std::isfinite(out.x) || !std::isfinite(out.y) || std::abs(out.x) > max_coordinate || std::abs(out.y) > max_coordinate)
			return Fail("Transformed clip or origin exceeds the ASS coordinate range.");
		return true;
	}

	bool CheckEdge(Point a, Point b) {
		auto const first = Transform(a), last = Transform(b);
		if (first.w * last.w <= 0)
			return Fail("Clip edge crosses the projective horizon.");
		Point unused;
		return Project(first, unused) && Project(last, unused);
	}

	bool FlattenCubic(std::array<HomogeneousPoint, 4> const& curve,
					  PathData& output, int depth = 0) {
		if (output.commands.size() >= max_curve_points)
			return Fail("Projective clip exceeds the curve subdivision limit.");
		bool const same_sign = std::ranges::all_of(curve, [&](auto const& p) {
			return p.w * curve[0].w > 0;
		});
		if (same_sign) {
			std::array<Point, 4> projected{};
			for (size_t i = 0; i < curve.size(); ++i)
				if (!Project(curve[i], projected[i]))
					return false;
			// With equal-sign weights, a rational Bezier lies in the convex
			// hull of its projected controls. Distance to the chord is convex,
			// so this bounds the entire curve, rather than sampled points.
			double const error = std::max(
				DistanceToSegment(projected[1], projected[0], projected[3]),
				DistanceToSegment(projected[2], projected[0], projected[3]));
			if (error <= tolerance) {
				output.commands.push_back({.verb = PathVerb::LineTo, .p1 = projected[3]});
				return true;
			}
		}
		if (depth == 20)
			return Fail("Projective clip cannot meet the curve tolerance or crosses the horizon.");
		auto const a = Midpoint(curve[0], curve[1]);
		auto const b = Midpoint(curve[1], curve[2]);
		auto const c = Midpoint(curve[2], curve[3]);
		auto const d = Midpoint(a, b), e = Midpoint(b, c);
		auto const middle = Midpoint(d, e);
		return FlattenCubic({curve[0], a, d, middle}, output, depth + 1) && FlattenCubic({middle, e, c, curve[3]}, output, depth + 1);
	}

	bool TransformPath(PathData const& input, PathData& output) {
		Point current{}, start{};
		for (auto const& command : input.commands) {
			auto mapped = command;
			switch (command.verb) {
				case PathVerb::MoveTo:
					current = start = command.p1;
					if (!Project(Transform(command.p1), mapped.p1))
						return false;
					break;
				case PathVerb::LineTo:
					if (!CheckEdge(current, command.p1) || !Project(Transform(command.p1), mapped.p1))
						return false;
					current = command.p1;
					break;
				case PathVerb::CubicTo:
					if (!affine) {
						if (!FlattenCubic({Transform(current), Transform(command.p1),
										   Transform(command.p2), Transform(command.p3)},
										  output))
							return false;
						current = command.p3;
						continue;
					}
					if (!Project(Transform(command.p1), mapped.p1) || !Project(Transform(command.p2), mapped.p2) || !Project(Transform(command.p3), mapped.p3))
						return false;
					current = command.p3;
					break;
				case PathVerb::Close:
					if (!CheckEdge(current, start))
						return false;
					current = start;
					break;
				default:
					return Fail("Unsupported clip drawing curve.");
			}
			output.commands.push_back(mapped);
		}
		return true;
	}

	[[nodiscard]] std::string SerializeClip(PathData path) const {
		path = drawing::TransformPath(std::move(path),
									  {.m11 = output_factor, .m12 = 0, .m21 = 0, .m22 = output_factor, .dx = 0, .dy = 0});
		return std::to_string(output_scale) + "," + drawing::SerializeAssFilled(path);
	}

	bool Clip(std::vector<std::string_view> const& args, bool animated, std::string& replacement) {
		if (args.size() == 4) {
			std::array<Point, 4> corners{{{.x = static_cast<double>(ass_tag_scanner::ArgToInt(args[0])), .y = static_cast<double>(ass_tag_scanner::ArgToInt(args[1]))},
										  {.x = static_cast<double>(ass_tag_scanner::ArgToInt(args[2])), .y = static_cast<double>(ass_tag_scanner::ArgToInt(args[1]))},
										  {.x = static_cast<double>(ass_tag_scanner::ArgToInt(args[2])), .y = static_cast<double>(ass_tag_scanner::ArgToInt(args[3]))},
										  {.x = static_cast<double>(ass_tag_scanner::ArgToInt(args[0])), .y = static_cast<double>(ass_tag_scanner::ArgToInt(args[3]))}}};
			if (!animated && (corners[2].x <= corners[0].x || corners[2].y <= corners[0].y)) {
				replacement = "0,0,0,0";
				return true;
			}
			PathData path;
			path.commands.push_back({.verb = PathVerb::MoveTo, .p1 = corners[0]});
			for (size_t i = 1; i < corners.size(); ++i)
				path.commands.push_back({.verb = PathVerb::LineTo, .p1 = corners[i]});
			path.commands.push_back({.verb = PathVerb::Close});
			PathData transformed;
			if (!TransformPath(path, transformed))
				return false;
			bool const axis_aligned = affine && matrix[1] == 0 && matrix[3] == 0 && matrix[0] > 0 && matrix[4] > 0;
			auto const a = transformed.commands[0].p1, b = transformed.commands[2].p1;
			bool const integral = a.x == std::round(a.x) && a.y == std::round(a.y) && b.x == std::round(b.x) && b.y == std::round(b.y);
			if (axis_aligned && integral) {
				replacement = Number(a.x) + "," + Number(a.y) + "," + Number(b.x) + "," + Number(b.y);
				return true;
			}
			if (animated)
				return Fail("Animated rectangular clip cannot be represented after this transform; vector clips do not animate in ASS.");
			converted_rectangle = true;
			replacement = SerializeClip(std::move(transformed));
			return true;
		}
		if (args.size() != 1 && args.size() != 2)
			return Fail("Unsupported clip argument count.");
		int const scale = args.size() == 2 ? std::max(1, ass_tag_scanner::ArgToInt(args[0])) : 1;
		if (scale > 31)
			return Fail("Unsupported vector clip scale.");
		auto const shape = args.back();
		auto const lexemes = drawing::LexDrawing(shape);
		if (std::ranges::any_of(lexemes, [](auto const& lexeme) {
				return lexeme.type == drawing::LexemeType::Error;
			}))
			return Fail("Malformed vector clip drawing cannot be transformed safely.");
		size_t offset = 0;
		for (auto const& lexeme : lexemes) {
			if (lexeme.type != drawing::LexemeType::Normal && lexeme.type != drawing::LexemeType::Command) {
				double const coordinate = ass_tag_scanner::ArgToDouble(shape.substr(offset, lexeme.length));
				if (std::abs(coordinate) > std::numeric_limits<std::int32_t>::max() / 64.0)
					return Fail("Vector clip input exceeds the ASS coordinate range.");
			}
			offset += lexeme.length;
		}
		auto path = drawing::ParseAss(shape, drawing::AssDrawingCompatMode::Libass);
		if (path.empty()) {
			replacement = args.size() == 2 ? std::string(args[0]) + "," + std::string(shape) : std::string(shape);
			return true;
		}
		double const factor = std::ldexp(1.0, 1 - scale);
		path = drawing::TransformPath(std::move(path), {.m11 = factor, .m12 = 0, .m21 = 0, .m22 = factor, .dx = 0, .dy = 0});
		PathData transformed;
		if (!TransformPath(path, transformed))
			return false;
		replacement = SerializeClip(std::move(transformed));
		return true;
	}

	std::string TransformBody(std::string_view body, int depth) {
		std::string output;
		size_t consumed = 0;
		ass_tag_scanner::ScanRawTags(body, [&](RawTag const& tag) {
			if (!result.success || !tag.has_paren)
				return;
			std::string replacement;
			bool replace = false;
			if (ass_tag_scanner::NameHasPrefix(tag.name, "t")) {
				if (depth >= 32) {
					Fail("Override transform nesting exceeds the supported depth.");
					return;
				}
				replacement = TransformBody(tag.args, depth + 1);
				replace = replacement != tag.args;
			}
			else {
				auto const args = ass_tag_scanner::SplitLibassArgs(tag.args);
				bool const origin = ass_tag_scanner::NameHasPrefix(tag.name, "org") && args.size() == 2;
				bool const clip = (ass_tag_scanner::NameHasPrefix(tag.name, "clip") || ass_tag_scanner::NameHasPrefix(tag.name, "iclip")) && !args.empty();
				if (!origin && !clip)
					return;
				result.has_geometry = true;
				if (clip)
					++clip_count;
				if (clip && depth > 0 && args.size() == 4)
					result.has_animated_clip = true;
				if (identity)
					return;
				if (origin) {
					Point p{.x = ass_tag_scanner::ArgToDouble(args[0]), .y = ass_tag_scanner::ArgToDouble(args[1])}, mapped;
					if (!Project(Transform(p), mapped))
						return;
					replacement = Number(mapped.x) + "," + Number(mapped.y);
				}
				else if (!Clip(args, depth > 0, replacement))
					return;
				replace = true;
			}
			if (replace) {
				auto const begin = static_cast<size_t>(tag.args.data() - body.data());
				output.append(body.substr(consumed, begin - consumed));
				output += replacement;
				consumed = begin + tag.args.size();
			}
		});
		output.append(body.substr(consumed));
		return output;
	}

	public:
	GeometryTransformer(TrackTransform const& transform, double curve_tolerance, GeometryTagResult& result)
		: matrix(transform.matrix), tolerance(curve_tolerance - output_quantization_error), result(result) {
		affine = matrix[6] == 0 && matrix[7] == 0 && matrix[8] != 0;
		identity = affine && matrix[0] == matrix[8] && matrix[4] == matrix[8] && matrix[1] == 0 && matrix[2] == 0 && matrix[3] == 0 && matrix[5] == 0;
	}

	void Run(std::string_view text) {
		if (!std::ranges::all_of(matrix, [](double value) { return std::isfinite(value); }) || !std::isfinite(tolerance) || tolerance <= 0) {
			Fail("Geometry transform or curve tolerance is invalid.");
			return;
		}
		std::string output;
		size_t consumed = 0;
		while (true) {
			size_t const open = text.find('{', consumed);
			if (open == std::string_view::npos)
				break;
			size_t const close = text.find('}', open + 1);
			if (close == std::string_view::npos)
				break;
			output.append(text.substr(consumed, open + 1 - consumed));
			output += TransformBody(text.substr(open + 1, close - open - 1), 0);
			if (!result.success)
				return;
			consumed = close;
		}
		output.append(text.substr(consumed));
		// Rectangular clips and vector clips occupy separate renderer slots;
		// vector clips are first-wins. Converting one of multiple clip tags
		// would change their composition or precedence.
		if (converted_rectangle && clip_count > 1) {
			Fail("Converting a rectangle alongside other clip tags would change ASS clip precedence.");
			return;
		}
		result.text = std::move(output);
	}
};

} // namespace

GeometryTagResult TransformGeometryTags(std::string_view text,
										TrackTransform const& script_transform, double curve_tolerance) {
	GeometryTagResult result;
	result.text = text;
	GeometryTransformer(script_transform, curve_tolerance, result).Run(text);
	return result;
}

namespace {
constexpr double max_affine_coordinate = (std::numeric_limits<std::int32_t>::max() / 64) / output_factor;

bool StrictCoordinate(std::string_view text, double& number) {
	if (!text.empty() && text.front() == '+')
		text.remove_prefix(1);
	auto const parsed = std::from_chars(text.data(), text.data() + text.size(), number);
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(number) && std::abs(number) <= std::numeric_limits<std::int32_t>::max();
}
}

PreparedAffineGeometry::PreparedAffineGeometry(std::string_view text) : source(text) {
	size_t consumed = 0;
	while (true) {
		auto const open = source.find('{', consumed);
		if (open == std::string::npos)
			break;
		auto const close = source.find('}', open + 1);
		if (close == std::string::npos)
			break;
		std::string_view body(source.data() + open + 1, close - open - 1);
		auto scan = [&](auto&& self, std::string_view contents, int depth) -> void {
			ass_tag_scanner::ScanRawTags(contents, [&](ass_tag_scanner::RawTag const& raw) {
				if (ass_tag_scanner::NameHasPrefix(raw.name, "t") && raw.has_paren) {
					if (depth < 32)
						self(self, raw.args, depth + 1);
					else {
						clip_present = true;
						error = "Geometry animation nesting is too deep.";
					}
					return;
				}
				bool const origin = ass_tag_scanner::NameHasPrefix(raw.name, "org");
				bool const clip = ass_tag_scanner::NameHasPrefix(raw.name, "clip") || ass_tag_scanner::NameHasPrefix(raw.name, "iclip");
				origin_present |= origin;
				clip_present |= clip;
				if (!origin && !clip)
					return;
				if (depth > 0) {
					error = "Animated clips or origins cannot be linked. Disable geometry linking to edit this line.";
					return;
				}
				Tag tag{.begin = static_cast<size_t>(raw.args.data() - source.data()), .length = raw.args.size(), .origin = origin};
				auto const args = ass_tag_scanner::SplitLibassArgs(raw.args);
				if (!raw.has_paren || (origin && args.size() != 2)) {
					error = "Malformed origin or clip.";
					return;
				}
				tag.rectangle = clip && args.size() == 4;
				if (origin || tag.rectangle) {
					for (auto arg : args) {
						double value = 0;
						if (!StrictCoordinate(arg, value))
							error = "Malformed origin or rectangular clip coordinates.";
						tag.coordinates.push_back({.begin = 0, .length = 0, .value = tag.rectangle ? std::trunc(value) : value});
					}
				}
				else {
					if (args.size() != 1 && args.size() != 2) {
						error = "Unsupported clip argument count.";
						return;
					}
					double factor = 1.0;
					if (args.size() == 2) {
						double scale = 0;
						if (!StrictCoordinate(args[0], scale) || scale < 1 || scale > 31 || scale != std::round(scale)) {
							error = "Unsupported vector clip scale.";
							return;
						}
						factor = std::ldexp(1.0, 1 - static_cast<int>(scale));
					}
					tag.shape = args.back();
					auto const lexemes = drawing::LexDrawing(tag.shape);
					if (std::ranges::any_of(lexemes, [](auto const& lexeme) { return lexeme.type == drawing::LexemeType::Error; }))
						error = "Malformed vector clip drawing.";
					size_t offset = 0;
					for (auto lexeme : lexemes) {
						size_t const end = offset + lexeme.length;
						if (lexeme.type == drawing::LexemeType::Normal || lexeme.type == drawing::LexemeType::Command || lexeme.type == drawing::LexemeType::Error) {
							offset = end;
							continue;
						}
						while (offset < end) {
							char const ch = tag.shape[offset];
							if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
								++offset;
								continue;
							}
							double value = 0;
							auto const *begin = tag.shape.data() + offset;
							auto const *number_begin = begin + (ch == '+');
							auto const parsed = std::from_chars(number_begin, tag.shape.data() + end, value);
							if (parsed.ec != std::errc{} || parsed.ptr == number_begin || !std::isfinite(value) || std::abs(value) > std::numeric_limits<std::int32_t>::max() / 64.0) {
								error = "Malformed vector clip drawing.";
								offset = end;
								break;
							}
							double const fixed = std::trunc(value * 64);
							if (fixed != std::nearbyint(value * 64))
								error = "Vector clip coordinates cannot be represented consistently by libass and VSFilter.";
							auto const length = static_cast<size_t>(parsed.ptr - begin);
							tag.coordinates.push_back({.begin = offset, .length = length, .value = fixed / 64 * factor});
							offset += length;
						}
					}
					if (tag.coordinates.size() % 2)
						error = "Incomplete vector clip coordinate pair.";
				}
				tags.push_back(std::move(tag));
			});
		};
		scan(scan, body, 0);
		consumed = close + 1;
	}
}

bool PreparedAffineGeometry::HasOrigin() const {
	return origin_present;
}
bool PreparedAffineGeometry::HasClip() const {
	return clip_present;
}

GeometryTagResult PreparedAffineGeometry::Transform(TrackTransform const& transform, bool origins) const {
	GeometryTagResult result{.text = source};
	auto fail = [&](std::string message) {
		result.success = false;
		result.error = std::move(message);
		return result;
	};
	if (!error.empty())
		return fail(error);
	auto m = transform.matrix;
	for (auto& value : m)
		if (std::abs(value - std::round(value)) < 1e-10)
			value = std::round(value);
	if (!std::ranges::all_of(m, [](double value) { return std::isfinite(value); }) || m[6] != 0 || m[7] != 0 || m[8] != 1)
		return fail("Geometry linking requires a finite affine transform.");
	bool const identity = m == std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1};
	if (identity)
		return result;
	auto project = [&](double x, double y, double& nx, double& ny) {
		nx = m[0] * x + m[1] * y + m[2];
		ny = m[3] * x + m[4] * y + m[5];
		return std::isfinite(nx) && std::isfinite(ny) && std::abs(nx) <= max_affine_coordinate && std::abs(ny) <= max_affine_coordinate;
	};
	size_t consumed = 0;
	bool converted_rectangle = false;
	auto const clip_count = static_cast<size_t>(std::ranges::count_if(tags, [](Tag const& tag) { return !tag.origin; }));
	std::string output;
	output.reserve(source.size());
	for (auto const& tag : tags) {
		if (tag.origin && !origins)
			continue;
		result.has_geometry = true;
		bool const empty_rectangle = tag.rectangle && (tag.coordinates[2].value <= tag.coordinates[0].value || tag.coordinates[3].value <= tag.coordinates[1].value);
		std::vector<double> points;
		points.reserve(tag.coordinates.size());
		for (size_t i = 0; !empty_rectangle && i < tag.coordinates.size(); i += 2) {
			double const x = tag.coordinates[i].value, y = tag.coordinates[i + 1].value;
			double nx, ny;
			if (!project(x, y, nx, ny))
				return fail("Transformed clip or origin exceeds the ASS coordinate range.");
			points.push_back(nx);
			points.push_back(ny);
		}
		std::string replacement;
		if (tag.origin)
			replacement = Number(points[0]) + "," + Number(points[1]);
		else if (tag.rectangle) {
			if (empty_rectangle)
				replacement = "0,0,0,0";
			else if (m[1] == 0 && m[3] == 0 && m[0] > 0 && m[4] > 0 && std::ranges::all_of(points, [](double v) { return std::abs(v - std::round(v)) < 1e-8; }))
				replacement = Number(std::round(points[0])) + "," + Number(std::round(points[1])) + "," + Number(std::round(points[2])) + "," + Number(std::round(points[3]));
			else {
				converted_rectangle = true;
				replacement = std::to_string(output_scale) + ",m";
				bool first = true;
				for (auto pair : std::array<std::array<double, 2>, 4>{{{tag.coordinates[0].value, tag.coordinates[1].value}, {tag.coordinates[2].value, tag.coordinates[1].value}, {tag.coordinates[2].value, tag.coordinates[3].value}, {tag.coordinates[0].value, tag.coordinates[3].value}}}) {
					double x, y;
					if (!project(pair[0], pair[1], x, y))
						return fail("Transformed clip or origin exceeds the ASS coordinate range.");
					replacement += " " + std::to_string(std::llround(x * output_factor)) + " " + std::to_string(std::llround(y * output_factor));
					if (first) {
						replacement += " l";
						first = false;
					}
				}
			}
		}
		else if (points.empty())
			replacement = source.substr(tag.begin, tag.length);
		else {
			replacement = std::to_string(output_scale) + ",";
			size_t shape_consumed = 0;
			for (size_t i = 0; i < tag.coordinates.size(); ++i) {
				auto const& coordinate = tag.coordinates[i];
				replacement.append(tag.shape, shape_consumed, coordinate.begin - shape_consumed);
				replacement += std::to_string(std::llround(points[i] * output_factor));
				shape_consumed = coordinate.begin + coordinate.length;
			}
			replacement.append(tag.shape, shape_consumed);
		}
		output.append(source, consumed, tag.begin - consumed);
		output += replacement;
		consumed = tag.begin + tag.length;
	}
	if (converted_rectangle && clip_count > 1)
		return fail("Converting a rectangle alongside other clip tags would change ASS clip precedence.");
	output.append(source, consumed);
	result.text = std::move(output);
	return result;
}
}
