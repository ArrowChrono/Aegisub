# Motion tracking

Motion Track follows one rectangular image patch through the selected subtitle
time range. Choose a clearly visible reference frame, draw the ROI, select a
model and direction, then Analyze. The overlay shows the tracked quadrilateral;
adjusting the ROI and analyzing again continues from the new reference.
Changing the tracking model or direction keeps the old trajectory available, but
Apply and Plan preview require Analyze with the new settings. Switching back to
the original settings restores access to the existing trajectory. A fresh analysis
uses a fresh ROI pose; continuing an existing session retains its accumulated pose.

## Models

| Model | Motion |
| --- | --- |
| Translation | Horizontal and vertical movement, with subpixel refinement |
| Similarity | Translation, in-plane rotation and uniform scale |
| Affine | Translation, rotation, shear and independent horizontal/vertical scale |
| Perspective | A planar homography: perspective deformation of all four ROI corners |

Similarity retains the subpixel position prior and refines its pose against the
frozen reference patch. Bounded backtracking accepts only an improved fit on the
same pixels; an inner sampling margin avoids mixing in the unknown background at
the reference boundary after resampling. An initial fit keeps sparse motion edges
available and is accepted only when it reduces the robust loss on the same pixels.
Robust refinement excludes gradients beside rejected pixels and fails if no
gradient remains on either translation axis. The search crop expands with the
estimated rotation and scale. These changes reduce numerical oscillation without
temporally smoothing away real motion or delaying
direction changes; the existing confidence, residual and jump limits still apply.

Affine and Perspective use direct grayscale registration against the reference
patch. They need texture and sufficiently small changes between adjacent frames.
They do not recognize object identities, estimate 3D depth, or follow independent
objects within one ROI. Partial occlusion is handled with robust residual weights;
severe occlusion, an invalid projection, or an excessive jump fails the frame.
Three consecutive failed frames stop that direction. Forward and backward passes
have independent motion priors.

Translation additionally supports template refresh, duplicate-frame handling and
fade detection/recovery. These options do not apply to the other backends.

## ASS output

