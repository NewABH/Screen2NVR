#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <objbase.h>
#include <xmllite.h>
#include <wrl/client.h>

#include "Security.h"
#include "Settings.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace Security
{
namespace
{
std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string Lower(std::string value)
{
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
bool Equal(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    unsigned int difference = 0;
    for (size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    return difference == 0;
}
bool ParseDigest(const std::string& header, std::map<std::string, std::string>& fields)
{
    const size_t separator = header.find_first_of(" \t");
    if (separator == std::string::npos || Lower(header.substr(0, separator)) != "digest" ||
        header.size() > 16 * 1024) return false;
    size_t pos = separator;
    const auto skipSpace = [&]() { while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t')) ++pos; };
    const auto isToken = [](unsigned char ch) {
        return std::isalnum(ch) || std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(ch)) != std::string_view::npos;
    };
    while (pos < header.size())
    {
        skipSpace();
        const size_t start = pos;
        while (pos < header.size() && isToken(static_cast<unsigned char>(header[pos]))) ++pos;
        if (pos == start) return false;
        const std::string key = Lower(header.substr(start, pos - start));
        skipSpace();
        if (pos == header.size() || header[pos++] != '=') return false;
        skipSpace();
        std::string value;
        if (pos < header.size() && header[pos] == '"')
        {
            ++pos;
            bool closed = false;
            while (pos < header.size())
            {
                char ch = header[pos++];
                if (ch == '"') { closed = true; break; }
                if (ch == '\\') { if (pos == header.size()) return false; ch = header[pos++]; }
                if (static_cast<unsigned char>(ch) < 32 || ch == 127) return false;
                value += ch;
            }
            if (!closed) return false;
        }
        else
        {
            const size_t valueStart = pos;
            while (pos < header.size() && isToken(static_cast<unsigned char>(header[pos]))) ++pos;
            if (pos == valueStart) return false;
            value = header.substr(valueStart, pos - valueStart);
        }
        if (!fields.emplace(key, value).second || fields.size() > 32) return false;
        skipSpace();
        if (pos == header.size()) break;
        if (header[pos++] != ',' || pos == header.size()) return false;
    }
    return !fields.empty();
}
bool DigestUriMatches(const std::string& supplied, const std::string& requested,
                      const std::string& method, std::string& reason)
{
    // Reasons contain only fixed categories, never any client-supplied URI text.
    reason = "Digest URI mismatch (empty signed URI)";
    if (supplied.empty()) return false;
    if (supplied == requested) return true;
    reason = "Digest URI mismatch (exact HTTP URI required)";
    if (method != "DESCRIBE" && method != "SETUP" && method != "PLAY" &&
        method != "GET_PARAMETER" && method != "TEARDOWN" && method != "OPTIONS") return false;
    // HTTP digest-uri remains exact. RTSP clients also use relative paths and
    // equivalent authorities with/without the default port. Do not resolve DNS
    // or discard a different host, port, query, or user-info to make a match.
    const auto splitRtsp = [](const std::string& uri, std::string& authority, std::string& path) {
        if (Lower(uri.substr(0, 7)) != "rtsp://") return false;
        const size_t slash = uri.find('/', 7);
        if (slash == std::string::npos || slash == 7) return false;
        authority = Lower(uri.substr(7, slash - 7));
        if (authority.find_first_of("@?#") != std::string::npos) return false;
        if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":554") == 0)
            authority.resize(authority.size() - 4);
        path = uri.substr(slash);
        return !authority.empty();
    };
    std::string requestedHost, requestedPath, suppliedHost, suppliedPath;
    reason = "Digest URI mismatch (request URI format or authority invalid)";
    if (!splitRtsp(requested, requestedHost, requestedPath)) return false;
    if (supplied[0] == '/') suppliedPath = supplied;
    else
    {
        reason = "Digest URI mismatch (signed URI format or authority invalid)";
        if (!splitRtsp(supplied, suppliedHost, suppliedPath)) return false;
        reason = "Digest URI mismatch (host or port differs)";
        if (suppliedHost != requestedHost) return false;
    }
    if (suppliedPath == requestedPath) return true;
    // The RTSP router exposes /stream and /stream/ as the same resource. SDP's
    // Content-Base ends in '/', whereas the ONVIF stream URI need not. Compare
    // these aliases consistently for PLAY/keepalive/TEARDOWN as well as DESCRIBE.
    // Only the comparison is normalized: HA2 below hashes the EXACT signed URI.
    const bool plainPaths = suppliedPath.find_first_of("?#") == std::string::npos &&
        requestedPath.find_first_of("?#") == std::string::npos &&
        suppliedPath.find("//") == std::string::npos && requestedPath.find("//") == std::string::npos;
    if (plainPaths && (suppliedPath == requestedPath + "/" || suppliedPath + "/" == requestedPath)) return true;
    reason = "Digest URI mismatch (stream path differs)";
    if (!plainPaths) reason = "Digest URI mismatch (query, fragment or duplicate slash differs)";
    else if (Lower(suppliedPath) == Lower(requestedPath)) reason = "Digest URI mismatch (path letter case differs)";
    else if (suppliedPath.find("/trackID=") != std::string::npos || requestedPath.find("/trackID=") != std::string::npos)
        reason = "Digest URI mismatch (stream/track path differs)";
    // VLC/NVR implementations may sign SETUP using the stream URL instead of
    // its track URL. Limit the exception to SETUP and our one advertised track;
    // still hash the EXACT signed URI, and never accept another stream's base.
    constexpr std::string_view track = "/trackID=0";
    if (method != "SETUP" || requestedPath.size() <= track.size() ||
        requestedPath.compare(requestedPath.size() - track.size(), track.size(), track) != 0) return false;
    const std::string base = requestedPath.substr(0, requestedPath.size() - track.size());
    return suppliedPath == base || suppliedPath == base + "/";
}
std::string Hash(LPCWSTR algorithmName, const std::string& input, ULONG size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, algorithmName, nullptr, 0) < 0) return {};
    std::string digest(size, '\0');
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()),
        reinterpret_cast<PUCHAR>(digest.data()), size);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0 ? digest : std::string();
}
bool Decode64(const std::string& encoded, std::string& decoded)
{
    if (encoded.empty() || encoded.size() > 4096) return false;
    DWORD size = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
                              nullptr, &size, nullptr, nullptr)) return false;
    decoded.resize(size);
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(decoded.data()), &size, nullptr, nullptr)) return false;
    decoded.resize(size);
    return Base64(decoded) == encoded; // Reject noncanonical encodings and trailing data.
}
bool FreshTimestamp(const std::string& text)
{
    if (text.size() < 20 || text.size() > 30 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text.back() != 'Z') return false;
    const auto number = [&](size_t start, size_t count) -> WORD
    {
        unsigned value = 0;
        for (size_t i = start; i < start + count; ++i)
        {
            if (text[i] < '0' || text[i] > '9') return 65535;
            value = value * 10 + text[i] - '0';
        }
        return static_cast<WORD>(value);
    };
    if (text.size() > 20)
    {
        if (text[19] != '.' || text.size() < 22) return false;
        for (size_t i = 20; i + 1 < text.size(); ++i) if (text[i] < '0' || text[i] > '9') return false;
    }
    SYSTEMTIME time{};
    time.wYear = number(0, 4); time.wMonth = number(5, 2); time.wDay = number(8, 2);
    time.wHour = number(11, 2); time.wMinute = number(14, 2); time.wSecond = number(17, 2);
    FILETIME file{}, now{};
    if (!SystemTimeToFileTime(&time, &file)) return false;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER sent{}, current{};
    sent.LowPart = file.dwLowDateTime; sent.HighPart = file.dwHighDateTime;
    current.LowPart = now.dwLowDateTime; current.HighPart = now.dwHighDateTime;
    const auto difference = sent.QuadPart > current.QuadPart ? sent.QuadPart - current.QuadPart : current.QuadPart - sent.QuadPart;
    return difference <= 300ULL * 10'000'000;
}
}

