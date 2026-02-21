
#include "presets.h"
#include "ns3/core-module.h"

namespace flsim
{

const TierPreset&
GetTierPreset(const ScenarioConfig& cfg, const std::string& tier)
{
  auto it = cfg.presets.find(tier);
  if (it == cfg.presets.end())
    {
      NS_FATAL_ERROR("Preset tier not found: " << tier);
    }
  return it->second;
}

std::vector<std::string>
ExpandTierAssignment(const ScenarioConfig& cfg)
{
  if (cfg.clients.presetByClient.size() != cfg.clients.numClients)
    {
      NS_FATAL_ERROR("clients.presetByClient size mismatch");
    }

  for (const auto& tier : cfg.clients.presetByClient)
    {
      GetTierPreset(cfg, tier);
    }

  return cfg.clients.presetByClient;
}

} // namespace flsim
