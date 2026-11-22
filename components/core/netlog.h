// Sends finished QSOs to a logging program over the network.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
bool netSendUdp(const std::string& host, int port, const uint8_t* data, size_t len);
bool netReachable(const std::string& host, int port, int timeoutMs = 3000);   // internet check (dead man's switch)
bool netSendTcp(const std::string& host, int port, const uint8_t* data, size_t len, int timeoutMs = 3000);
