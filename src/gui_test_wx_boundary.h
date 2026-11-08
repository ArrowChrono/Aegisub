#pragma once

#include <libaegisub/fs_fwd.h>
#include <libaegisub/cajun/elements.h>
#include <functional>
#include <string>

struct AppLaunchPlan;
class FrameMain;
class wxString;
namespace agi {
struct Context;
}
using WxBoundaryPublisher = std::function<void(std::string const&, json::Object)>;
void EnableWxBoundaryTestAssertions(AppLaunchPlan const& launch_plan);
void InstallWxBoundaryTestHost(FrameMain& frame, agi::Context& context, agi::fs::path const& artifacts, WxBoundaryPublisher const& publish);
bool RecordWxBoundaryTestAssertion(wxString const& file, int line, wxString const& function, wxString const& condition, wxString const& message);
