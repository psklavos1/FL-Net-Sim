#include "config.h"

#include "../common/json.hpp"

#include "ns3/core-module.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flsim::lte
{
using json = nlohmann::json;

static std::string
ReadTextFile(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::in);
    if (!in)
    {
        NS_FATAL_ERROR("Could not open JSON config file: " << path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

template <typename T>
static bool
GetIfPresent(const json& j, const std::string& key, T& out)
{
    if (!j.is_object())
    {
        return false;
    }
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
    {
        return false;
    }
    out = it->get<T>();
    return true;
}

static bool
HasObject(const json& j, const std::string& key)
{
    return j.is_object() && j.contains(key) && j.at(key).is_object();
}

static void
ApplyMobilitySpecFromJson(MobilitySpec& mobility, const json& j)
{
    GetIfPresent(j, "model", mobility.model);
    GetIfPresent(j, "speed_mps", mobility.speedMps);
    GetIfPresent(j, "min_x", mobility.minX);
    GetIfPresent(j, "max_x", mobility.maxX);
    GetIfPresent(j, "min_y", mobility.minY);
    GetIfPresent(j, "max_y", mobility.maxY);
}

static NodePosition
ParseNodePosition(const json& node, const std::string& fieldName)
{
    if (!node.is_object())
    {
        NS_FATAL_ERROR(fieldName << " must be an object");
    }
    NodePosition pos;
    if (!node.contains("x") || !node.contains("y"))
    {
        NS_FATAL_ERROR(fieldName << " must include x and y");
    }
    pos.x = node.at("x").get<double>();
    pos.y = node.at("y").get<double>();
    if (node.contains("z"))
    {
        pos.z = node.at("z").get<double>();
    }
    return pos;
}

ScenarioConfig
DefaultScenarioConfig()
{
    ScenarioConfig cfg;
    cfg.deviceTiers = {
        {"very_weak", DeviceTier{10.0}},
        {"weak", DeviceTier{13.0}},
        {"basic", DeviceTier{18.0}},
        {"strong", DeviceTier{21.0}},
        {"very_strong", DeviceTier{23.0}},
    };

    cfg.topology.cellQualityPresets = {
        {"default", CellQualityPreset{30.0, 50, 50}},
        {"modern", CellQualityPreset{40.0, 75, 75}},
    };

    cfg.mobilityPresets = {
        {"static", MobilityPreset{"constant_position", 0.0, 1.0}},
        {"slow", MobilityPreset{"random_walk_2d", 2.0, 1.5}},
        {"normal", MobilityPreset{"random_walk_2d", 5.0, 2.0}},
        {"fast", MobilityPreset{"random_walk_2d", 10.0, 2.5}},
    };

    cfg.topology.enbs = {
        EnbConfig{"enb-0", "default", NodePosition{-200.0, 0.0, 15.0}, MobilitySpec{}},
        EnbConfig{"enb-1", "default", NodePosition{200.0, 0.0, 15.0}, MobilitySpec{}},
    };

    cfg.topology.clients.resize(6);
    const std::array<uint32_t, 6> enbIdx = {0, 0, 0, 1, 1, 1};
    const std::array<const char*, 6> mobility = {
        "static", "slow", "normal", "static", "normal", "fast"};
    const std::array<double, 6> radius = {150.0, 180.0, 220.0, 120.0, 180.0, 260.0};
    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        cfg.topology.clients[i].enbIndex = enbIdx[i];
        cfg.topology.clients[i].mobilityPreset = mobility[i];
        cfg.topology.clients[i].radiusM = radius[i];
    }

    cfg.clients.numClients = 6;
    cfg.clients
        .deviceTierByClient = {"very_strong", "strong", "basic", "basic", "weak", "very_weak"};
    cfg.clients.mobilityPresetByClient = {"static", "slow", "normal", "static", "normal", "fast"};
    cfg.clients.selectedClients = {0, 1, 2, 3, 4, 5};
    std::vector<bool> selectedByClient(cfg.clients.numClients, false);
    for (auto idx : cfg.clients.selectedClients)
    {
        if (idx < selectedByClient.size())
        {
            selectedByClient[idx] = true;
        }
    }
    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        if (i < cfg.clients.deviceTierByClient.size())
        {
            cfg.topology.clients[i].deviceTier = cfg.clients.deviceTierByClient[i];
        }
        cfg.topology.clients[i].hasSelected = true;
        cfg.topology.clients[i].selected =
            (i < selectedByClient.size()) ? selectedByClient[i] : true;
    }
    return cfg;
}

static void
ApplyJsonConfig(ScenarioConfig& cfg, const json& root)
{
    if (!root.is_object())
    {
        NS_FATAL_ERROR("Top-level JSON must be an object.");
    }


    GetIfPresent(root, "description", cfg.description);
    if (HasObject(root, "reproducibility"))
    {
        const auto& r = root.at("reproducibility");
        GetIfPresent(r, "round", cfg.round);
        GetIfPresent(r, "seed", cfg.seed);
    }

    if (HasObject(root, "sim"))
    {
        const auto& sim = root.at("sim");
        GetIfPresent(sim, "simulation_time", cfg.sim.stopS);
        GetIfPresent(sim, "poll_ms", cfg.sim.pollMs);
    }

    if (HasObject(root, "presets"))
    {
        const auto& p = root.at("presets");
        if (p.contains("cell_quality_presets") && !HasObject(p, "cell_quality_presets"))
        {
            NS_FATAL_ERROR("presets.cell_quality_presets must be an object");
        }

        if (HasObject(p, "cell_quality_presets"))
        {
            const auto& presets = p.at("cell_quality_presets");
            cfg.topology.cellQualityPresets.clear();
            for (auto it = presets.begin(); it != presets.end(); ++it)
            {
                const std::string name = it.key();
                const json& obj = it.value();
                if (!obj.is_object())
                {
                    NS_FATAL_ERROR("presets.cell_quality_presets." << name << " must be an object");
                }
                CellQualityPreset preset = cfg.topology.cellQualityPresets.count(name)
                                               ? cfg.topology.cellQualityPresets[name]
                                               : CellQualityPreset{};
                GetIfPresent(obj, "tx_power_dbm", preset.txPowerDbm);
                GetIfPresent(obj, "dl_bandwidth", preset.dlBandwidth);
                GetIfPresent(obj, "ul_bandwidth", preset.ulBandwidth);
                cfg.topology.cellQualityPresets[name] = preset;
            }
        }

        if (p.contains("device_tiers") && !HasObject(p, "device_tiers"))
        {
            NS_FATAL_ERROR("presets.device_tiers must be an object");
        }
        if (HasObject(p, "device_tiers"))
        {
            const auto& tiers = p.at("device_tiers");
            cfg.deviceTiers.clear();
            for (auto it = tiers.begin(); it != tiers.end(); ++it)
            {
                const std::string tierName = it.key();
                const json& obj = it.value();
                if (!obj.is_object())
                {
                    NS_FATAL_ERROR("presets.device_tiers." << tierName << " must be an object");
                }
                DeviceTier tier =
                    cfg.deviceTiers.count(tierName) ? cfg.deviceTiers[tierName] : DeviceTier{};
                GetIfPresent(obj, "tx_power_dbm", tier.txPowerDbm);
                cfg.deviceTiers[tierName] = tier;
            }
        }

        if (p.contains("mobility_presets") && !HasObject(p, "mobility_presets"))
        {
            NS_FATAL_ERROR("presets.mobility_presets must be an object");
        }
        if (HasObject(p, "mobility_presets"))
        {
            const auto& presets = p.at("mobility_presets");
            cfg.mobilityPresets.clear();
            for (auto it = presets.begin(); it != presets.end(); ++it)
            {
                const std::string presetName = it.key();
                const json& obj = it.value();
                if (!obj.is_object())
                {
                    NS_FATAL_ERROR("presets.mobility_presets." << presetName
                                                               << " must be an object");
                }
                MobilityPreset preset = cfg.mobilityPresets.count(presetName)
                                            ? cfg.mobilityPresets[presetName]
                                            : MobilityPreset{};
                GetIfPresent(obj, "model", preset.model);
                GetIfPresent(obj, "speed_mps", preset.speedMps);
                GetIfPresent(obj, "area_scale", preset.areaScale);
                cfg.mobilityPresets[presetName] = preset;
            }
        }
    }

    if (HasObject(root, "network"))
    {
        const auto& topo = root.at("network");

        if (topo.contains("server_side") && !HasObject(topo, "server_side"))
        {
            NS_FATAL_ERROR("network.server_side must be an object");
        }
        if (HasObject(topo, "server_side"))
        {
            const auto& serverSide = topo.at("server_side");
            if (HasObject(serverSide, "server_ap") &&
                serverSide.at("server_ap").contains("position"))
            {
                cfg.topology.serverSide.serverApPosition =
                    ParseNodePosition(serverSide.at("server_ap").at("position"),
                                      "network.server_side.server_ap.position");
            }
            if (HasObject(serverSide, "server") &&
                serverSide.at("server").contains("position"))
            {
                cfg.topology.serverSide.serverPosition =
                    ParseNodePosition(serverSide.at("server").at("position"),
                                      "network.server_side.server.position");
            }
            if (HasObject(serverSide, "entry_link"))
            {
                const auto& link = serverSide.at("entry_link");
                GetIfPresent(link, "up_mbps", cfg.topology.serverSide.entryLink.upMbps);
                GetIfPresent(link, "down_mbps", cfg.topology.serverSide.entryLink.downMbps);
                GetIfPresent(link,
                             "oneway_delay_ms",
                             cfg.topology.serverSide.entryLink.oneWayDelayMs);
                GetIfPresent(link, "loss", cfg.topology.serverSide.entryLink.lossRate);
            }
            if (HasObject(serverSide, "server_link"))
            {
                const auto& link = serverSide.at("server_link");
                GetIfPresent(link, "up_mbps", cfg.topology.serverSide.serverLink.upMbps);
                GetIfPresent(link, "down_mbps", cfg.topology.serverSide.serverLink.downMbps);
                GetIfPresent(link,
                             "oneway_delay_ms",
                             cfg.topology.serverSide.serverLink.oneWayDelayMs);
                GetIfPresent(link, "loss", cfg.topology.serverSide.serverLink.lossRate);
            }
        }

        if (topo.contains("channel") && !HasObject(topo, "channel"))
        {
            NS_FATAL_ERROR("network.channel must be an object");
        }
        if (HasObject(topo, "channel"))
        {
            const auto& ch = topo.at("channel");
            GetIfPresent(ch, "pathloss_model", cfg.topology.channel.pathlossModel);
            GetIfPresent(ch, "log_distance_exponent", cfg.topology.channel.logDistanceExponent);
            GetIfPresent(ch,
                         "log_distance_reference_distance_m",
                         cfg.topology.channel.logDistanceReferenceDistanceM);
            GetIfPresent(ch,
                         "log_distance_reference_loss_db",
                         cfg.topology.channel.logDistanceReferenceLossDb);
        }

        if (topo.contains("enbs"))
        {
            const auto& enbs = topo.at("enbs");
            if (!enbs.is_array())
            {
                NS_FATAL_ERROR("network.enbs must be an array");
            }
            cfg.topology.enbs.clear();
            for (const auto& entry : enbs)
            {
                if (!entry.is_object())
                {
                    NS_FATAL_ERROR("network.enbs entries must be objects");
                }
                EnbConfig enb;
                GetIfPresent(entry, "name", enb.name);
                if (!entry.contains("cell_quality"))
                {
                    NS_FATAL_ERROR("network.enbs entries must define cell_quality");
                }
                GetIfPresent(entry, "cell_quality", enb.quality);
                if (entry.contains("position"))
                {
                    enb.position = ParseNodePosition(entry.at("position"),
                                                     "network.enbs.position");
                }
                if (HasObject(entry, "mobility"))
                {
                    ApplyMobilitySpecFromJson(enb.mobility, entry.at("mobility"));
                }
                cfg.topology.enbs.push_back(enb);
            }
        }

        if (topo.contains("clients"))
        {
            const auto& clients = topo.at("clients");
            if (!clients.is_array())
            {
                NS_FATAL_ERROR("network.clients must be an array");
            }
            cfg.topology.clients.clear();
            for (const auto& entry : clients)
            {
                if (!entry.is_object())
                {
                    NS_FATAL_ERROR("network.clients entries must be objects");
                }
                ClientNodeConfig c;
                GetIfPresent(entry, "enb", c.enbIndex);
                GetIfPresent(entry, "device_tier", c.deviceTier);
                GetIfPresent(entry, "mobility_preset", c.mobilityPreset);
                if (entry.contains("selected"))
                {
                    c.hasSelected = true;
                    GetIfPresent(entry, "selected", c.selected);
                }
                GetIfPresent(entry, "radius_m", c.radiusM);
                if (entry.contains("position"))
                {
                    c.hasExplicitPosition = true;
                    c.position = ParseNodePosition(entry.at("position"),
                                                   "network.clients.position");
                }
                if (HasObject(entry, "mobility"))
                {
                    c.hasMobilityOverride = true;
                    ApplyMobilitySpecFromJson(c.mobility, entry.at("mobility"));
                }
                cfg.topology.clients.push_back(c);
            }
        }
    }

    if (!cfg.topology.clients.empty())
    {
        const size_t numClients = cfg.topology.clients.size();
        cfg.clients.numClients = static_cast<uint32_t>(numClients);

        cfg.clients.deviceTierByClient.resize(numClients, "basic");
        cfg.clients.mobilityPresetByClient.resize(numClients, "static");

        bool hasClientSelected = false;
        for (size_t i = 0; i < numClients; ++i)
        {
            auto& c = cfg.topology.clients[i];
            if (!c.deviceTier.empty())
            {
                cfg.clients.deviceTierByClient[i] = c.deviceTier;
            }
            else
            {
                c.deviceTier = cfg.clients.deviceTierByClient[i];
            }

            if (!c.mobilityPreset.empty())
            {
                cfg.clients.mobilityPresetByClient[i] = c.mobilityPreset;
            }
            else
            {
                c.mobilityPreset = cfg.clients.mobilityPresetByClient[i];
            }

            if (c.hasSelected)
            {
                hasClientSelected = true;
            }
        }

        if (hasClientSelected)
        {
            cfg.clients.selectedClients.clear();
            for (size_t i = 0; i < numClients; ++i)
            {
                if (cfg.topology.clients[i].selected)
                {
                    cfg.clients.selectedClients.push_back(static_cast<uint32_t>(i));
                }
            }
        }
        else
        {
            cfg.clients.selectedClients.clear();
            for (size_t i = 0; i < numClients; ++i)
            {
                cfg.clients.selectedClients.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    if (HasObject(root, "fl_traffic"))
    {
        const auto& f = root.at("fl_traffic");
        GetIfPresent(f, "transport", cfg.fl.transport);
        GetIfPresent(f, "model_size_mb", cfg.fl.modelSizeMb);
        GetIfPresent(f, "sync_start_jitter_ms", cfg.fl.syncStartJitterMs);
        GetIfPresent(f, "compute_s", cfg.fl.computeS);

        if (HasObject(f, "tcp"))
        {
            const auto& tcp = f.at("tcp");
            GetIfPresent(tcp, "socket_type", cfg.fl.tcp.socketType);
            GetIfPresent(tcp, "sack", cfg.fl.tcp.sack);
            GetIfPresent(tcp, "snd_buf_bytes", cfg.fl.tcp.sndBufBytes);
            GetIfPresent(tcp, "rcv_buf_bytes", cfg.fl.tcp.rcvBufBytes);
            GetIfPresent(tcp, "segment_size_bytes", cfg.fl.tcp.segmentSizeBytes);
            GetIfPresent(tcp, "app_send_size_bytes", cfg.fl.tcp.appSendSizeBytes);
        }
    }

    if (HasObject(root, "metrics"))
    {
        const auto& m = root.at("metrics");
        GetIfPresent(m, "flow_monitor", cfg.metrics.flowMonitor);
        GetIfPresent(m, "event_log", cfg.metrics.eventLog);
        GetIfPresent(m, "netanim", cfg.metrics.netAnim);
    }
}

static void
ApplyCliOverrides(ScenarioConfig& cfg, const CliOverrides& o)
{
    if (o.round != std::numeric_limits<uint32_t>::max())
    {
        cfg.round = o.round;
    }
    if (o.seed != std::numeric_limits<uint64_t>::max())
    {
        cfg.seed = o.seed;
    }

    if (o.numClients != std::numeric_limits<uint32_t>::max())
    {
        cfg.clients.numClients = o.numClients;
        cfg.topology.clients.resize(o.numClients);
        cfg.clients.deviceTierByClient.resize(o.numClients, "basic");
        cfg.clients.mobilityPresetByClient.resize(o.numClients, "static");
        for (uint32_t i = 0; i < o.numClients; ++i)
        {
            cfg.topology.clients[i].deviceTier = cfg.clients.deviceTierByClient[i];
            cfg.topology.clients[i].mobilityPreset = cfg.clients.mobilityPresetByClient[i];
        }
    }

    if (o.numEnbs != std::numeric_limits<uint32_t>::max())
    {
        cfg.topology.enbs.resize(o.numEnbs);
    }

    if (!std::isnan(o.stopS))
    {
        cfg.sim.stopS = o.stopS;
    }

    if (!std::isnan(o.modelSizeMb))
    {
        cfg.fl.modelSizeMb = o.modelSizeMb;
    }
}

static void
ValidateConfig(ScenarioConfig& cfg)
{
    if (cfg.seed == 0)
    {
        NS_FATAL_ERROR("seed must be > 0");
    }
    if (cfg.round == 0)
    {
        NS_FATAL_ERROR("round must be > 0");
    }

    if (cfg.topology.enbs.empty())
    {
        NS_FATAL_ERROR("network.enbs must be non-empty");
    }

    if (cfg.topology.clients.empty())
    {
        if (cfg.clients.numClients == 0)
        {
            NS_FATAL_ERROR("network.clients must be non-empty");
        }
        cfg.topology.clients.resize(cfg.clients.numClients);
    }
    cfg.clients.numClients = static_cast<uint32_t>(cfg.topology.clients.size());

    if (cfg.clients.deviceTierByClient.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("device_tier list size ("
                       << cfg.clients.deviceTierByClient.size()
                       << ") must match network.clients size (" << cfg.clients.numClients << ")");
    }
    if (cfg.clients.mobilityPresetByClient.size() != cfg.clients.numClients)
    {
        NS_FATAL_ERROR("mobility_preset list size ("
                       << cfg.clients.mobilityPresetByClient.size()
                       << ") must match network.clients size (" << cfg.clients.numClients << ")");
    }

    if (cfg.clients.selectedClients.empty())
    {
        NS_FATAL_ERROR("at least one client must be selected");
    }

    std::set<uint32_t> uniq(cfg.clients.selectedClients.begin(), cfg.clients.selectedClients.end());
    if (uniq.size() != cfg.clients.selectedClients.size())
    {
        NS_FATAL_ERROR("selected clients contains duplicates");
    }

    for (uint32_t idx : cfg.clients.selectedClients)
    {
        if (idx >= cfg.clients.numClients)
        {
            NS_FATAL_ERROR("selected client index " << idx
                                                    << " out of range for network.clients size="
                                                    << cfg.clients.numClients);
        }
    }

    for (uint32_t i = 0; i < cfg.topology.enbs.size(); ++i)
    {
        const auto& enb = cfg.topology.enbs[i];
        if (enb.name.empty())
        {
            NS_FATAL_ERROR("enbs[" << i << "].name must be non-empty");
        }
        if (enb.quality.empty())
        {
            NS_FATAL_ERROR("enbs[" << i << "].cell_quality must be non-empty");
        }
    }

    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        const auto& c = cfg.topology.clients[i];
        if (c.enbIndex >= cfg.topology.enbs.size())
        {
            NS_FATAL_ERROR("network.clients[" << i << "].enb out of range");
        }
        if (c.radiusM <= 0.0)
        {
            NS_FATAL_ERROR("network.clients[" << i << "].radius_m must be > 0");
        }
        if (!c.mobilityPreset.empty() && !cfg.mobilityPresets.count(c.mobilityPreset))
        {
            NS_FATAL_ERROR("network.clients[" << i << "].mobility_preset='" << c.mobilityPreset
                                               << "' not found in mobility_presets");
        }
    }

    for (const auto& [name, tier] : cfg.deviceTiers)
    {
        if (tier.txPowerDbm <= 0.0)
        {
            NS_FATAL_ERROR("device_tier '" << name << "' tx_power_dbm must be > 0");
        }
    }

    if (cfg.topology.cellQualityPresets.empty())
    {
        NS_FATAL_ERROR("presets.cell_quality_presets must define at least one preset");
    }

    for (const auto& [name, preset] : cfg.topology.cellQualityPresets)
    {
        if (preset.txPowerDbm <= 0.0)
        {
            NS_FATAL_ERROR("cell_quality_presets." << name << ".tx_power_dbm must be > 0");
        }
        if (preset.dlBandwidth == 0 || preset.ulBandwidth == 0)
        {
            NS_FATAL_ERROR("cell_quality_presets." << name << " bandwidth must be > 0");
        }
    }

    for (const auto& enb : cfg.topology.enbs)
    {
        if (!cfg.topology.cellQualityPresets.count(enb.quality))
        {
            NS_FATAL_ERROR("enb.cell_quality '" << enb.quality
                                                << "' missing from cell_quality_presets");
        }
    }

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const auto& mobilityPreset = cfg.clients.mobilityPresetByClient[i];
        if (!cfg.mobilityPresets.count(mobilityPreset))
        {
            NS_FATAL_ERROR("Mobility preset '" << mobilityPreset << "' for client " << i
                                               << " is missing from mobility_presets");
        }
    }

    for (const auto& [name, preset] : cfg.mobilityPresets)
    {
        if (preset.model != "constant_position" && preset.model != "random_walk_2d")
        {
            NS_FATAL_ERROR("mobility_preset '"
                           << name << "' model must be constant_position or random_walk_2d");
        }
        if (preset.speedMps < 0.0)
        {
            NS_FATAL_ERROR("mobility_preset '" << name << "' speed_mps must be >= 0");
        }
        if (preset.areaScale <= 0.0)
        {
            NS_FATAL_ERROR("mobility_preset '" << name << "' area_scale must be > 0");
        }
    }

    if (cfg.sim.stopS <= 0)
    {
        NS_FATAL_ERROR("sim.simulation_time must be > 0");
    }

    if (cfg.sim.pollMs <= 0)
    {
        NS_FATAL_ERROR("sim.poll_ms must be > 0");
    }

    if (cfg.fl.modelSizeMb <= 0)
    {
        NS_FATAL_ERROR("fl_traffic.model_size_mb must be > 0");
    }

    if (cfg.fl.transport != "tcp" && cfg.fl.transport != "udp")
    {
        NS_FATAL_ERROR("fl_traffic.transport must be 'tcp' or 'udp'");
    }

    if (cfg.fl.transport == "tcp" && cfg.fl.tcp.appSendSizeBytes > cfg.fl.tcp.segmentSizeBytes)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be <= segment_size_bytes");
    }
    if (cfg.fl.tcp.appSendSizeBytes == 0)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be > 0");
    }

    if (cfg.topology.serverSide.entryLink.upMbps <= 0 ||
        cfg.topology.serverSide.entryLink.downMbps <= 0)
    {
        NS_FATAL_ERROR("network.server_side.entry_link up/down must be > 0");
    }
    if (cfg.topology.serverSide.entryLink.oneWayDelayMs < 0)
    {
        NS_FATAL_ERROR("network.server_side.entry_link.oneway_delay_ms must be >= 0");
    }
    if (cfg.topology.serverSide.entryLink.lossRate < 0 ||
        cfg.topology.serverSide.entryLink.lossRate > 1)
    {
        NS_FATAL_ERROR("network.server_side.entry_link.loss must be in [0,1]");
    }

    if (cfg.topology.serverSide.serverLink.upMbps <= 0 ||
        cfg.topology.serverSide.serverLink.downMbps <= 0)
    {
        NS_FATAL_ERROR("network.server_side.server_link up/down must be > 0");
    }
    if (cfg.topology.serverSide.serverLink.oneWayDelayMs < 0)
    {
        NS_FATAL_ERROR("network.server_side.server_link.oneway_delay_ms must be >= 0");
    }
    if (cfg.topology.serverSide.serverLink.lossRate < 0 ||
        cfg.topology.serverSide.serverLink.lossRate > 1)
    {
        NS_FATAL_ERROR("network.server_side.server_link.loss must be in [0,1]");
    }

    if (cfg.topology.channel.logDistanceExponent <= 0.0)
    {
        NS_FATAL_ERROR("network.channel.log_distance_exponent must be > 0");
    }
    if (cfg.topology.channel.logDistanceReferenceDistanceM <= 0.0)
    {
        NS_FATAL_ERROR("network.channel.log_distance_reference_distance_m must be > 0");
    }
}

