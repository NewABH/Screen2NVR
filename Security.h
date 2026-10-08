#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <map>
#include <mutex>

namespace Security
{
std::string Base64(std::string_view bytes);
std::string Md5Hex(const std::string& value);
std::string Header(const std::string& request, const std::string& name, bool* present = nullptr);
bool Basic(const std::string& authorization, const std::string& user, const std::string& password);
bool OnvifToken(const std::string& xml, const std::string& user, const std::string& password,
                bool* present = nullptr, std::string* reason = nullptr);
bool IpAllowed(const std::string& address, const std::string& list);
bool NormalizeIpList(const std::wstring& value, std::wstring& normalized);

enum class DigestChallengeMode { QopAuth, LegacyMd5 };

// A challenge belongs to the server and peer IP, not a particular TCP connection.
// HTTP clients reconnect after 401; RTSP clients may do the same.
class DigestAuthenticator
{
public:
    std::string Challenge(const std::string& peer, bool stale = false,
                          DigestChallengeMode mode = DigestChallengeMode::QopAuth);
    bool Validate(const std::string& authorization, const std::string& method, const std::string& uri,
                  const std::string& user, const std::string& password, const std::string& peer,
                  std::string& reason);
private:
    struct Nonce
    {
        uint64_t issued = 0;
        std::string peer;
        std::map<std::string, uint32_t> counts;
    };
    std::mutex mutex_;
    std::map<std::string, Nonce> nonces_;
};

class AuthenticationLogLimiter
{
public:
    explicit AuthenticationLogLimiter(uint64_t intervalMs = 10'000, unsigned burst = 1)
        : intervalMs_(intervalMs), burst_(burst) {}
    bool ShouldLog();
private:
    std::mutex mutex_;
    uint64_t last_ = 0;
    uint64_t intervalMs_;
    unsigned burst_, used_ = 0;
};
}
