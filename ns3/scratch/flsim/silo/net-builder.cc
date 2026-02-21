#include "net-builder.h"
#include "ns3/assert.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/mobility-module.h"
#include "../common/utils.h"
#include "ns3/point-to-point-module.h"

#include <cmath>

namespace flsim
{
using namespace ns3;

static void
SetPointToPointDeviceRate(Ptr<NetDevice> dev, double mbps)
{
  Ptr<PointToPointNetDevice> p2p = dev->GetObject<PointToPointNetDevice>();
  if (!p2p)
    {
      NS_FATAL_ERROR("NetDevice is not a PointToPointNetDevice");
    }

  // PointToPointNetDevice expects a DataRateValue
  std::ostringstream oss;
  oss << mbps << "Mbps";
  p2p->SetAttribute("DataRate", DataRateValue(DataRate(oss.str())));
}

static void
AttachReceiveLossModelIfNeeded(Ptr<NetDevice> rxDev, double lossRate)
{
  if (lossRate <= 0.0)
    return;

  if (lossRate < 0.0 || lossRate > 1.0)
    {
      NS_FATAL_ERROR("lossRate must be in [0,1], got " << lossRate);
    }

  Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
  // Interpret configured loss as packet loss probability.
  em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
  em->SetAttribute("ErrorRate", DoubleValue(lossRate));
  rxDev->SetAttribute("ReceiveErrorModel", PointerValue(em));
}

static Ipv4InterfaceContainer
AssignSubnet(Ipv4AddressHelper& ipv4,
             const NetDeviceContainer& devs,
             std::vector<Ipv4Address>* outSiloIpMaybe,
             uint32_t siloIndex,
             bool recordSiloSideAsFirst)
{
  // caller controls order of devs; we assume devs.Get(0) is the "silo-side" if recordSiloSideAsFirst
  Ipv4InterfaceContainer ifc = ipv4.Assign(devs);

  if (outSiloIpMaybe)
    {
      Ipv4Address a = recordSiloSideAsFirst ? ifc.GetAddress(0) : ifc.GetAddress(1);
      if (siloIndex >= outSiloIpMaybe->size())
        {
          NS_FATAL_ERROR("Internal error: silo index out of range");
        }
      (*outSiloIpMaybe)[siloIndex] = a;
    }

  ipv4.NewNetwork();
  return ifc;
}

static std::vector<Vector>
ComputeDefaultSiloPositions(uint32_t numSilos, double spacing)
{
  std::vector<Vector> out;
  if (numSilos == 0)
    return out;

  const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(numSilos)));
  out.reserve(numSilos);
  for (uint32_t i = 0; i < numSilos; ++i)
    {
      uint32_t row = i / cols;
      uint32_t col = i % cols;
      out.emplace_back(col * spacing, row * spacing, 0.0);
    }
  return out;
}

