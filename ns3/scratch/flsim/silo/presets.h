#pragma once
#include "config.h"
#include <string>
#include <vector>

namespace flsim
{

// Build per-client tier list from network.clients[].preset.
std::vector<std::string> ExpandTierAssignment(const ScenarioConfig& cfg);

// Convenience getter
const TierPreset& GetTierPreset(const ScenarioConfig& cfg, const std::string& tier);

} // namespace flsim
