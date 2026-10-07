#pragma once

class AssFile;
namespace agi::vfr {
class Framerate;
}

void TransformAssFileFramerateForExport(AssFile& subs,
										agi::vfr::Framerate const& source,
										agi::vfr::Framerate const& destination);
