#include "presets.h"

#include "ns3/core-module.h"

namespace flsim::wifi
{

const DeviceTier&
GetDeviceTier(const ScenarioConfig& cfg, const std::string& tierName)
{
    auto it = cfg.deviceTiers.find(tierName);
    if (it == cfg.deviceTiers.end())
    {
        NS_FATAL_ERROR("Device tier not found: " << tierName);
    }
    return it->second;
}

std::vector<std::string>
ExpandDeviceTierAssignment(const ScenarioConfig& cfg)
{
    if (cfg.clients.deviceTierByClient.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("clients.deviceTierByClient size mismatch");
    }

    for (const auto& tierName : cfg.clients.deviceTierByClient)
    {
        GetDeviceTier(cfg, tierName);
    }

    return cfg.clients.deviceTierByClient;
}

} // namespace flsim::wifi