std::string Base64(std::string_view bytes)
{
    if (bytes.empty()) return {};
    DWORD size = 0;
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), flags,
                              nullptr, &size)) return {};
    std::string output(size, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), flags,
                              output.data(), &size)) return {};
    output.resize(size);
    while (!output.empty() && output.back() == '\0') output.pop_back();
    return output;
}
std::string Md5Hex(const std::string& value)
{
    const std::string digest = Hash(BCRYPT_MD5_ALGORITHM, value, 16);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::string Header(const std::string& request, const std::string& name, bool* present)
{
    if (present) *present = false;
    const size_t end = request.find("\r\n\r\n");
    if (end == std::string::npos) return {};
    const std::string wanted = Lower(name);
    std::string result;
    bool found = false, currentWanted = false;
    size_t start = request.find("\r\n");
    while (start < end)
    {
        start += 2;
        const size_t next = request.find("\r\n", start);
        // RTSP 1.0 permits folded fields. Unfold only the current field; a line
        // resembling Authorization inside another field must never become one.
        if (request[start] == ' ' || request[start] == '\t')
        {
            if (currentWanted) result += " " + Trim(request.substr(start, next - start));
            start = next;
            continue;
        }
        const size_t colon = request.find(':', start);
        currentWanted = colon < next && Lower(request.substr(start, colon - start)) == wanted;
        if (currentWanted)
        {
            if (present) *present = true;
            if (found) return {}; // Duplicate authorization fields are ambiguous.
            found = true;
            result = Trim(request.substr(colon + 1, next - colon - 1));
        }
        start = next;
    }
    return result;
}
bool Basic(const std::string& authorization, const std::string& user, const std::string& password)
{
    if (user.empty() || password.empty()) return false;
    const auto separator = authorization.find_first_of(" \t");
    if (separator == std::string::npos || Lower(authorization.substr(0, separator)) != "basic") return false;
    return Equal(Trim(authorization.substr(separator)), Base64(user + ":" + password));
}

std::string DigestAuthenticator::Challenge(const std::string& peer, bool stale, DigestChallengeMode mode)
{
    std::array<UCHAR, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {}; // Fail closed if Windows cannot generate a secure nonce.
    // 128 cryptographically random bits, encoded as camera-style ASCII hex.
    constexpr char hex[] = "0123456789abcdef";
    std::string nonce;
    nonce.reserve(bytes.size() * 2);
    for (const auto byte : bytes) { nonce += hex[byte >> 4]; nonce += hex[byte & 15]; }
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now = GetTickCount64();
    for (auto it = nonces_.begin(); it != nonces_.end();)
        if (now - it->second.issued > 300'000) it = nonces_.erase(it); else ++it;
    if (nonces_.size() >= 512)
    {
        const auto oldest = std::min_element(nonces_.begin(), nonces_.end(),
            [](const auto& a, const auto& b) { return a.second.issued < b.second.issued; });
        nonces_.erase(oldest);
    }
    nonces_[nonce] = { now, peer, {} };
    return "Digest realm=\"Screen2NVR\", nonce=\"" + nonce + "\", algorithm=MD5" +
        (mode == DigestChallengeMode::QopAuth ? ", qop=\"auth\"" : "") +
        (stale ? ", stale=true" : "");
}

bool DigestAuthenticator::Validate(const std::string& authorization, const std::string& method, const std::string& uri,
                                   const std::string& user, const std::string& password, const std::string& peer,
                                   std::string& reason)
{
    reason = "malformed Digest header or unsupported authorization scheme";
    std::map<std::string, std::string> fields;
    if (user.empty() || password.empty() || !ParseDigest(authorization, fields)) return false;
    const auto value = [&](const char* key) -> std::string {
        const auto found = fields.find(key); return found == fields.end() ? std::string() : found->second;
    };
    const std::string nonce = value("nonce"), cnonce = value("cnonce"), qop = value("qop"), nc = value("nc");
    const std::string algorithm = Lower(value("algorithm"));
    if (value("username") != user) { reason = "Digest username mismatch"; return false; }
    if (value("realm") != "Screen2NVR") { reason = "Digest realm mismatch"; return false; }
    if (!DigestUriMatches(value("uri"), uri, method, reason)) return false;
    if (value("response").size() != 32) { reason = "invalid Digest response length (expected MD5)"; return false; }
    if (!algorithm.empty() && algorithm != "md5" && algorithm != "md5-sess")
        { reason = "unsupported Digest algorithm (expected MD5 or MD5-sess)"; return false; }
    uint32_t count = 0;
    if (!qop.empty())
    {
        if (qop != "auth") { reason = "unsupported Digest qop (expected auth)"; return false; }
        if (nc.size() != 8 ||
            !std::all_of(nc.begin(), nc.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
            { reason = "invalid Digest nonce-count (expected 8 hex digits)"; return false; }
        if (cnonce.empty() || cnonce.size() > 256)
            { reason = "invalid Digest cnonce length"; return false; }
        count = static_cast<uint32_t>(std::stoul(nc, nullptr, 16));
        if (!count) { reason = "invalid Digest nonce-count (zero)"; return false; }
    }
    else if (!nc.empty()) { reason = "Digest nonce-count without qop"; return false; }
    std::string ha1 = Md5Hex(user + ":Screen2NVR:" + password);
    const std::string ha2 = Md5Hex(method + ":" + value("uri"));
    if (ha1.empty() || ha2.empty()) return false;
    if (algorithm == "md5-sess")
    {
        if (cnonce.empty() || cnonce.size() > 256) return false;
        ha1 = Md5Hex(ha1 + ":" + nonce + ":" + cnonce);
        if (ha1.empty()) return false;
    }
    const std::string expected = qop.empty() ? Md5Hex(ha1 + ":" + nonce + ":" + ha2)
        : Md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    if (expected.empty() || !Equal(Lower(value("response")), expected))
        { reason = "Digest response mismatch (password or client digest calculation)"; return false; }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = nonces_.find(nonce);
    if (found == nonces_.end() || GetTickCount64() - found->second.issued > 300'000)
    {
        reason = "Digest nonce expired or unknown";
        return false;
    }
    if (found->second.peer != peer) { reason = "Digest peer IP mismatch"; return false; }
    if (!qop.empty())
    {
        auto& counts = found->second.counts;
        const auto previous = counts.find(cnonce);
        if ((previous != counts.end() && count <= previous->second) ||
            (previous == counts.end() && counts.size() >= 128))
        {
            reason = "Digest replay or invalid nonce-count";
            return false;
        }
        counts[cnonce] = count;
    }
    reason.clear();
    return true;
}

bool AuthenticationLogLimiter::ShouldLog()
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t now = GetTickCount64();
    if (!used_ || now - last_ >= intervalMs_) { last_ = now; used_ = 0; }
    if (used_ >= burst_) return false;
    ++used_;
    return true;
}
bool IpAllowed(const std::string& address, const std::string& list)
{
    if (address == "127.0.0.1" || Trim(list).empty()) return true;
    std::string input = list;
    std::replace(input.begin(), input.end(), ';', ',');
    std::istringstream stream(input);
    std::string item;
    while (std::getline(stream, item, ',')) if (Trim(item) == address) return true;
    return false;
}
bool NormalizeIpList(const std::wstring& value, std::wstring& normalized)
{
    std::string text = WideToUtf8String(value);
    normalized.clear();
    std::replace(text.begin(), text.end(), ';', ',');
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        item = Trim(item);
        if (item.empty()) continue;
        in_addr address{};
        if (inet_pton(AF_INET, item.c_str(), &address) != 1) return false;
        char canonical[INET_ADDRSTRLEN]{};
        if (!inet_ntop(AF_INET, &address, canonical, sizeof(canonical))) return false;
        if (!normalized.empty()) normalized += L", ";
        normalized.append(canonical, canonical + strlen(canonical));
    }
    return !normalized.empty() || Trim(text).empty();
}

bool OnvifToken(const std::string& xml, const std::string& user, const std::string& password,
                bool* present, std::string* reason)
{
    if (present) *present = false;
    if (reason) *reason = "invalid WS-Security UsernameToken or credentials";
    if (xml.empty() || xml.size() > 64 * 1024 || user.empty() || password.empty()) return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ULONG written = 0;
    if (FAILED(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), &written)) || written != xml.size() ||
        FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return false;
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 24)) || FAILED(reader->SetInput(stream.Get()))) return false;
    constexpr wchar_t soap[] = L"http://www.w3.org/2003/05/soap-envelope";
    constexpr wchar_t soap11[] = L"http://schemas.xmlsoap.org/soap/envelope/";
    constexpr wchar_t wsse[] = L"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
    constexpr wchar_t wsu[] = L"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
    const std::wstring typeRoot = L"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#";
    struct Node { std::wstring name, ns; };
    std::vector<Node> path;
    std::array<std::wstring, 4> values;
    std::array<bool, 4> seen{};
    std::wstring passwordType;
    int tokens = 0, activeField = -1;
    const auto tokenPath = [&]()
    {
        return path.size() >= 4 && path[0].name == L"Envelope" && (path[0].ns == soap || path[0].ns == soap11) &&
            path[1].name == L"Header" && path[1].ns == path[0].ns &&
            path[2].name == L"Security" && path[2].ns == wsse &&
            path[3].name == L"UsernameToken" && path[3].ns == wsse;
    };
    XmlNodeType nodeType{};
    HRESULT hr;
    while ((hr = reader->Read(&nodeType)) == S_OK)
    {
        UINT depth = 0;
        if (FAILED(reader->GetDepth(&depth))) return false;
        if (nodeType == XmlNodeType_Element)
        {
            const wchar_t* name = nullptr; const wchar_t* ns = nullptr;
            if (FAILED(reader->GetLocalName(&name, nullptr)) || FAILED(reader->GetNamespaceUri(&ns, nullptr))) return false;
            if (activeField >= 0) return false; // Credential fields cannot contain nested markup.
            path.resize(depth); path.push_back({ name, ns });
            if (path.size() == 4 && tokenPath())
            {
                if (present) *present = true;
                if (++tokens != 1) return false;
            }
            if (path.size() == 5 && tokenPath())
            {
                int field = -1;
                if (path[4].ns == wsse)
                {
                    if (path[4].name == L"Username") field = 0;
                    else if (path[4].name == L"Password") field = 1;
                    else if (path[4].name == L"Nonce") field = 2;
                }
                if (path[4].ns == wsu && path[4].name == L"Created") field = 3;
                if (field >= 0)
                {
                    if (seen[field]) return false;
                    seen[field] = true;
                    if (field == 1 && reader->MoveToAttributeByName(L"Type", nullptr) == S_OK)
                    {
                        const wchar_t* type = nullptr;
                        if (FAILED(reader->GetValue(&type, nullptr))) return false;
                        passwordType = type; reader->MoveToElement();
                    }
                    if (field == 2 && reader->MoveToAttributeByName(L"EncodingType", nullptr) == S_OK)
                    {
                        const wchar_t* encoding = nullptr;
                        if (FAILED(reader->GetValue(&encoding, nullptr)) || std::wstring(encoding) !=
                            L"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary") return false;
                        reader->MoveToElement();
                    }
                    if (!reader->IsEmptyElement()) activeField = field;
                }
            }
            if (reader->IsEmptyElement()) path.resize(depth);
        }
        else if (nodeType == XmlNodeType_EndElement)
        {
            activeField = -1;
            path.resize(depth);
        }
        else if (activeField >= 0 && (nodeType == XmlNodeType_Text || nodeType == XmlNodeType_CDATA ||
                                      nodeType == XmlNodeType_Whitespace))
        {
            const wchar_t* value = nullptr; UINT length = 0;
            if (FAILED(reader->GetValue(&value, &length))) return false;
            values[activeField].append(value, length);
            if (values[activeField].size() > 4096) return false;
        }
    }
    if (hr != S_FALSE || tokens != 1 || !seen[0] || !seen[1] || WideToUtf8String(values[0]) != user) return false;
    const std::string provided = WideToUtf8String(values[1]);
    if (passwordType.empty() || passwordType == typeRoot + L"PasswordText") return Equal(provided, password);
    if (passwordType != typeRoot + L"PasswordDigest" || !seen[2] || !seen[3]) return false;
    const std::string nonceText = WideToUtf8String(values[2]), created = WideToUtf8String(values[3]);
    std::string nonce;
    if (!Decode64(nonceText, nonce) || nonce.size() < 8 || nonce.size() > 64) return false;
    if (!FreshTimestamp(created))
    {
        if (reason) *reason = "WS-Security timestamp invalid or outside 5-minute window; check PC/NVR time";
        return false;
    }
    const std::string digest = Hash(BCRYPT_SHA1_ALGORITHM, nonce + created + password, 20);
    if (digest.empty() || !Equal(provided, Base64(digest))) return false;
    // Bound memory and reject reused nonces for the full accepted timestamp window (including future skew).
    static std::mutex cacheMutex;
    static std::map<std::string, uint64_t> usedNonces;
    std::lock_guard<std::mutex> lock(cacheMutex);
    const uint64_t now = GetTickCount64();
    for (auto it = usedNonces.begin(); it != usedNonces.end();)
        if (now - it->second > 600'000) it = usedNonces.erase(it); else ++it;
    if (usedNonces.count(nonceText) || usedNonces.size() >= 8192)
    {
        if (reason) *reason = "WS-Security nonce replay or replay-cache limit";
        return false;
    }
    usedNonces.emplace(nonceText, now);
    return true;
}
}
