#pragma once

#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"

#include <string>
#include <vector>

#include "config.h"

namespace flsim
{

struct BuiltNetwork
{
  // Nodes
  ns3::Ptr<ns3::Node> server;
  ns3::Ptr<ns3::Node> serverAp; // server-side access point/router
  ns3::NodeContainer silos;

  // Convenience containers
  ns3::NodeContainer all;

  // Per-silo IP address on silo side (useful for traffic config later)
  std::vector<ns3::Ipv4Address> siloIps;

  // Server-facing IP (single server link)
  ns3::Ipv4Address serverIp;

};

// Builds a star topology for cross-silo FL.
//
//   silo_i <--> serverAp <--> server
//
// tiers.size() must equal cfg.topology.numSilos and tiers[i] must exist in cfg.presets.
BuiltNetwork BuildStarSiloNetwork(const ScenarioConfig& cfg,
                                  const std::vector<std::string>& tiers);

} // namespace flsim
