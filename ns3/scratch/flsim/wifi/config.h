#pragma once

#include "ns3/core-module.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace flsim::wifi
{

// -------- Preset parameters --------
struct DeviceTier
{
    double txPowerDbm = 18.0;
};

struct MobilityPreset
{
    std::string model = "constant_position"; // constant_position | random_walk_2d
    double speedMps = 0.0;
    double areaScale = 1.5; // multiplier for wifi radius to derive movement bounds
};

struct MobilitySpec
{
    std::string model = "constant_position"; // constant_position | random_walk_2d
    double speedMps = 0.0;
    double minX = -50.0;
    double maxX = 50.0;
    double minY = -50.0;
    double maxY = 50.0;
};

struct NodePosition
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ApQualityPreset
{
    std::string wifiStandard = "80211ac"; // 80211n | 80211ac | 80211ax
    double txPowerDbm = 20.0;
};

// -------- Topology --------
struct ApConfig
{
    std::string ssid = "flsim-ap";
    std::string quality = "default";
    NodePosition position;
    MobilitySpec mobility;
};

struct ClientNodeConfig
{
    uint32_t apIndex = 0;
    std::string deviceTier = "";
    std::string mobilityPreset = "";
    bool hasSelected = false;
    bool selected = true;
    double radiusM = 10.0;
    bool hasExplicitPosition = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool hasMobilityOverride = false;
    MobilitySpec mobility;
};

struct ServerSideLinkConfig
{
    double upMbps = 1000.0;
    double downMbps = 1000.0;
    double oneWayDelayMs = 2.0;
    double lossRate = 0.0;
};

struct ServerSideConfig
{
    ServerSideLinkConfig accessLink; // AP <-> server AP (ISP router)
    ServerSideLinkConfig serverLink{10000.0, 10000.0, 1.0, 0.0};
    bool hasServerApPosition = false;
    NodePosition serverApPosition;
    bool hasServerPosition = false;
    NodePosition serverPosition;
};

struct ChannelConfig
{
    double logDistanceExponent = 3.0;
    double logDistanceReferenceDistanceM = 1.0;
    double logDistanceReferenceLossDb = 46.6777; // ~5 GHz free-space loss @ 1 m
};

struct TopologyConfig
{
    std::map<std::string, ApQualityPreset> apQualityPresets;
    std::vector<ApConfig> aps;
    std::vector<ClientNodeConfig> clients;
    ServerSideConfig serverSide;
    ChannelConfig channel;
};

// -------- Client and transport config --------
struct ClientsConfig
{
    uint32_t numClients = 6;
    std::vector<std::string> deviceTierByClient;
    std::vector<std::string> mobilityPresetByClient;
    std::vector<uint32_t> selectedClients;
};

struct TcpConfig
{
    std::string socketType = "ns3::TcpCubic";
    bool sack = true;
    uint32_t sndBufBytes = 4 * 1024 * 1024;
    uint32_t rcvBufBytes = 4 * 1024 * 1024;
    uint32_t segmentSizeBytes = 1448;
    uint32_t appSendSizeBytes = 1448;
};

struct FlTrafficConfig
{
    std::string transport = "tcp";
    double modelSizeMb = 50.0;
    double syncStartJitterMs = 200.0;
    double computeS = 5.0;
    TcpConfig tcp;
};

struct MetricsConfig
{
    bool flowMonitor = true;
    bool eventLog = true;
    bool netAnim = false;
};

// -------- Simulation and scenario --------
struct SimConfig
{
    double stopS = 240.0;
    double pollMs = 10.0;
};

struct ScenarioConfig
{
    std::string description = "cross_silo_home_wifi";
    uint32_t round = 1;
    uint64_t seed = 1;
    std::string wifiManager = "ns3::MinstrelHtWifiManager";

    SimConfig sim;
    TopologyConfig topology;
    ClientsConfig clients;
    std::map<std::string, DeviceTier> deviceTiers;
    std::map<std::string, MobilityPreset> mobilityPresets;
    FlTrafficConfig fl;
    MetricsConfig metrics;
};

// -------- CLI override container --------
// Sentinel values mean "not set".
struct CliOverrides
{
    std::string configPath;

    uint32_t round = std::numeric_limits<uint32_t>::max();
    uint64_t seed = std::numeric_limits<uint64_t>::max();
    uint32_t numClients = std::numeric_limits<uint32_t>::max();
    uint32_t numAps = std::numeric_limits<uint32_t>::max();
    double modelSizeMb = std::numeric_limits<double>::quiet_NaN();
    double stopS = std::numeric_limits<double>::quiet_NaN();
};

// -------- Public API --------
ScenarioConfig DefaultScenarioConfig();
void AddCommandLineArgs(ns3::CommandLine& cmd, CliOverrides& o);
ScenarioConfig ResolveConfig(const CliOverrides& o);
void PrintResolvedConfig(const ScenarioConfig& cfg);

} // namespace flsim::wifi
