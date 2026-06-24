#pragma once

#include "ReplayGainInfo.hxx"

#include <string_view>

struct ConfigData;

void replay_gain_external_init(const ConfigData &config);

bool
replay_gain_external_read(std::string_view uri, ReplayGainInfo &info) noexcept;