Exact writes a static pose for each frame and merges adjacent identical output.
Compact fits motion in actual video time, including variable-frame-rate timecodes.
Its error setting is measured in video pixels, independent of PlayRes. The default
is **1 video pixel** (`100` in the dialog's 1/100-pixel control). Existing saved
Compact error values are retained; upgrading does not reset them to the new
default. Position and pose interpolation are checked separately for
Translation/Similarity. Full geometry interpolation is checked by evaluating the
generated ASS at every tracked frame and measuring the projected geometry in video
pixels.
Compact animation windows use ASS's serialized centisecond event times. If the
selected position precision cannot meet the error budget, Apply rejects the plan
and requests more position decimals or a larger Compact error.
Position decimals also controls full geometry position tags and Compact move
endpoints. Other pose channels keep the precision needed by the geometry solver.
Exact merges positions only when they serialize identically at the selected
precision. Changing any apply option clears the old Plan preview immediately.

Position fitting first tries a least-squares single `\move`. If that candidate
exceeds the maximum per-frame error, a bounded feasibility search tries other
endpoints before splitting the run. A feasible single segment takes priority
over a smaller least-squares loss that would still require multiple events.
Candidates must meet the same video-pixel error budget with coordinate-rounding
reserve, and the output still passes the final serialized ASS position check.
The search never crosses tracking-gap hold boundaries or relaxes the budget;
it does not guarantee a global minimax solution or find every feasible segment.

For Similarity, position fitting determines event boundaries. The initial pose
fit uses a 0.05-degree rotation tolerance and 0.05 ASS percentage-point scale
tolerance. If that fit needs multiple `\t` ramps, Compact also tries a single
linear or accelerated transform against the subtitle's measured geometry. Nearly
constant channels can remain static. This simplification is accepted only when
the **combined** position, rotation and scale error stays within Compact error
in video pixels at every visible tracked frame, using the serialized ASS values
and event clock. It does not independently give each channel the full budget.

Text simplification requires a font measurement provider; drawings can use their
intrinsic bounds. Mixed font/size runs, unavailable measurements, and layouts
which could wrap during the tracked zoom retain the initial strict pose fit.
That fallback keeps its separate position and pose tolerances; it is not a new
combined-pixel guarantee. If a single transform cannot meet the geometry budget,
multiple `\t` ramps stay in the same event rather than splitting the subtitle
for pose curvature alone. Nonlinear position paths and coverage boundaries can
still require multiple events; full geometry output retains its separate rules.

Compact, Exact and the other output options reuse the same Analyze data, including
after Apply. Switching an output option lets you preview or apply again without
retracking. Each Apply builds from the subtitle lines captured at Analyze and
replaces the previous output, so motion and event splitting do not accumulate.
Manual changes to the tracked subtitle lines, their styles, script settings or
timecodes require Analyze again; undoing an Apply also requires a fresh analysis.
An explicit Analyze after Apply starts from the currently selected subtitle lines.

## Debug data

After Analyze, **Export debug data...** saves a single versioned JSON file without
changing the subtitles. Export before or after Apply: it retains the original
Analyze-time selected lines, not the generated replacement events, and includes
the current apply settings and newly computed plan. Changing the model/direction
or editing the source requires a matching valid analysis, just as for Apply.

The file contains the selected subtitle text, referenced styles and font names,
rendering script settings, every tracking sample and transform, reference/domain
metadata, exact CFR/VFR frame mapping, and plan output or failure diagnostics.
Video/audio, project file paths, unrelated dialogue, attachments and unrelated
script metadata are not collected. This is **not anonymized subtitle content**:
review the JSON before sharing it. The save location is chosen explicitly.

For offline debugging, `ReadMotionTrackDebugBundle` reconstructs an owned ASS
source plus `ApplyPlanInput`; passing its `file`, `targets` and `input` to
`BuildApplyPlan` replays planning without decoding video. Its `exported_plan`
provides the original output for comparison. Geometry-aware Similarity Compact
and full-geometry replay need equivalent fonts and the same text-extents provider
when the capture used platform font measurement. Without a provider, Similarity
keeps the strict scalar fit and may emit more transforms. The package does not
embed fonts or measurements. Bounds residuals describe projected geometry, not a
pixel-by-pixel comparison of rendered alpha masks.

## Geometry and source effects

Affine and Perspective compose the tracked map with the subtitle's existing
geometry. They can write position, scale, shear and all three rotation tags. The
existing perspective solver evaluates PlayRes, LayoutRes, alignment and the source
geometry before choosing a representation; a result outside the error budget
rejects the complete apply plan.
Vertical font faces retain the renderer's semantic base rotation while the
remaining transform tags follow the tracked plane.

Static `\org`, rectangular `\clip`/`\iclip`, and vector clips follow the motion.
Rotated, sheared or perspective rectangles become vector paths. Drawing scales
are respected. Affine transformations preserve cubic curves; perspective curves
are subdivided with a bounded approximation error. Absolute clip paths and moving
origins cannot generally be animated by ASS, so Compact retains static segments
where needed. It may therefore produce as many events as Exact.

Full geometry apply currently requires static source geometry. Geometry animations,
animated clips, unsupported mixed text/drawing runs and ambiguous clip conversions
are rejected with a diagnostic, rather than partially rewriting a subtitle. The
font measurement and drawing restrictions of the perspective tool also apply.
Similarity also checks rotation and scale across visible text runs: different
per-run geometry is rejected instead of replacing it with one uniform transform.
Other supported local styling and resets with matching geometry retain their scope.
Disable smoothing, trajectory stabilization and border/shadow/blur scaling when
using full geometry apply; the established Translation/Similarity path retains
those options for subtitles without additional absolute or perspective geometry.

Standard source `\fad`/`\fade` and color/opacity `\t` animations keep their original
event timeline when output is split into new events. Implicit transform durations
are made explicit before the split; supported acceleration values are preserved.
Nested or unsupported animations and karaoke must be made static before applying
tracking. Unsupported forms reject the complete plan with a diagnostic.

The same coverage rule applies to every model: a failed gap bounded by good samples
holds the previous good pose; missing coverage at either end prevents Apply. All
selected lines are planned before mutation, and more than 100 generated events
requires the existing event-count confirmation.
