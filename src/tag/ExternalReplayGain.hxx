#pragma once

#include "ReplayGainInfo.hxx"

class InputStream;
struct ConfigData;

void replay_gain_external_init(const ConfigData &config);

bool
replay_gain_external_read(InputStream &is, ReplayGainInfo &info) noexcept;