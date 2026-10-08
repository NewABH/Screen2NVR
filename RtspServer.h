#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct RtspClientStatistics
{
    uint32_t devices = 0, mainSessions = 0, subSessions = 0;
};

class RtspServer
{
public:
    using LogCallback = std::function<void(const std::string&)>;

    explicit RtspServer(LogCallback logger);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    void Start(uint16_t port, const std::string& path, bool authenticationEnabled = false,
               const std::string& userName = {}, const std::string& password = {},
               const std::string& allowedIpAddresses = {}, const std::string& subPath = {});
    void Stop();
    void PublishAccessUnit(const uint8_t* data, size_t size, int64_t sampleTime100ns, unsigned streamIndex = 0);
    RtspClientStatistics ClientStatistics() const;
    bool Probe(unsigned streamIndex = 0) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
