#pragma once

#include "config.h"

#include <string>
#include <vector>

namespace flsim::wifi
{

// Helpers for device tier lookup and per-client expansion.
const DeviceTier& GetDeviceTier(const ScenarioConfig& cfg, const std::string& tierName);
std::vector<std::string> ExpandDeviceTierAssignment(const ScenarioConfig& cfg);

} // namespace flsim::wifi
