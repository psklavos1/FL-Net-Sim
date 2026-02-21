#pragma once
#include "ns3/core-module.h"

#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace flsim
{

// -------- Preset tier parameters (network) --------
struct TierPreset
{
    double upMbps = 50.0;
    double downMbps = 200.0;
    double oneWayDelayMs = 25.0;
    double lossRate = 0.0; // 0..1
};

// -------- Server-side network --------
struct ServerSideLinkConfig
{
    double upMbps = 1000.0;
    double downMbps = 1000.0;
    double oneWayDelayMs = 2.0;
    double lossRate = 0.0; // 0..1
};

struct NodePosition
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ServerSideConfig
{
    ServerSideLinkConfig serverLink{10000.0, 10000.0, 1.0, 0.0};
    std::string queueDiscType = "fq_codel"; // future use
    bool hasServerApPosition = false;
    NodePosition serverApPosition;
    bool hasServerPosition = false;
    NodePosition serverPosition;
};

struct SiloClientConfig
{
    bool hasPosition = false;
    NodePosition position;
    std::string preset = "";
    bool hasSelected = false;
    bool selected = true;
};

// -------- Topology --------
struct TopologyConfig
{
    uint32_t numSilos = 5;
    ServerSideConfig serverSide;
    std::vector<SiloClientConfig> clients; // optional positions
};

// -------- Clients --------
struct ClientsConfig
{
    uint32_t numClients = 5;
    std::vector<std::string> presetByClient;
    std::vector<uint32_t> selectedClients;
};

// -------- TCP transport --------
struct TcpConfig
{
    std::string socketType = "ns3::TcpCubic";
    bool sack = true;
    uint32_t sndBufBytes = 4 * 1024 * 1024;
    uint32_t rcvBufBytes = 4 * 1024 * 1024;
    uint32_t segmentSizeBytes = 1448;
    uint32_t appSendSizeBytes = 1448;
};

// -------- Traffic/FL --------
struct FlTrafficConfig
{
    std::string transport = "tcp";
    double modelSizeMb = 200.0;
    double syncStartJitterMs = 200.0;
    double computeS = 20.0;
    TcpConfig tcp;
};

// -------- Metrics flags --------
struct MetricsConfig
{
    bool flowMonitor = true;
    bool queueTraces = true;
    bool eventLog = true;
    bool netAnim = false;
};

// -------- Simulation --------
struct SimConfig
{
    double stopS = 300.0;
    double pollMs = 10.0;
};

// -------- Whole scenario config --------
struct ScenarioConfig
{
    std::string description = "cross_silo_star";
    uint32_t round = 1;
    uint64_t seed = 1;

    SimConfig sim;
    TopologyConfig topology;
    ClientsConfig clients;

    // presets by name
    std::map<std::string, TierPreset> presets;

    FlTrafficConfig fl;
    MetricsConfig metrics;
};

// -------- CLI override container --------
// Sentinel values mean "not set"
struct CliOverrides
{
    std::string configPath;

    uint32_t numSilos = std::numeric_limits<uint32_t>::max();
    double modelSizeMb = std::numeric_limits<double>::quiet_NaN();
    double stopS = std::numeric_limits<double>::quiet_NaN();
    uint32_t round = std::numeric_limits<uint32_t>::max();
    uint64_t seed = std::numeric_limits<uint64_t>::max();
};

// -------- Public API --------
ScenarioConfig DefaultScenarioConfig(); // provides strong/basic/weak defaults

void AddCommandLineArgs(ns3::CommandLine& cmd, CliOverrides& o);

// The main entry: defaults -> JSON -> CLI, then validate; returns final config
ScenarioConfig ResolveConfig(const CliOverrides& o);

// Pretty print (for testing)
void PrintResolvedConfig(const ScenarioConfig& cfg);

} // namespace flsim
