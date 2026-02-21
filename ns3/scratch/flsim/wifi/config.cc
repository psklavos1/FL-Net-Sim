#include "config.h"

#include "../common/json.hpp"

#include "ns3/core-module.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace flsim::wifi
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

static bool
IsSupportedWifiStandard(const std::string& standard)
{
    return standard == "80211n" || standard == "80211ac" || standard == "80211ax";
}

ScenarioConfig
DefaultScenarioConfig()
{
    ScenarioConfig cfg;
    cfg.deviceTiers = {
        {"very_weak", DeviceTier{10.0}},
        {"weak", DeviceTier{13.0}},
        {"basic", DeviceTier{17.0}},
        {"strong", DeviceTier{20.0}},
        {"very_strong", DeviceTier{23.0}},
    };

    cfg.topology.apQualityPresets = {
        {"default", ApQualityPreset{"80211ac", 20.0}},
        {"modern", ApQualityPreset{"80211ax", 23.0}},
    };

    cfg.mobilityPresets = {
        {"static", MobilityPreset{"constant_position", 0.0, 1.0}},
        {"slow", MobilityPreset{"random_walk_2d", 0.4, 1.5}},
        {"normal", MobilityPreset{"random_walk_2d", 1.0, 2.0}},
        {"fast", MobilityPreset{"random_walk_2d", 2.0, 2.5}},
    };

    cfg.topology.aps = {
        ApConfig{"home-ap-0", "default", NodePosition{-25.0, 0.0, 1.5}, MobilitySpec{}},
        ApConfig{"home-ap-1", "default", NodePosition{25.0, 0.0, 1.5}, MobilitySpec{}},
    };

    cfg.topology.clients.resize(6);
    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        cfg.topology.clients[i].apIndex = (i < 3) ? 0 : 1;
        cfg.topology.clients[i].mobilityPreset = "static";
        cfg.topology.clients[i].radiusM = 12.0;
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
    if (root.contains("round") || root.contains("seed"))
    {
        NS_FATAL_ERROR("round/seed must be set under reproducibility.{round,seed}");
    }
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

    if (HasObject(root, "network"))
    {
        const auto& topo = root.at("network");
        GetIfPresent(topo, "wifi_manager", cfg.wifiManager);

        if (HasObject(topo, "server_side"))
        {
            const auto& s = topo.at("server_side");
            if (HasObject(s, "access_link"))
            {
                const auto& a = s.at("access_link");
                GetIfPresent(a, "up_mbps", cfg.topology.serverSide.accessLink.upMbps);
                GetIfPresent(a, "down_mbps", cfg.topology.serverSide.accessLink.downMbps);
                GetIfPresent(a,
                             "oneway_delay_ms",
                             cfg.topology.serverSide.accessLink.oneWayDelayMs);
                GetIfPresent(a, "loss", cfg.topology.serverSide.accessLink.lossRate);
            }
            if (HasObject(s, "server_link"))
            {
                const auto& c = s.at("server_link");
                GetIfPresent(c, "up_mbps", cfg.topology.serverSide.serverLink.upMbps);
                GetIfPresent(c, "down_mbps", cfg.topology.serverSide.serverLink.downMbps);
                GetIfPresent(c,
                             "oneway_delay_ms",
                             cfg.topology.serverSide.serverLink.oneWayDelayMs);
                GetIfPresent(c, "loss", cfg.topology.serverSide.serverLink.lossRate);
            }
        }


        if (HasObject(topo, "server_side"))
        {
            const auto& s = topo.at("server_side");
            if (HasObject(s, "server_ap"))
            {
                const auto& ap = s.at("server_ap");
                if (ap.contains("position"))
                {
                    cfg.topology.serverSide.serverApPosition =
                        ParseNodePosition(ap.at("position"),
                                          "network.server_side.server_ap.position");
                    cfg.topology.serverSide.hasServerApPosition = true;
                }
            }
            if (HasObject(s, "server"))
            {
                const auto& srv = s.at("server");
                if (srv.contains("position"))
                {
                    cfg.topology.serverSide.serverPosition =
                        ParseNodePosition(srv.at("position"),
                                          "network.server_side.server.position");
                    cfg.topology.serverSide.hasServerPosition = true;
                }
            }
        }

        if (HasObject(topo, "channel"))
        {
            const auto& ch = topo.at("channel");
            GetIfPresent(ch, "log_distance_exponent", cfg.topology.channel.logDistanceExponent);
            GetIfPresent(ch,
                         "log_distance_reference_distance_m",
                         cfg.topology.channel.logDistanceReferenceDistanceM);
            GetIfPresent(ch,
                         "log_distance_reference_loss_db",
                         cfg.topology.channel.logDistanceReferenceLossDb);
        }

        if (topo.contains("access_points"))
        {
            const auto& aps = topo.at("access_points");
            if (!aps.is_array())
            {
                NS_FATAL_ERROR("network.access_points must be an array");
            }
            cfg.topology.aps.clear();
            for (const auto& entry : aps)
            {
                if (!entry.is_object())
                {
                    NS_FATAL_ERROR("network.access_points entries must be objects");
                }
                ApConfig ap;
                GetIfPresent(entry, "ssid", ap.ssid);
                if (!entry.contains("ap_quality"))
                {
                    NS_FATAL_ERROR("network.access_points entries must define ap_quality");
                }
                GetIfPresent(entry, "ap_quality", ap.quality);
                if (!HasObject(entry, "position"))
                {
                    NS_FATAL_ERROR("network.access_points entries must define position");
                }
                ap.position =
                    ParseNodePosition(entry.at("position"), "network.access_points.position");
                if (HasObject(entry, "mobility"))
                {
                    ApplyMobilitySpecFromJson(ap.mobility, entry.at("mobility"));
                }
                cfg.topology.aps.push_back(ap);
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
                GetIfPresent(entry, "ap", c.apIndex);
                GetIfPresent(entry, "device_tier", c.deviceTier);
                GetIfPresent(entry, "mobility_preset", c.mobilityPreset);
                if (entry.contains("selected"))
                {
                    c.hasSelected = true;
                    GetIfPresent(entry, "selected", c.selected);
                }
                GetIfPresent(entry, "radius_m", c.radiusM);
                if (HasObject(entry, "position"))
                {
                    const auto& pos = entry.at("position");
                    c.hasExplicitPosition = true;
                    GetIfPresent(pos, "x", c.x);
                    GetIfPresent(pos, "y", c.y);
                    GetIfPresent(pos, "z", c.z);
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

    if (root.contains("presets"))
    {
        const auto& p = root.at("presets");
        if (!p.is_object())
        {
            NS_FATAL_ERROR("presets must be an object");
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
                if (obj.contains("wifi_standard"))
                {
                    NS_FATAL_ERROR("presets.device_tiers."
                                   << tierName
                                   << ".wifi_standard is not allowed. Standards are "
                                      "defined per AP quality presets.");
                }
                if (obj.contains("radius_m"))
                {
                    NS_FATAL_ERROR("presets.device_tiers."
                                   << tierName
                                   << ".radius_m is not allowed. Set radius_m in "
                                      "network.clients");
                }
                if (obj.contains("manager") || obj.contains("data_mode") ||
                    obj.contains("control_mode"))
                {
                    NS_FATAL_ERROR("presets.device_tiers."
                                   << tierName
                                   << " cannot set manager/data/control modes. Use "
                                      "wifi_manager.");
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

        if (p.contains("ap_quality_presets") && !HasObject(p, "ap_quality_presets"))
        {
            NS_FATAL_ERROR("presets.ap_quality_presets must be an object");
        }
        if (HasObject(p, "ap_quality_presets"))
        {
            const auto& presets = p.at("ap_quality_presets");
            cfg.topology.apQualityPresets.clear();
            for (auto it = presets.begin(); it != presets.end(); ++it)
            {
                const std::string name = it.key();
                const json& obj = it.value();
                if (!obj.is_object())
                {
                    NS_FATAL_ERROR("presets.ap_quality_presets." << name << " must be an object");
                }
                ApQualityPreset preset = cfg.topology.apQualityPresets.count(name)
                                             ? cfg.topology.apQualityPresets[name]
                                             : ApQualityPreset{};
                GetIfPresent(obj, "wifi_standard", preset.wifiStandard);
                GetIfPresent(obj, "tx_power_dbm", preset.txPowerDbm);
                cfg.topology.apQualityPresets[name] = preset;
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

    if (o.numAps != std::numeric_limits<uint32_t>::max())
    {
        cfg.topology.aps.resize(o.numAps);
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

    if (cfg.topology.aps.empty())
    {
        NS_FATAL_ERROR("network.access_points must be non-empty");
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

    for (uint32_t i = 0; i < cfg.clients.numClients; ++i)
    {
        const auto& tierName = cfg.clients.deviceTierByClient[i];
        if (!cfg.deviceTiers.count(tierName))
        {
            NS_FATAL_ERROR("Device tier '" << tierName << "' for client " << i
                                           << " is missing from device_tiers");
        }
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

    for (uint32_t i = 0; i < cfg.topology.aps.size(); ++i)
    {
        const auto& ap = cfg.topology.aps[i];
        if (ap.ssid.empty())
        {
            NS_FATAL_ERROR("access_points[" << i << "].ssid must be non-empty");
        }
        if (ap.quality.empty())
        {
            NS_FATAL_ERROR("access_points[" << i << "].ap_quality must be non-empty");
        }
    }

    for (uint32_t i = 0; i < cfg.topology.clients.size(); ++i)
    {
        const auto& c = cfg.topology.clients[i];
        if (c.apIndex >= cfg.topology.aps.size())
        {
            NS_FATAL_ERROR("network.clients[" << i << "].ap out of range");
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
    if (cfg.topology.apQualityPresets.empty())
    {
        NS_FATAL_ERROR("presets.ap_quality_presets must define at least one preset");
    }
    for (const auto& [name, preset] : cfg.topology.apQualityPresets)
    {
        if (!IsSupportedWifiStandard(preset.wifiStandard))
        {
            NS_FATAL_ERROR("ap_quality_presets."
                           << name << ".wifi_standard must be 80211n/80211ac/80211ax");
        }
        if (preset.txPowerDbm <= 0.0)
        {
            NS_FATAL_ERROR("ap_quality_presets." << name << ".tx_power_dbm must be > 0");
        }
    }
    for (const auto& ap : cfg.topology.aps)
    {
        if (!cfg.topology.apQualityPresets.count(ap.quality))
        {
            NS_FATAL_ERROR("access_points.ap_quality '"
                           << ap.quality << "' missing from presets.ap_quality_presets");
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

    if (cfg.fl.transport != "tcp")
    {
        NS_FATAL_ERROR("Only fl_traffic.transport='tcp' is currently supported");
    }

    if (cfg.fl.computeS < 0)
    {
        NS_FATAL_ERROR("fl_traffic.compute_s must be >= 0");
    }

    if (cfg.fl.tcp.sndBufBytes < 64 * 1024 || cfg.fl.tcp.rcvBufBytes < 64 * 1024)
    {
        NS_FATAL_ERROR("fl_traffic.tcp snd/rcv buffers must be >= 65536 bytes");
    }

    if (cfg.fl.tcp.segmentSizeBytes < 536 || cfg.fl.tcp.segmentSizeBytes > 8960)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.segment_size_bytes must be in [536, 8960]");
    }

    if (cfg.fl.tcp.appSendSizeBytes == 0)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be > 0");
    }

    if (cfg.fl.tcp.appSendSizeBytes > cfg.fl.tcp.segmentSizeBytes)
    {
        NS_FATAL_ERROR("fl_traffic.tcp.app_send_size_bytes must be <= segment_size_bytes");
    }

    if (cfg.topology.serverSide.accessLink.upMbps <= 0 ||
        cfg.topology.serverSide.accessLink.downMbps <= 0)
    {
        NS_FATAL_ERROR("network.server_side.access_link up/down must be > 0");
    }

    if (cfg.topology.serverSide.accessLink.oneWayDelayMs < 0)
    {
        NS_FATAL_ERROR("network.server_side.access_link.oneway_delay_ms must be >= 0");
    }

    if (cfg.topology.serverSide.accessLink.lossRate < 0 ||
        cfg.topology.serverSide.accessLink.lossRate > 1)
    {
        NS_FATAL_ERROR("network.server_side.access_link.loss must be in [0,1]");
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
    if (cfg.wifiManager.empty())
    {
        NS_FATAL_ERROR("wifi_manager must be non-empty");
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
    cmd.AddValue("numAps", "Override number of APs", o.numAps);
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
    std::cout << "\n==== Resolved WiFi Configuration ====\n";
    std::cout << "description: " << c.description << "\n";
    std::cout << "round: " << c.round << "\n";
    std::cout << "seed: " << c.seed << "\n";
    std::cout << "wifi_manager: " << c.wifiManager << "\n";

    std::cout << "\n[network]\n";
    std::cout << "num_aps: " << c.topology.aps.size() << "\n";
    std::cout << "num_clients: " << c.clients.numClients << "\n";
    std::cout << "server_side.access_link: up=" << c.topology.serverSide.accessLink.upMbps
              << " down=" << c.topology.serverSide.accessLink.downMbps
              << " delay_ms=" << c.topology.serverSide.accessLink.oneWayDelayMs
              << " loss=" << c.topology.serverSide.accessLink.lossRate << "\n";
    std::cout << "server_side.server_link: up=" << c.topology.serverSide.serverLink.upMbps
              << " down=" << c.topology.serverSide.serverLink.downMbps
              << " delay_ms=" << c.topology.serverSide.serverLink.oneWayDelayMs
              << " loss=" << c.topology.serverSide.serverLink.lossRate << "\n";
    std::cout << "ap_quality_presets:\n";
    for (const auto& kv : c.topology.apQualityPresets)
    {
        const auto& q = kv.second;
        std::cout << "  " << kv.first << ": {wifi_standard=" << q.wifiStandard
                  << ", tx_power_dbm=" << q.txPowerDbm << "}\n";
    }
    std::cout << "ap_quality_by_ap:";
    for (const auto& ap : c.topology.aps)
    {
        std::cout << " " << ap.quality;
    }
    std::cout << "\n";
    std::cout << "channel.log_distance_exponent: " << c.topology.channel.logDistanceExponent
              << "\n";
    std::cout << "channel.log_distance_reference_distance_m: "
              << c.topology.channel.logDistanceReferenceDistanceM << "\n";
    std::cout << "channel.log_distance_reference_loss_db: "
              << c.topology.channel.logDistanceReferenceLossDb << "\n";
    if (c.topology.serverSide.hasServerApPosition || c.topology.serverSide.hasServerPosition)
    {
        std::cout << "server_side.server_ap.position: "
                  << (c.topology.serverSide.hasServerApPosition ? "set" : "auto") << "\n";
        std::cout << "server_side.server.position: "
                  << (c.topology.serverSide.hasServerPosition ? "set" : "auto") << "\n";
    }

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

} // namespace flsim::wifi