BuiltNetwork
BuildStarSiloNetwork(const ScenarioConfig& cfg, const std::vector<std::string>& tiers)
{
  if (tiers.size() != cfg.topology.numSilos)
    {
      NS_FATAL_ERROR("tiers.size() (" << tiers.size() << ") != numSilos (" << cfg.topology.numSilos
                                      << ")");
    }

  BuiltNetwork out;

  // ---- Create nodes ----
  out.silos.Create(cfg.topology.numSilos);
  out.server = CreateObject<Node>();
  out.serverAp = CreateObject<Node>();

  NodeContainer core;
  core.Add(out.server);
  core.Add(out.serverAp);

  out.all.Add(out.silos);
  out.all.Add(core);

  // ---- Positions (for NetAnim only) ----
  const double spacing = 20.0;
  std::vector<Vector> siloPositions = ComputeDefaultSiloPositions(cfg.topology.numSilos, spacing);
  if (!cfg.topology.clients.empty())
    {
      for (uint32_t i = 0; i < cfg.topology.clients.size() && i < siloPositions.size(); ++i)
        {
          const auto& c = cfg.topology.clients[i];
          if (c.hasPosition)
            {
              siloPositions[i] = Vector(c.position.x, c.position.y, c.position.z);
            }
        }
    }

  for (uint32_t i = 0; i < out.silos.GetN(); ++i)
    {
      flsim::common::SetConstantPosition(out.silos.Get(i), siloPositions[i]);
    }

  // Place server-side AP and server.
  const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(cfg.topology.numSilos)));
  const uint32_t rows = static_cast<uint32_t>(std::ceil(cfg.topology.numSilos / static_cast<double>(cols)));
  const double centerX = (cols > 0) ? (static_cast<double>(cols - 1) * spacing / 2.0) : 0.0;
  const double baseY = rows * spacing;

  Vector serverApPos(centerX, baseY + spacing, 0.0);
  if (cfg.topology.serverSide.hasServerApPosition)
    {
      serverApPos = Vector(cfg.topology.serverSide.serverApPosition.x,
                           cfg.topology.serverSide.serverApPosition.y,
                           cfg.topology.serverSide.serverApPosition.z);
    }
  flsim::common::SetConstantPosition(out.serverAp, serverApPos);

  Vector serverPos(centerX, baseY + 2.0 * spacing, 0.0);
  if (cfg.topology.serverSide.hasServerPosition)
    {
      serverPos = Vector(cfg.topology.serverSide.serverPosition.x,
                         cfg.topology.serverSide.serverPosition.y,
                         cfg.topology.serverSide.serverPosition.z);
    }
  flsim::common::SetConstantPosition(out.server, serverPos);

  // ---- Install Internet stack ----
  InternetStackHelper internet;
  internet.Install(out.all);

  // ---- Addressing base ----
  // Each P2P link gets a /30; NewNetwork() increments base automatically.
  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.0.0.0", "255.255.255.252");

  // ---- Build links ----
  PointToPointHelper p2p;
  // We'll set channel delay per-link because tiers differ.
  // We'll set device DataRate per-device after Install() to allow asymmetric up/down.

  out.siloIps.assign(cfg.topology.numSilos, Ipv4Address("0.0.0.0"));

  // Silos <-> server-side AP (independent bottlenecks)
  for (uint32_t i = 0; i < out.silos.GetN(); ++i)
    {
      const std::string& tier = tiers[i];
      auto it = cfg.presets.find(tier);
      if (it == cfg.presets.end())
        {
          NS_FATAL_ERROR("Tier '" << tier << "' not found in cfg.presets");
        }
      const TierPreset& tp = it->second;

      std::ostringstream delay;
      delay << tp.oneWayDelayMs << "ms";
      p2p.SetChannelAttribute("Delay", StringValue(delay.str()));

      NodeContainer pair(out.silos.Get(i), out.serverAp);
      NetDeviceContainer devs = p2p.Install(pair);

      // Asymmetric TX rates:
      // - silo TX = upMbps (upload bottleneck)
      // - serverAp TX = downMbps (download bottleneck)
      SetPointToPointDeviceRate(devs.Get(0), tp.upMbps);    // silo side
      SetPointToPointDeviceRate(devs.Get(1), tp.downMbps);  // serverAp side

      AttachReceiveLossModelIfNeeded(devs.Get(0), tp.lossRate);
      AttachReceiveLossModelIfNeeded(devs.Get(1), tp.lossRate);

      AssignSubnet(ipv4, devs, &out.siloIps, i, true);
    }

  // Server-side AP <-> server (server link)
  {
    const auto& s = cfg.topology.serverSide.serverLink;

    std::ostringstream delay;
    delay << s.oneWayDelayMs << "ms";
    p2p.SetChannelAttribute("Delay", StringValue(delay.str()));

    NodeContainer pair(out.serverAp, out.server);
    NetDeviceContainer devs = p2p.Install(pair);

    SetPointToPointDeviceRate(devs.Get(0), s.upMbps);    // serverAp TX (clients -> server)
    SetPointToPointDeviceRate(devs.Get(1), s.downMbps);  // server TX (server -> clients)

    AttachReceiveLossModelIfNeeded(devs.Get(0), s.lossRate);
    AttachReceiveLossModelIfNeeded(devs.Get(1), s.lossRate);

    Ipv4InterfaceContainer ifc = AssignSubnet(ipv4, devs, nullptr, 0, true);
    out.serverIp = ifc.GetAddress(1);
  }

  // ---- Routing ----
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  return out;
}

} // namespace flsim