void
AddCommandLineArgs(ns3::CommandLine& cmd, CliOverrides& o)
{
    cmd.AddValue("config", "Path to JSON config file", o.configPath);
    cmd.AddValue("round", "Round id (also used as RNG run id)", o.round);
    cmd.AddValue("seed", "Experiment-level RNG seed", o.seed);
    cmd.AddValue("numClients", "Override number of clients", o.numClients);
    cmd.AddValue("numEnbs", "Override number of eNBs", o.numEnbs);
    cmd.AddValue("simTime", "Override sim.simulation_time", o.stopS);
    cmd.AddValue("stopS", "Override sim.simulation_time", o.stopS);
    cmd.AddValue("modelSizeMb", "Override fl_traffic.model_size_mb", o.modelSizeMb);
}

ScenarioConfig
ResolveConfig(const CliOverrides& in)
{
    ScenarioConfig cfg = DefaultScenarioConfig();

    if (!in.configPath.empty())
    {
        const std::string text = ReadTextFile(in.configPath);
        json root;
        try
        {
            root = json::parse(text);
        }
        catch (const std::exception& e)
        {
            NS_FATAL_ERROR("JSON parse error: " << e.what());
        }
        ApplyJsonConfig(cfg, root);
    }

    ApplyCliOverrides(cfg, in);
    ValidateConfig(cfg);
    return cfg;
}

