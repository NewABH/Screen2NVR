#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "RtspServer.h"
#include "Security.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <iomanip>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace
{
constexpr size_t kMaximumRtpPayload = 1200;
constexpr uint8_t kPayloadType = 96;

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char valueChar) { return static_cast<char>(std::tolower(valueChar)); });
    return value;
}

std::string Trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

const char* DiagnosticMethod(const std::string& method)
{
    for (const char* name : { "OPTIONS", "DESCRIBE", "SETUP", "PLAY", "GET_PARAMETER", "TEARDOWN" })
        if (method == name) return name;
    return "OTHER"; // Never write arbitrary client strings or credential-bearing URIs to the log.
}

bool SendAll(SOCKET socket, const uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        const int sent = send(socket, reinterpret_cast<const char*>(data + offset),
                              static_cast<int>(size - offset), 0);
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

std::string Base64(const std::vector<uint8_t>& input)
{
    return Security::Base64(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
}

std::string RandomSessionId()
{
    std::random_device random;
    std::mt19937_64 generator(random());
    std::ostringstream value;
    value << std::hex << generator();
    return value.str();
}

struct NalUnit
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

std::vector<NalUnit> SplitNalUnits(const uint8_t* data, size_t size)
{
    std::vector<NalUnit> units;
    const auto prefixLength = [data, size](size_t offset) -> size_t {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
            return 3;
        if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
            data[offset + 2] == 0 && data[offset + 3] == 1)
            return 4;
        return 0;
    };

    size_t firstStart = size;
    for (size_t offset = 0; offset + 3 <= size; ++offset)
    {
        if (prefixLength(offset) != 0)
        {
            firstStart = offset;
            break;
        }
    }

    if (firstStart != size)
    {
        size_t offset = firstStart;
        while (offset < size)
        {
            const size_t prefix = prefixLength(offset);
            if (prefix == 0)
            {
                ++offset;
                continue;
            }
            const size_t nalStart = offset + prefix;
            size_t nalEnd = nalStart;
            while (nalEnd < size && prefixLength(nalEnd) == 0) ++nalEnd;
            if (nalEnd > nalStart) units.push_back({ data + nalStart, nalEnd - nalStart });
            offset = nalEnd;
        }
        return units;
    }

    size_t offset = 0;
    while (offset + 4 <= size)
    {
        const uint32_t nalSize = (static_cast<uint32_t>(data[offset]) << 24) |
                                 (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                 (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                 static_cast<uint32_t>(data[offset + 3]);
        offset += 4;
        if (nalSize == 0 || nalSize > size - offset) break;
        units.push_back({ data + offset, nalSize });
        offset += nalSize;
    }
    if (units.empty() && size > 0) units.push_back({ data, size });
    return units;
}

std::vector<NalUnit> OrderAccessUnit(std::vector<NalUnit> units, bool containsIdr,
                                    const std::vector<uint8_t>& cachedSps,
                                    const std::vector<uint8_t>& cachedPps)
{
    if (!containsIdr)
    {
        const auto aud = std::find_if(units.begin(), units.end(), [](const NalUnit& unit) {
            return unit.size && (unit.data[0] & 0x1F) == 9;
        });
        if (aud != units.end()) std::rotate(units.begin(), aud, aud + 1);
        return units; // Reuse the split vector without another allocation on every P frame.
    }
    std::vector<NalUnit> ordered;
    ordered.reserve(units.size() + 2);
    // H.264 7.4.1.2.3: an AUD, when present, must be the first NAL in the access unit.
    // Do not prepend cached parameter sets ahead of the encoder's delimiter.
    for (const auto& unit : units)
        if (unit.size && (unit.data[0] & 0x1F) == 9) ordered.push_back(unit);
    if (containsIdr)
    {
        // Retain every original parameter set (including distinct parameter-set IDs).
        // Supply a cached set only when the encoder omitted that kind on this IDR.
        for (const uint8_t type : { uint8_t(7), uint8_t(8) })
        {
            bool found = false;
            for (const auto& unit : units)
                if (unit.size && (unit.data[0] & 0x1F) == type)
                { ordered.push_back(unit); found = true; }
            const auto& cached = type == 7 ? cachedSps : cachedPps;
            if (!found && !cached.empty()) ordered.push_back({ cached.data(), cached.size() });
        }
    }
    for (const auto& unit : units)
    {
        if (!unit.size) continue;
        const uint8_t type = unit.data[0] & 0x1F;
        if (type == 9 || (containsIdr && (type == 7 || type == 8))) continue;
        ordered.push_back(unit);
    }
    return ordered;
}
}

struct RtspServer::Impl
{
    enum class Transport
    {
        none,
        tcp,
        udp,
    };

    struct Client
    {
        SOCKET rtspSocket = INVALID_SOCKET;
        SOCKET rtpSocket = INVALID_SOCKET;
        SOCKET rtcpSocket = INVALID_SOCKET;
        sockaddr_in rtpDestination{};
        Transport transport = Transport::none;
        uint8_t interleavedChannel = 0;
        std::atomic_bool playing{ false };
        std::atomic_bool internalProbe{ false };
        unsigned streamIndex = 0;
        bool setup = false;
        std::atomic_bool alive{ true };
        std::atomic_bool threadFinished{ false };
        std::mutex sendMutex;
        std::string receiveBuffer;
        std::string sessionId = RandomSessionId();
        std::string peerAddress;
        uint64_t diagnosticId = 0;
        const char* diagnosticMethod = "NONE";
        std::string diagnosticAuth, diagnosticReason;
        std::set<std::string> loggedResponses; // Only touched by this client's control thread.
        std::atomic<uint64_t> sentPackets{ 0 }, sentFrames{ 0 };
        std::atomic<int> sendError{ 0 };
    };

    explicit Impl(LogCallback callback) : logger(std::move(callback))
    {
        std::random_device random;
        std::mt19937 generator(random());
        for (auto& stream : streams)
        {
            stream.sequence = static_cast<uint16_t>(generator());
            stream.ssrc = generator();
        }
    }

    ~Impl() { Stop(); }

    void Start(uint16_t requestedPort, const std::string& requestedPath, bool requireAuthentication,
               const std::string& requestedUserName, const std::string& requestedPassword,
               const std::string& requestedAllowedIps, const std::string& subPath)
    {
        if (running) return;
        port = requestedPort;
        path = requestedPath;
        streams[0].path = path;
        streams[1].path = subPath;
        if (path.empty() || (!subPath.empty() && path == subPath))
            throw std::runtime_error("RTSP main/sub stream paths must be nonempty and distinct");
        authenticationEnabled = requireAuthentication;
        userName = requestedUserName;
        password = requestedPassword;
        allowedIpAddresses = requestedAllowedIps;
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) ThrowSocket("RTSP socket");

        BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            ThrowSocket("RTSP bind");
        if (listen(listener, SOMAXCONN) == SOCKET_ERROR) ThrowSocket("RTSP listen");

        running = true;
        acceptThread = std::thread([this]() { AcceptLoop(); });
        logger("Embedded RTSP server listening on TCP " + std::to_string(port) + ", path /" + path);
    }

    void Stop()
    {
        if (!running.exchange(false)) return;
        if (listener != INVALID_SOCKET)
        {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (const auto& client : clients)
            {
                client->alive = false;
                shutdown(client->rtspSocket, SD_BOTH);
            }
        }
        if (acceptThread.joinable()) acceptThread.join();
        for (auto& entry : clientThreads)
            if (entry.second.joinable()) entry.second.join();
        clientThreads.clear();
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.clear();
    }

    void Publish(const uint8_t* data, size_t size, int64_t sampleTime100ns, unsigned streamIndex)
    {
        if (streamIndex >= streams.size() || streams[streamIndex].path.empty()) return;
        auto& channel = streams[streamIndex];
        auto& sps = channel.sps; auto& pps = channel.pps;
        std::vector<NalUnit> units = SplitNalUnits(data, size);
        if (units.empty()) return;

        bool containsIdr = false;
        bool containsPicture = false;
        {
            std::lock_guard<std::mutex> lock(codecMutex);
            for (const NalUnit& unit : units)
            {
                if (unit.size == 0) continue;
                const uint8_t type = unit.data[0] & 0x1F;
                if (type >= 1 && type <= 5) containsPicture = true;
                if (type == 7) sps.assign(unit.data, unit.data + unit.size);
                else if (type == 8) pps.assign(unit.data, unit.data + unit.size);
                else if (type == 5) containsIdr = true;
            }
        }

        // Media types can deliver SPS/PPS before the first picture. Cache these for SDP
        // and the next IDR, but do not transmit an empty video access unit.
        if (!containsPicture) return;

        const uint32_t timestamp = static_cast<uint32_t>((sampleTime100ns * 90'000LL) / 10'000'000LL);
        channel.lastTimestamp.store(timestamp);

        std::vector<uint8_t> currentSps;
        std::vector<uint8_t> currentPps;
        if (containsIdr)
        {
            std::lock_guard<std::mutex> lock(codecMutex);
            currentSps = sps;
            currentPps = pps;
        }
        const auto ordered = OrderAccessUnit(std::move(units), containsIdr, currentSps, currentPps);
        for (size_t index = 0; index < ordered.size(); ++index)
            SendNal(ordered[index].data, ordered[index].size, timestamp,
                    index + 1 == ordered.size(), streamIndex);
    }

    void AcceptLoop()
    {
        while (running)
        {
            sockaddr_in remote{};
            int remoteLength = sizeof(remote);
            const SOCKET socketValue = accept(listener, reinterpret_cast<sockaddr*>(&remote), &remoteLength);
            if (socketValue == INVALID_SOCKET)
            {
                if (!running) break;
                continue;
            }
            char remoteText[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &remote.sin_addr, remoteText, sizeof(remoteText));
            if (!IsIpAllowed(remoteText))
            {
                if (connectionLog.ShouldLog()) logger("RTSP connection rejected by IP allowlist: " + std::string(remoteText));
                closesocket(socketValue);
                continue;
            }
            for (auto iterator = clientThreads.begin(); iterator != clientThreads.end();)
            {
                if (iterator->first->threadFinished.load())
                {
                    if (iterator->second.joinable()) iterator->second.join();
                    iterator = clientThreads.erase(iterator);
                }
                else ++iterator;
            }
            DWORD timeout = 2000;
            setsockopt(socketValue, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            DWORD receiveTimeout = 65'000;
            setsockopt(socketValue, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                if (clients.size() >= (std::string(remoteText) == "127.0.0.1" ? 68U : 64U))
                { closesocket(socketValue); continue; }
            }
            auto client = std::make_shared<Client>();
            client->rtspSocket = socketValue;
            client->peerAddress = remoteText;
            client->diagnosticId = nextDiagnosticId++;
            {
                std::lock_guard<std::mutex> lock(clientsMutex);
                clients.push_back(client);
            }
            clientThreads.emplace_back(client, std::thread([this, client]() { ClientLoop(client); }));
        }
    }

    void ClientLoop(const std::shared_ptr<Client>& client)
    {
        while (running && client->alive)
        {
            std::string request;
            if (!ReceiveRequest(client, request)) break;
            try
            {
                if (!HandleRequest(client, request)) break;
            }
            catch (const std::exception&)
            {
                Trace(client, "malformed RTSP request rejected");
                SendResponse(client, "0", "400 Bad Request", {}, {});
                break;
            }
        }

        client->playing = false;
        client->alive = false;
        Trace(client, "closed; RTP packets=" + std::to_string(client->sentPackets.load()) +
            ", frames=" + std::to_string(client->sentFrames.load()) +
            ", send_error=" + std::to_string(client->sendError.load()));
        shutdown(client->rtspSocket, SD_BOTH);
        closesocket(client->rtspSocket);
        if (client->rtpSocket != INVALID_SOCKET) closesocket(client->rtpSocket);
        if (client->rtcpSocket != INVALID_SOCKET) closesocket(client->rtcpSocket);
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
        client->threadFinished = true;
    }

    bool ReceiveRequest(const std::shared_ptr<Client>& client, std::string& request)
    {
        while (true)
        {
            const auto& input = client->receiveBuffer;
            if (!input.empty() && input[0] == '$')
            {
                // Interleaved client RTCP is binary, not an RTSP request.
                if (input.size() >= 4)
                {
                    const size_t length = (static_cast<unsigned char>(input[2]) << 8) |
                                           static_cast<unsigned char>(input[3]);
                    if (input.size() >= length + 4)
                    { client->receiveBuffer.erase(0, length + 4); continue; }
                }
            }
            else
            {
                const size_t delimiter = input.find("\r\n\r\n");
                if (delimiter != std::string::npos)
                {
                    size_t bodySize = 0;
                    const std::string length = Security::Header(input, "content-length");
                    if (!length.empty())
                    {
                        if (length.size() > 5 || !std::all_of(length.begin(), length.end(),
                            [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
                        bodySize = static_cast<size_t>(std::stoul(length));
                    }
                    const size_t total = delimiter + 4 + bodySize;
                    if (total > 64 * 1024) return false;
                    if (input.size() >= total)
                    {
                        request = input.substr(0, total);
                        client->receiveBuffer.erase(0, total);
                        return true;
                    }
                }
            }
            std::array<char, 4096> buffer{};
            const int received = recv(client->rtspSocket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) return false;
            client->receiveBuffer.append(buffer.data(), static_cast<size_t>(received));
            if (client->receiveBuffer.size() > 128 * 1024) return false;
        }
    }

    bool HandleRequest(const std::shared_ptr<Client>& client, const std::string& request)
    {
        std::istringstream stream(request);
        std::string firstLine;
        std::getline(stream, firstLine);
        firstLine = Trim(firstLine);
        std::istringstream firstLineStream(firstLine);
        std::string method;
        std::string uri;
        std::string version;
        firstLineStream >> method >> uri >> version;
        client->diagnosticMethod = DiagnosticMethod(method);
        client->diagnosticAuth = authenticationEnabled ? "not supplied" : "disabled";
        client->diagnosticReason.clear();

        std::map<std::string, std::string> headers;
        std::string line;
        while (std::getline(stream, line))
        {
            line = Trim(line);
            if (line.empty()) break;
            const size_t separator = line.find(':');
            if (separator != std::string::npos)
                headers[ToLower(Trim(line.substr(0, separator)))] = Trim(line.substr(separator + 1));
        }
        const std::string cseq = headers.count("cseq") ? headers["cseq"] : "0";
        if (!client->setup && client->peerAddress == "127.0.0.1" &&
            Security::Header(request, "user-agent") == "Screen2NVR-HealthProbe") client->internalProbe = true;

        if (authenticationEnabled && method != "OPTIONS")
        {
            const std::string authorization = Security::Header(request, "authorization");
            std::string reason;
            const bool basic = ToLower(authorization.substr(0, authorization.find_first_of(" \t"))) == "basic";
            const bool valid = basic ? Security::Basic(authorization, userName, password) :
                digestAuthentication.Validate(authorization, method, uri, userName, password, client->peerAddress, reason);
            if (basic && !valid) reason = "Basic credentials mismatch";
            client->diagnosticAuth = authorization.empty() ? "not supplied" : basic ? "Basic" : "Digest/other";
            if (!valid)
            {
                client->diagnosticReason = authorization.empty() ? "initial challenge" : reason;
                // Some camera/NVR clients cannot complete the qop/nc/cnonce exchange.
                // Offer RTSP 1.0's classic MD5; ONVIF HTTP keeps its qop=auth challenge.
                // Validation still checks the password, URI, method and peer-bound nonce.
                const std::string challenge = digestAuthentication.Challenge(client->peerAddress,
                    reason == "Digest nonce expired or unknown", Security::DigestChallengeMode::LegacyMd5);
                if (challenge.empty()) return SendResponse(client, cseq, "503 Service Unavailable", {}, {});
                return SendResponse(client, cseq, "401 Unauthorized",
                                    { { "WWW-Authenticate", challenge } }, {});
            }
        }

        if (method == "OPTIONS")
        {
            return SendResponse(client, cseq, "200 OK",
                                { { "Public", "OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN" } }, {});
        }
        std::string requestPath = uri;
        if (uri.compare(0, 7, "rtsp://") == 0)
        {
            const size_t slash = uri.find('/', 7);
            requestPath = slash == std::string::npos ? "/" : uri.substr(slash);
        }
        unsigned streamIndex = static_cast<unsigned>(streams.size());
        for (unsigned index = 0; index < streams.size(); ++index)
        {
            if (streams[index].path.empty()) continue;
            const std::string base = "/" + streams[index].path;
            if (requestPath == base || requestPath == base + "/" ||
                (method == "SETUP" && requestPath == base + "/trackID=0")) streamIndex = index;
        }
        if (streamIndex >= streams.size())
            return SendResponse(client, cseq, "404 Not Found", {}, {});
        if (client->setup && client->streamIndex != streamIndex)
            return SendResponse(client, cseq, "455 Method Not Valid in This State", {}, {});
        auto& channel = streams[streamIndex];
        if (method == "DESCRIBE")
        {
            const std::string sdp = BuildSdp(streamIndex);
            if (sdp.empty())
                return SendResponse(client, cseq, "503 Service Unavailable", { { "Retry-After", "1" } }, {});
            return SendResponse(client, cseq, "200 OK",
                                { { "Content-Type", "application/sdp" },
                                  { "Content-Base", uri.back() == '/' ? uri : uri + "/" } }, sdp);
        }
        if (method == "SETUP")
        {
            if (client->setup) return SendResponse(client, cseq, "455 Method Not Valid in This State", {}, {});
            client->streamIndex = streamIndex;
            const auto transportIterator = headers.find("transport");
            if (transportIterator == headers.end())
                return SendResponse(client, cseq, "461 Unsupported Transport", {}, {});
            const std::string transport = transportIterator->second;
            const std::string lowerTransport = ToLower(transport);
            std::string responseTransport;

            if (lowerTransport.find("rtp/avp/tcp") != std::string::npos)
            {
                client->transport = Transport::tcp;
                const size_t interleaved = lowerTransport.find("interleaved=");
                if (interleaved != std::string::npos)
                {
                    const int interleavedNumber = std::stoi(lowerTransport.substr(interleaved + 12));
                    if (interleavedNumber < 0 || interleavedNumber > 254)
                        return SendResponse(client, cseq, "461 Unsupported Transport", {}, {});
                    client->interleavedChannel = static_cast<uint8_t>(interleavedNumber);
                }
                responseTransport = "RTP/AVP/TCP;unicast;interleaved=" +
                                    std::to_string(client->interleavedChannel) + "-" +
                                    std::to_string(client->interleavedChannel + 1) + ";ssrc=" + SsrcText(streamIndex);
            }
            else if (lowerTransport.find("rtp/avp") != std::string::npos)
            {
                const size_t portPosition = lowerTransport.find("client_port=");
                if (portPosition == std::string::npos)
                    return SendResponse(client, cseq, "461 Unsupported Transport", {}, {});
                const int parsedClientPort = std::stoi(lowerTransport.substr(portPosition + 12));
                if (parsedClientPort < 1 || parsedClientPort > 65534)
                    return SendResponse(client, cseq, "461 Unsupported Transport", {}, {});
                const uint16_t clientRtpPort = static_cast<uint16_t>(parsedClientPort);
                sockaddr_in peer{};
                int peerLength = sizeof(peer);
                if (getpeername(client->rtspSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength) == SOCKET_ERROR)
                    return false;
                client->rtpDestination = peer;
                client->rtpDestination.sin_port = htons(clientRtpPort);
                client->rtpSocket = CreateBoundUdpSocket();
                client->rtcpSocket = CreateBoundUdpSocket();
                client->transport = Transport::udp;
                responseTransport = "RTP/AVP;unicast;client_port=" + std::to_string(clientRtpPort) + "-" +
                                    std::to_string(clientRtpPort + 1) + ";server_port=" +
                                    std::to_string(SocketPort(client->rtpSocket)) + "-" +
                                    std::to_string(SocketPort(client->rtcpSocket)) + ";ssrc=" + SsrcText(streamIndex);
            }
            else
            {
                return SendResponse(client, cseq, "461 Unsupported Transport", {}, {});
            }

            client->setup = true;
            return SendResponse(client, cseq, "200 OK",
                                { { "Transport", responseTransport },
                                  { "Session", client->sessionId + ";timeout=60" } }, {});
        }
        if (method == "PLAY")
        {
            if (!client->setup) return SendResponse(client, cseq, "455 Method Not Valid in This State", {}, {});
            const bool sent = SendResponse(client, cseq, "200 OK",
                                { { "Session", client->sessionId },
                                  { "RTP-Info", "url=" + uri + (uri.back() == '/' ? "" : "/") + "trackID=0;seq=" +
                                                std::to_string(channel.sequence.load()) + ";rtptime=" +
                                                std::to_string(channel.lastTimestamp.load()) } }, {});
            // The PLAY response must precede interleaved video on this connection.
            client->playing = sent;
            return sent;
        }
        if (method == "GET_PARAMETER")
            return SendResponse(client, cseq, "200 OK", { { "Session", client->sessionId } }, {});
        if (method == "TEARDOWN")
        {
            SendResponse(client, cseq, "200 OK", { { "Session", client->sessionId } }, {});
            return false;
        }
        return SendResponse(client, cseq, "405 Method Not Allowed", {}, {});
    }

    std::string BuildSdp(unsigned streamIndex)
    {
        std::lock_guard<std::mutex> lock(codecMutex);
        const auto& sps = streams[streamIndex].sps;
        const auto& pps = streams[streamIndex].pps;
        if (sps.size() < 4 || pps.empty()) return {};
        std::ostringstream profile;
        profile << std::uppercase << std::hex << std::setfill('0')
                << std::setw(2) << static_cast<unsigned int>(sps[1])
                << std::setw(2) << static_cast<unsigned int>(sps[2])
                << std::setw(2) << static_cast<unsigned int>(sps[3]);
        return "v=0\r\n"
               "o=- 0 0 IN IP4 0.0.0.0\r\n"
               "s=Screen2NVR\r\n"
               "c=IN IP4 0.0.0.0\r\n"
               "t=0 0\r\n"
               "a=control:*\r\n"
               "m=video 0 RTP/AVP 96\r\n"
               "a=control:trackID=0\r\n"
               "a=rtpmap:96 H264/90000\r\n"
               "a=fmtp:96 packetization-mode=1;profile-level-id=" + profile.str() +
               ";sprop-parameter-sets=" + Base64(sps) + "," + Base64(pps) + "\r\n";
    }

    bool IsIpAllowed(const std::string& address) const
    {
        return Security::IpAllowed(address, allowedIpAddresses);
    }

    RtspClientStatistics ClientStatistics() const
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        RtspClientStatistics result;
        std::set<std::string> addresses;
        for (const auto& client : clients)
        {
            if (!client->playing.load() || !client->alive.load() || client->internalProbe.load()) continue;
            addresses.insert(client->peerAddress);
            if (client->streamIndex == 0) ++result.mainSessions; else ++result.subSessions;
        }
        result.devices = static_cast<uint32_t>(addresses.size());
        return result;
    }

    bool Probe(unsigned streamIndex) const
    {
        if (streamIndex >= streams.size() || streams[streamIndex].path.empty()) return false;
        SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (probe == INVALID_SOCKET) return false;
        DWORD timeout = 1500;
        setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        bool ok = connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR;
        const std::string uri = "rtsp://127.0.0.1:" + std::to_string(port) + "/" + streams[streamIndex].path;
        const std::string authorization = authenticationEnabled
            ? "Authorization: Basic " + Security::Base64(userName + ":" + password) + "\r\n" : std::string();
        std::string pending;
        auto receive = [&]() -> bool
        {
            std::array<char, 8192> buffer{};
            const int received = recv(probe, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) return false;
            pending.append(buffer.data(), static_cast<size_t>(received));
            return pending.size() <= 128 * 1024;
        };
        auto request = [&](const std::string& method, const std::string& requestUri, int cseq,
                           const std::string& headers, std::string& response) -> bool
        {
            const std::string text = method + " " + requestUri + " RTSP/1.0\r\nCSeq: " +
                std::to_string(cseq) + "\r\nUser-Agent: Screen2NVR-HealthProbe\r\n" + authorization + headers + "\r\n";
            if (!SendAll(probe, reinterpret_cast<const uint8_t*>(text.data()), text.size())) return false;
            size_t delimiter = pending.find("\r\n\r\n");
            while (delimiter == std::string::npos)
            {
                if (pending.size() > 8192 || !receive()) return false;
                delimiter = pending.find("\r\n\r\n");
            }
            const std::string contentLength = Security::Header(pending, "content-length");
            size_t bodySize = 0;
            if (!contentLength.empty())
            {
                if (contentLength.size() > 5 || !std::all_of(contentLength.begin(), contentLength.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
                bodySize = static_cast<size_t>(std::stoul(contentLength));
            }
            const size_t total = delimiter + 4 + bodySize;
            if (total > 64 * 1024) return false;
            while (pending.size() < total) if (!receive()) return false;
            response = pending.substr(0, total);
            pending.erase(0, total); // Keep RTP bytes that arrived in the same TCP read.
            return response.rfind("RTSP/1.0 200 ", 0) == 0;
        };
        std::string response;
        if (ok) ok = request("DESCRIBE", uri, 1, "Accept: application/sdp\r\n", response);
        if (ok) ok = request("SETUP", uri + "/trackID=0", 2,
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", response);
        std::string session;
        if (ok)
        {
            const size_t position = ToLower(response).find("session:");
            if (position != std::string::npos)
            {
                const size_t start = position + 8;
                const size_t end = response.find_first_of(";\r\n", start);
                session = Trim(response.substr(start, end - start));
            }
            ok = !session.empty();
        }
        if (ok) ok = request("PLAY", uri, 3, "Session: " + session + "\r\n", response);
        if (ok)
        {
            while (pending.size() < 4 && ok) ok = receive();
            if (ok)
            {
                const size_t length = (static_cast<unsigned char>(pending[2]) << 8) |
                                      static_cast<unsigned char>(pending[3]);
                ok = pending[0] == '$' && pending[1] == 0 && length >= 13;
                while (ok && pending.size() < length + 4) ok = receive();
                if (ok) ok = (static_cast<unsigned char>(pending[4]) >> 6) == 2 &&
                             (static_cast<unsigned char>(pending[5]) & 0x7F) == kPayloadType;
            }
        }
        closesocket(probe);
        return ok;
    }

    void Trace(const std::shared_ptr<Client>& client, const std::string& event)
    {
        // Exclude the internal watchdog. Bound ALL session diagnostics, including
        // rejected/reconnecting peers, to 48 lines/minute; regular logs rotate too.
        if (client->peerAddress != "127.0.0.1" && connectionLog.ShouldLog())
            logger("RTSP peer=" + client->peerAddress + " conn=" + std::to_string(client->diagnosticId) + " " + event);
    }

    bool SendResponse(const std::shared_ptr<Client>& client, const std::string& cseq,
                      const std::string& status,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& body)
    {
        const std::string key = std::string(client->diagnosticMethod) + " " + status;
        if (client->loggedResponses.size() < 16 && client->loggedResponses.insert(key + client->diagnosticReason).second)
        {
            std::string event = key + "; auth=" + client->diagnosticAuth;
            if (!client->diagnosticReason.empty()) event += "; " + client->diagnosticReason;
            if (client->setup)
                event += "; stream=" + std::to_string(client->streamIndex + 1) +
                    (client->transport == Transport::tcp ? "; transport=TCP" : "; transport=UDP");
            Trace(client, event);
        }
        std::ostringstream response;
        response << "RTSP/1.0 " << status << "\r\nCSeq: " << cseq << "\r\nServer: Screen2NVR\r\n";
        for (const auto& header : headers) response << header.first << ": " << header.second << "\r\n";
        if (!body.empty()) response << "Content-Length: " << body.size() << "\r\n";
        response << "\r\n" << body;
        const std::string output = response.str();
        std::lock_guard<std::mutex> lock(client->sendMutex);
        const bool sent = SendAll(client->rtspSocket, reinterpret_cast<const uint8_t*>(output.data()), output.size());
        if (!sent) client->sendError = WSAGetLastError();
        return sent;
    }

    void SendNal(const uint8_t* nal, size_t size, uint32_t timestamp, bool marker, unsigned streamIndex)
    {
        if (size <= kMaximumRtpPayload)
        {
            SendPacket(nal, size, timestamp, marker, streamIndex);
            return;
        }
        const uint8_t nalHeader = nal[0];
        const uint8_t fuIndicator = static_cast<uint8_t>((nalHeader & 0xE0) | 28);
        const uint8_t nalType = static_cast<uint8_t>(nalHeader & 0x1F);
        size_t offset = 1;
        bool first = true;
        while (offset < size)
        {
            const size_t chunk = std::min(kMaximumRtpPayload - 2, size - offset);
            const bool last = offset + chunk == size;
            std::array<uint8_t, kMaximumRtpPayload> payload{};
            payload[0] = fuIndicator;
            payload[1] = static_cast<uint8_t>(nalType | (first ? 0x80 : 0) | (last ? 0x40 : 0));
            std::copy_n(nal + offset, chunk, payload.data() + 2);
            SendPacket(payload.data(), chunk + 2, timestamp, marker && last, streamIndex);
            first = false;
            offset += chunk;
        }
    }

    void SendPacket(const uint8_t* payload, size_t payloadSize, uint32_t timestamp, bool marker, unsigned streamIndex)
    {
        std::array<uint8_t, 12 + kMaximumRtpPayload> packet{};
        const uint16_t packetSequence = streams[streamIndex].sequence.fetch_add(1);
        const uint32_t ssrc = streams[streamIndex].ssrc;
        packet[0] = 0x80;
        packet[1] = static_cast<uint8_t>(kPayloadType | (marker ? 0x80 : 0));
        packet[2] = static_cast<uint8_t>(packetSequence >> 8);
        packet[3] = static_cast<uint8_t>(packetSequence);
        packet[4] = static_cast<uint8_t>(timestamp >> 24);
        packet[5] = static_cast<uint8_t>(timestamp >> 16);
        packet[6] = static_cast<uint8_t>(timestamp >> 8);
        packet[7] = static_cast<uint8_t>(timestamp);
        packet[8] = static_cast<uint8_t>(ssrc >> 24);
        packet[9] = static_cast<uint8_t>(ssrc >> 16);
        packet[10] = static_cast<uint8_t>(ssrc >> 8);
        packet[11] = static_cast<uint8_t>(ssrc);
        std::copy_n(payload, payloadSize, packet.data() + 12);

        std::vector<std::shared_ptr<Client>> snapshot;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            snapshot = clients;
        }
        for (const auto& client : snapshot)
        {
            if (!client->playing || !client->alive || client->streamIndex != streamIndex) continue;
            if (client->transport == Transport::tcp)
            {
                std::array<uint8_t, 4 + 12 + kMaximumRtpPayload> interleaved{};
                interleaved[0] = '$';
                interleaved[1] = client->interleavedChannel;
                const uint16_t length = static_cast<uint16_t>(payloadSize + 12);
                interleaved[2] = static_cast<uint8_t>(length >> 8);
                interleaved[3] = static_cast<uint8_t>(length);
                std::copy_n(packet.data(), length, interleaved.data() + 4);
                std::lock_guard<std::mutex> lock(client->sendMutex);
                if (!SendAll(client->rtspSocket, interleaved.data(), length + 4))
                { client->sendError = WSAGetLastError(); client->playing = false; }
                else
                {
                    if (client->sentPackets.fetch_add(1) == 0) Trace(client, "first RTP packet sent; transport=TCP");
                    if (marker) ++client->sentFrames;
                }
            }
            else if (client->transport == Transport::udp)
            {
                const int sent = sendto(client->rtpSocket, reinterpret_cast<const char*>(packet.data()),
                                        static_cast<int>(payloadSize + 12), 0,
                                        reinterpret_cast<const sockaddr*>(&client->rtpDestination),
                                        sizeof(client->rtpDestination));
                if (sent == SOCKET_ERROR) { client->sendError = WSAGetLastError(); client->playing = false; }
                else
                {
                    if (client->sentPackets.fetch_add(1) == 0) Trace(client, "first RTP packet sent; transport=UDP");
                    if (marker) ++client->sentFrames;
                }
            }
        }
    }

    SOCKET CreateBoundUdpSocket()
    {
        SOCKET result = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (result == INVALID_SOCKET) ThrowSocket("RTP UDP socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = 0;
        if (bind(result, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            closesocket(result);
            ThrowSocket("RTP UDP bind");
        }
        return result;
    }

    static uint16_t SocketPort(SOCKET socketValue)
    {
        sockaddr_in address{};
        int length = sizeof(address);
        if (getsockname(socketValue, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) return 0;
        return ntohs(address.sin_port);
    }

    std::string SsrcText(unsigned streamIndex) const
    {
        std::ostringstream value;
        value << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << streams[streamIndex].ssrc;
        return value.str();
    }

    [[noreturn]] static void ThrowSocket(const char* operation)
    {
        throw std::runtime_error(std::string(operation) + " failed: WSA " + std::to_string(WSAGetLastError()));
    }

    LogCallback logger;
    Security::DigestAuthenticator digestAuthentication;
    Security::AuthenticationLogLimiter connectionLog{ 60'000, 48 };
    uint64_t nextDiagnosticId = 1; // Only the accept thread increments this counter.
    uint16_t port = 554;
    std::string path;
    bool authenticationEnabled = false;
    std::string userName;
    std::string password;
    std::string allowedIpAddresses;
    std::atomic_bool running{ false };
    SOCKET listener = INVALID_SOCKET;
    std::thread acceptThread;
    std::vector<std::pair<std::shared_ptr<Client>, std::thread>> clientThreads;
    mutable std::mutex clientsMutex;
    std::vector<std::shared_ptr<Client>> clients;
    std::mutex codecMutex;
    struct Stream
    {
        std::string path;
        std::vector<uint8_t> sps, pps;
        std::atomic<uint16_t> sequence{ 0 };
        std::atomic<uint32_t> lastTimestamp{ 0 };
        uint32_t ssrc = 0;
    };
    std::array<Stream, 2> streams;
};

RtspServer::RtspServer(LogCallback logger) : impl_(std::make_unique<Impl>(std::move(logger))) {}
RtspServer::~RtspServer() = default;
void RtspServer::Start(uint16_t port, const std::string& path, bool authenticationEnabled,
                       const std::string& userName, const std::string& password,
                       const std::string& allowedIpAddresses, const std::string& subPath)
{
    impl_->Start(port, path, authenticationEnabled, userName, password, allowedIpAddresses, subPath);
}
void RtspServer::Stop() { impl_->Stop(); }
void RtspServer::PublishAccessUnit(const uint8_t* data, size_t size, int64_t sampleTime100ns, unsigned streamIndex)
{
    impl_->Publish(data, size, sampleTime100ns, streamIndex);
}
RtspClientStatistics RtspServer::ClientStatistics() const { return impl_->ClientStatistics(); }
bool RtspServer::Probe(unsigned streamIndex) const { return impl_->Probe(streamIndex); }