void
PrintResolvedConfig(const ScenarioConfig& c)
{
    std::cout << "\n==== Resolved LTE Configuration ====\n";
    std::cout << "description: " << c.description << "\n";
    std::cout << "round: " << c.round << "\n";
    std::cout << "seed: " << c.seed << "\n";
    std::cout << "scheduler: ns3::PfFfMacScheduler\n";

    std::cout << "\n[network]\n";
    std::cout << "num_enbs: " << c.topology.enbs.size() << "\n";
    std::cout << "num_clients: " << c.clients.numClients << "\n";
    std::cout << "server_side.entry_link: up=" << c.topology.serverSide.entryLink.upMbps
              << " down=" << c.topology.serverSide.entryLink.downMbps
              << " delay_ms=" << c.topology.serverSide.entryLink.oneWayDelayMs
              << " loss=" << c.topology.serverSide.entryLink.lossRate << "\n";
    std::cout << "server_side.server_link: up=" << c.topology.serverSide.serverLink.upMbps
              << " down=" << c.topology.serverSide.serverLink.downMbps
              << " delay_ms=" << c.topology.serverSide.serverLink.oneWayDelayMs
              << " loss=" << c.topology.serverSide.serverLink.lossRate << "\n";
    std::cout << "server_side.server_ap.position: ("
              << c.topology.serverSide.serverApPosition.x << ", "
              << c.topology.serverSide.serverApPosition.y << ", "
              << c.topology.serverSide.serverApPosition.z << ")\n";
    std::cout << "server_side.server.position: ("
              << c.topology.serverSide.serverPosition.x << ", "
              << c.topology.serverSide.serverPosition.y << ", "
              << c.topology.serverSide.serverPosition.z << ")\n";
    std::cout << "cell_quality_presets:\n";
    for (const auto& kv : c.topology.cellQualityPresets)
    {
        const auto& q = kv.second;
        std::cout << "  " << kv.first << ": {tx_power_dbm=" << q.txPowerDbm
                  << ", dl_bandwidth=" << q.dlBandwidth << ", ul_bandwidth=" << q.ulBandwidth
                  << "}\n";
    }
    std::cout << "cell_quality_by_enb:";
    for (const auto& enb : c.topology.enbs)
    {
        std::cout << " " << enb.quality;
    }
    std::cout << "\n";
    std::cout << "channel.pathloss_model: " << c.topology.channel.pathlossModel << "\n";
    std::cout << "channel.log_distance_exponent: " << c.topology.channel.logDistanceExponent
              << "\n";
    std::cout << "channel.log_distance_reference_distance_m: "
              << c.topology.channel.logDistanceReferenceDistanceM << "\n";
    std::cout << "channel.log_distance_reference_loss_db: "
              << c.topology.channel.logDistanceReferenceLossDb << "\n";

    std::cout << "\n[network.clients]\n";
    std::cout << "selected:";
    for (auto idx : c.clients.selectedClients)
    {
        std::cout << " " << idx;
    }
    std::cout << "\n";
    std::cout << "device_tier:";
    for (const auto& tierName : c.clients.deviceTierByClient)
    {
        std::cout << " " << tierName;
    }
    std::cout << "\n";
    std::cout << "mobility_preset:";
    for (const auto& presetName : c.clients.mobilityPresetByClient)
    {
        std::cout << " " << presetName;
    }
    std::cout << "\n";

    std::cout << "\n[device_tiers]\n";
    for (const auto& kv : c.deviceTiers)
    {
        const auto& p = kv.second;
        std::cout << kv.first << ": {tx_power_dbm=" << p.txPowerDbm << "}\n";
    }

    std::cout << "\n[mobility_presets]\n";
    for (const auto& kv : c.mobilityPresets)
    {
        const auto& p = kv.second;
        std::cout << kv.first << ": {model=" << p.model << ", speed_mps=" << p.speedMps
                  << ", area_scale=" << p.areaScale << "}\n";
    }

    std::cout << "\n[fl_traffic]\n";
    std::cout << "model_mb: " << c.fl.modelSizeMb << "\n";
    std::cout << "sync_start_jitter_ms: " << c.fl.syncStartJitterMs << "\n";
    std::cout << "compute_s: " << c.fl.computeS << "\n";

    std::cout << "\n[metrics]\n";
    std::cout << "flow_monitor: " << (c.metrics.flowMonitor ? "true" : "false") << "\n";
    std::cout << "event_log: " << (c.metrics.eventLog ? "true" : "false") << "\n";
    std::cout << "netanim: " << (c.metrics.netAnim ? "true" : "false") << "\n";

    std::cout << "=====================================\n\n";
}

} // namespace flsim::lte
