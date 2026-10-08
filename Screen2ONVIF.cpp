#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iostream>
#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <regex>
#include <random>
#include <map>
#include <set>
#include <mutex>
#include <locale>
#include <vector>

#include "Screen2ONVIF.h"
#include "Security.h"
#include <objbase.h>
#include <xmllite.h>
#include <wrl/client.h>

#pragma comment(lib, "ws2_32.lib")

std::string APP_DIR = "C:\\screen2nvr";
std::string UUID_PATH = "C:\\screen2nvr\\onvif.uuid";

std::string CameraName = "PC Screen Camera";
std::string Manufacturer, Model, Serial;

int OnvifPort = kDefaultOnvifPort;
int RtspPort = 554;
std::string RtspPath = WideToUtf8String(kDefaultMainRtspPath);
uint32_t VideoWidth = 1920;
uint32_t VideoHeight = 1080;
uint32_t VideoFrameRate = 12;
uint32_t VideoBitrateKbps = 2000;
uint32_t VideoGop = 25;
std::string DeviceUuid;
bool AuthenticationEnabled = false;
std::string AuthenticationUser;
std::string AuthenticationPassword;
std::string AllowedIpAddresses;
std::function<void(const std::string&)> EmbeddedLogger;
AppSettings OnvifOverlaySettings;
std::mutex OnvifSettingsMutex;

void ReportEmbeddedError(const std::string& message)
{
    if (EmbeddedLogger) EmbeddedLogger(message);
    else std::cerr << message << "\n";
}

std::string Trim(const std::string& s);


std::string XmlEscape(const std::string& s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
        case '&': r += "&amp;"; break;
        case '<': r += "&lt;"; break;
        case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break;
        case '\'': r += "&apos;"; break;
        default: r += c; break;
        }
    }
    return r;
}

std::string UrlEncode(const std::string& s)
{
    std::ostringstream out;
    const char* hex = "0123456789ABCDEF";

    for (unsigned char c : s)
    {
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out << c;
        }
        else
        {
            out << '%' << hex[c >> 4] << hex[c & 15];
        }
    }

    return out.str();
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}


std::string RandomUuid()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> d(0, 15);

    auto h = [&]() -> char
        {
            const char* hex = "0123456789abcdef";
            return hex[d(gen)];
        };

    std::string s = "urn:uuid:";
    int groups[] = { 8, 4, 4, 4, 12 };

    for (int g = 0; g < 5; g++)
    {
        if (g > 0) s += "-";
        for (int i = 0; i < groups[g]; i++) s += h();
    }

    return s;
}

std::string LoadOrCreateUuid()
{
    std::ifstream f(UUID_PATH);
    if (f.good())
    {
        std::string u;
        std::getline(f, u);
        u = Trim(u);
        if (!u.empty()) return u;
    }

    std::string u = RandomUuid();

    CreateDirectoryA(APP_DIR.c_str(), NULL);
    std::ofstream out(UUID_PATH);
    out << u;

    return u;
}

std::string GetLocalIp()
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s != INVALID_SOCKET)
    {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(65530);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

        connect(s, (sockaddr*)&remote, sizeof(remote));

        sockaddr_in local{};
        int len = sizeof(local);

        if (getsockname(s, (sockaddr*)&local, &len) == 0)
        {
            char buf[64]{};
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
            closesocket(s);
            return buf;
        }

        closesocket(s);
    }

    char hostname[256]{};
    gethostname(hostname, sizeof(hostname));

    addrinfo hints{};
    hints.ai_family = AF_INET;

    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) == 0)
    {
        for (addrinfo* p = result; p; p = p->ai_next)
        {
            sockaddr_in* addr = (sockaddr_in*)p->ai_addr;
            char buf[64]{};
            inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));

            std::string ip = buf;
            if (ip != "127.0.0.1")
            {
                freeaddrinfo(result);
                return ip;
            }
        }

        freeaddrinfo(result);
    }

    return "127.0.0.1";
}

std::string Soap(const std::string& body)
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope "
        "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
        "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
        "xmlns:ter=\"http://www.onvif.org/ver10/error\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
        "<s:Body>" + body + "</s:Body>"
        "</s:Envelope>";
}

std::string MediaServiceCapabilities()
{
    return std::string("<trt:Capabilities OSD=\"true\" SnapshotUri=\"false\" Rotation=\"false\" VideoSourceMode=\"false\">") +
           "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"" +
           (OnvifOverlaySettings.subStreamEnabled ? "2" : "1") + "\"/>"
           "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/>"
           "</trt:Capabilities>";
}

std::string DeviceServiceCapabilities()
{
    return "<tds:Capabilities><tds:Network/><tds:Security UsernameToken=\"true\" HttpDigest=\"true\"/>"
           "<tds:System/></tds:Capabilities>";
}

std::string GetServices(const std::string& ip, bool includeCapabilities)
{
    std::string xaddr = "http://" + ip + ":" + std::to_string(OnvifPort) + "/onvif/device_service";

    return
        "<tds:GetServicesResponse>"
        "<tds:Service>"
        "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
        "<tds:XAddr>" + XmlEscape(xaddr) + "</tds:XAddr>"
        + (includeCapabilities ? "<tds:Capabilities>" + DeviceServiceCapabilities() + "</tds:Capabilities>" : "") +
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version>"
        "</tds:Service>"
        "<tds:Service>"
        "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
        "<tds:XAddr>" + XmlEscape(xaddr) + "</tds:XAddr>"
        + (includeCapabilities ? "<tds:Capabilities>" + MediaServiceCapabilities() + "</tds:Capabilities>" : "") +
        "<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version>"
        "</tds:Service>"
        "</tds:GetServicesResponse>";
}

std::string GetDeviceInformation()
{
    return
        "<tds:GetDeviceInformationResponse>"
        "<tds:Manufacturer>" + XmlEscape(Manufacturer) + "</tds:Manufacturer>"
        "<tds:Model>" + XmlEscape(Model) + "</tds:Model>"
        "<tds:FirmwareVersion>1.0</tds:FirmwareVersion>"
        "<tds:SerialNumber>" + XmlEscape(Serial) + "</tds:SerialNumber>"
        "<tds:HardwareId>PC-SCREEN</tds:HardwareId>"
        "</tds:GetDeviceInformationResponse>";
}

std::string GetCapabilities(const std::string& ip)
{
    std::string xaddr = "http://" + ip + ":" + std::to_string(OnvifPort) + "/onvif/device_service";

    return
        "<tds:GetCapabilitiesResponse>"
        "<tds:Capabilities>"
        "<tt:Device><tt:XAddr>" + XmlEscape(xaddr) + "</tt:XAddr></tt:Device>"
        "<tt:Media>"
        "<tt:XAddr>" + XmlEscape(xaddr) + "</tt:XAddr>"
        "<tt:StreamingCapabilities>"
        "<tt:RTPMulticast>false</tt:RTPMulticast>"
        "<tt:RTP_TCP>true</tt:RTP_TCP>"
        "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
        "</tt:StreamingCapabilities>"
        "</tt:Media>"
        "</tds:Capabilities>"
        "</tds:GetCapabilitiesResponse>";
}

std::string GetProfileXml(unsigned streamIndex, const std::string& element)
{
    const auto settings = streamIndex ? MakeSubStreamSettings(OnvifOverlaySettings) : OnvifOverlaySettings;
    const std::string width = std::to_string(settings.outputWidth);
    const std::string height = std::to_string(settings.outputHeight);
    const std::string fps = std::to_string(settings.frameRate);
    const std::string bitrate = std::to_string(settings.bitrateKbps);
    const std::string gop = std::to_string(settings.gopSize);
    const std::string index = std::to_string(streamIndex + 1);
    const std::string name = XmlEscape(CameraName + (streamIndex ? " (Substream)" : ""));
    return
        "<trt:" + element + " token=\"Profile_" + index + "\" fixed=\"true\">"
        "<tt:Name>" + name + "</tt:Name>"

        "<tt:VideoSourceConfiguration token=\"VideoSourceConfig_1\">"
        "<tt:Name>" + XmlEscape(CameraName) + "</tt:Name>"
        "<tt:UseCount>" + (OnvifOverlaySettings.subStreamEnabled ? "2" : "1") + "</tt:UseCount>"
        "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
        "<tt:Bounds x=\"0\" y=\"0\" width=\"" + std::to_string(VideoWidth) + "\" height=\"" + std::to_string(VideoHeight) + "\"/>"
        "</tt:VideoSourceConfiguration>"

        "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfig_" + index + "\">"
        "<tt:Name>" + name + "</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:Encoding>H264</tt:Encoding>"
        "<tt:Resolution><tt:Width>" + width + "</tt:Width><tt:Height>" + height + "</tt:Height></tt:Resolution>"
        "<tt:Quality>5</tt:Quality>"
        "<tt:RateControl>"
        "<tt:FrameRateLimit>" + fps + "</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>" + bitrate + "</tt:BitrateLimit>"
        "</tt:RateControl>"
        "<tt:H264><tt:GovLength>" + gop + "</tt:GovLength><tt:H264Profile>Baseline</tt:H264Profile></tt:H264>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        "</tt:VideoEncoderConfiguration>"

        "</trt:" + element + ">";
}

std::string GetProfiles(bool single = false, unsigned streamIndex = 0)
{
    const std::string operation = single ? "GetProfile" : "GetProfiles";
    std::string body = GetProfileXml(single ? streamIndex : 0, single ? "Profile" : "Profiles");
    if (!single && OnvifOverlaySettings.subStreamEnabled) body += GetProfileXml(1, "Profiles");
    return "<trt:" + operation + "Response>" + body + "</trt:" + operation + "Response>";
}

std::string GetStreamUri(const std::string& rtspUrl)
{
    return
        "<trt:GetStreamUriResponse>"
        "<trt:MediaUri>"
        "<tt:Uri>" + XmlEscape(rtspUrl) + "</tt:Uri>"
        "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
        "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
        "<tt:Timeout>PT60S</tt:Timeout>"
        "</trt:MediaUri>"
        "</trt:GetStreamUriResponse>";
}

std::string GetScopes()
{
    std::string name = UrlEncode(CameraName);

    return
        "<tds:GetScopesResponse>"
        "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef>"
        "<tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef>"
        "<tt:ScopeItem>onvif://www.onvif.org/name/" + name + "</tt:ScopeItem></tds:Scopes>"
        "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef>"
        "<tt:ScopeItem>onvif://www.onvif.org/hardware/Screen2NVR</tt:ScopeItem></tds:Scopes>"
        "</tds:GetScopesResponse>";
}

std::string GetHostname()
{
    return
        "<tds:GetHostnameResponse>"
        "<tds:HostnameInformation>"
        "<tt:FromDHCP>false</tt:FromDHCP>"
        "<tt:Name>" + XmlEscape(CameraName) + "</tt:Name>"
        "</tds:HostnameInformation>"
        "</tds:GetHostnameResponse>";
}

std::string GetSystemDateAndTime()
{
    SYSTEMTIME st{};
    GetSystemTime(&st);

    std::ostringstream ss;
    ss
        << "<tds:GetSystemDateAndTimeResponse>"
        << "<tds:SystemDateAndTime>"
        << "<tt:DateTimeType>NTP</tt:DateTimeType>"
        << "<tt:DaylightSavings>false</tt:DaylightSavings>"
        << "<tt:UTCDateTime>"
        << "<tt:Time>"
        << "<tt:Hour>" << st.wHour << "</tt:Hour>"
        << "<tt:Minute>" << st.wMinute << "</tt:Minute>"
        << "<tt:Second>" << st.wSecond << "</tt:Second>"
        << "</tt:Time>"
        << "<tt:Date>"
        << "<tt:Year>" << st.wYear << "</tt:Year>"
        << "<tt:Month>" << st.wMonth << "</tt:Month>"
        << "<tt:Day>" << st.wDay << "</tt:Day>"
        << "</tt:Date>"
        << "</tt:UTCDateTime>"
        << "</tds:SystemDateAndTime>"
        << "</tds:GetSystemDateAndTimeResponse>";

    return ss.str();
}

std::string GetVideoSources()
{
    const std::string width = std::to_string(VideoWidth);
    const std::string height = std::to_string(VideoHeight);
    const std::string fps = std::to_string(VideoFrameRate);
    return
        "<trt:GetVideoSourcesResponse>"
        "<trt:VideoSources token=\"VideoSource_1\">"
        "<tt:Framerate>" + fps + "</tt:Framerate>"
        "<tt:Resolution><tt:Width>" + width + "</tt:Width><tt:Height>" + height + "</tt:Height></tt:Resolution>"
        "</trt:VideoSources>"
        "</trt:GetVideoSourcesResponse>";
}

std::string GetVideoSourceConfigurations(bool single = false)
{
    const std::string width = std::to_string(VideoWidth);
    const std::string height = std::to_string(VideoHeight);
    const std::string operation = single ? "GetVideoSourceConfiguration" : "GetVideoSourceConfigurations";
    const std::string element = single ? "Configuration" : "Configurations";
    return
        "<trt:" + operation + "Response>"
        "<trt:" + element + " token=\"VideoSourceConfig_1\">"
        "<tt:Name>" + XmlEscape(CameraName) + "</tt:Name>"
        "<tt:UseCount>" + (OnvifOverlaySettings.subStreamEnabled ? "2" : "1") + "</tt:UseCount>"
        "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
        "<tt:Bounds x=\"0\" y=\"0\" width=\"" + width + "\" height=\"" + height + "\"/>"
        "</trt:" + element + ">"
        "</trt:" + operation + "Response>";
}

std::string GetEncoderConfigurationXml(unsigned streamIndex, const std::string& element)
{
    const auto settings = streamIndex ? MakeSubStreamSettings(OnvifOverlaySettings) : OnvifOverlaySettings;
    const std::string width = std::to_string(settings.outputWidth);
    const std::string height = std::to_string(settings.outputHeight);
    const std::string fps = std::to_string(settings.frameRate);
    const std::string bitrate = std::to_string(settings.bitrateKbps);
    const std::string gop = std::to_string(settings.gopSize);
    return
        "<trt:" + element + " token=\"VideoEncoderConfig_" + std::to_string(streamIndex + 1) + "\">"
        "<tt:Name>" + XmlEscape(CameraName + (streamIndex ? " (Substream)" : "")) + "</tt:Name>"
        "<tt:UseCount>1</tt:UseCount>"
        "<tt:Encoding>H264</tt:Encoding>"
        "<tt:Resolution><tt:Width>" + width + "</tt:Width><tt:Height>" + height + "</tt:Height></tt:Resolution>"
        "<tt:Quality>5</tt:Quality>"
        "<tt:RateControl>"
        "<tt:FrameRateLimit>" + fps + "</tt:FrameRateLimit>"
        "<tt:EncodingInterval>1</tt:EncodingInterval>"
        "<tt:BitrateLimit>" + bitrate + "</tt:BitrateLimit>"
        "</tt:RateControl>"
        "<tt:H264><tt:GovLength>" + gop + "</tt:GovLength><tt:H264Profile>Baseline</tt:H264Profile></tt:H264>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        "</trt:" + element + ">";
}

std::string GetVideoEncoderConfigurations(bool single = false, unsigned streamIndex = 0, bool compatible = false)
{
    const std::string operation = compatible ? "GetCompatibleVideoEncoderConfigurations" :
        single ? "GetVideoEncoderConfiguration" : "GetVideoEncoderConfigurations";
    std::string body = GetEncoderConfigurationXml(single ? streamIndex : 0, single ? "Configuration" : "Configurations");
    if (!single && OnvifOverlaySettings.subStreamEnabled) body += GetEncoderConfigurationXml(1, "Configurations");
    return "<trt:" + operation + "Response>" + body + "</trt:" + operation + "Response>";
}

std::string GetVideoEncoderConfigurationOptions(unsigned streamIndex = 0)
{
    const auto settings = streamIndex ? MakeSubStreamSettings(OnvifOverlaySettings) : OnvifOverlaySettings;
    const std::string width = std::to_string(settings.outputWidth);
    const std::string height = std::to_string(settings.outputHeight);
    return
        "<trt:GetVideoEncoderConfigurationOptionsResponse>"
        "<trt:Options>"
        "<tt:H264>"
        "<tt:ResolutionsAvailable><tt:Width>" + width + "</tt:Width><tt:Height>" + height + "</tt:Height></tt:ResolutionsAvailable>"
        "<tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>600</tt:Max></tt:GovLengthRange>"
        "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>" + std::to_string(settings.frameRate) + "</tt:Max></tt:FrameRateRange>"
        "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>"
        "<tt:H264ProfilesSupported>Baseline</tt:H264ProfilesSupported>"
        "<tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported>"
        "</tt:H264>"
        "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>10</tt:Max></tt:QualityRange>"
        "</trt:Options>"
        "</trt:GetVideoEncoderConfigurationOptionsResponse>";
}

namespace
{
constexpr char kDeviceNamespace[] = "http://www.onvif.org/ver10/device/wsdl";
constexpr char kMediaNamespace[] = "http://www.onvif.org/ver10/media/wsdl";
constexpr char kNameOsdToken[] = "OSD_CameraName";

struct OnvifRequest
{
    std::string operation, service;
    std::map<std::string, std::string> parameters;
    std::string Parameter(const char* name) const
    {
        const auto found = parameters.find(name);
        return found == parameters.end() ? std::string() : Trim(found->second);
    }
};

bool ParseOnvifRequest(const std::string& xml, OnvifRequest& request)
{
    // Dispatch by the SOAP Body element, not by substrings in headers, tokens or text.
    if (xml.empty() || xml.size() > 64 * 1024) return false;
    using Microsoft::WRL::ComPtr;
    ComPtr<IStream> stream;
    ULONG written = 0;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), &written)) || written != xml.size() ||
        FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return false;
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 24)) || FAILED(reader->SetInput(stream.Get()))) return false;
    std::wstring soapNamespace;
    bool inBody = false;
    unsigned bodyCount = 0, operationCount = 0;
    std::string field;
    XmlNodeType node{};
    HRESULT hr;
    while ((hr = reader->Read(&node)) == S_OK)
    {
        UINT depth = 0;
        if (FAILED(reader->GetDepth(&depth))) return false;
        if (node == XmlNodeType_Element)
        {
            const wchar_t* local = nullptr; const wchar_t* ns = nullptr;
            if (FAILED(reader->GetLocalName(&local, nullptr)) || !local ||
                FAILED(reader->GetNamespaceUri(&ns, nullptr)) || !ns) return false;
            const std::wstring name(local), uri(ns);
            if (depth == 0)
            {
                if (name != L"Envelope" || (uri != L"http://www.w3.org/2003/05/soap-envelope" &&
                    uri != L"http://schemas.xmlsoap.org/soap/envelope/")) return false;
                soapNamespace = uri;
            }
            if (depth == 1)
            {
                inBody = name == L"Body" && uri == soapNamespace;
                if (inBody && ++bodyCount != 1) return false;
            }
            if (inBody && depth == 2)
            {
                if (++operationCount != 1) return false;
                request.operation = WideToUtf8String(name);
                request.service = WideToUtf8String(uri);
            }
            if (inBody && depth == 3 && WideToUtf8String(uri) == request.service)
            {
                const std::string key = WideToUtf8String(name);
                // Only flat parameters used by the read operations are interpreted.
                if (key == "ConfigurationToken" || key == "ProfileToken" || key == "OSDToken" || key == "IncludeCapability")
                {
                    if (!request.parameters.emplace(key, "").second) return false;
                    field = reader->IsEmptyElement() ? std::string() : key;
                }
            }
            else if (!field.empty() && depth > 3) return false;
        }
        else if (node == XmlNodeType_EndElement)
        {
            if (depth == 3) field.clear();
            if (depth == 1) inBody = false;
        }
        else if (!field.empty() && (node == XmlNodeType_Text || node == XmlNodeType_CDATA || node == XmlNodeType_Whitespace))
        {
            const wchar_t* value = nullptr; UINT length = 0;
            if (reader->GetValue(&value, &length) != S_OK || !value) return false;
            auto& parameter = request.parameters[field];
            parameter += WideToUtf8String(std::wstring(value, length));
            if (parameter.size() > 4096) return false;
        }
    }
    return hr == S_FALSE && bodyCount == 1 && operationCount == 1;
}

std::string SoapFault(const char* subcode, const char* reason)
{
    return Soap(std::string("<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:") +
        subcode + "</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">" +
        XmlEscape(reason) + "</s:Text></s:Reason></s:Fault>");
}

std::string CameraNameOsd(const char* element)
{
    const auto& settings = OnvifOverlaySettings;
    const bool visible = settings.overlayTemplate.find(L"{camera}") != std::wstring::npos;
    const bool right = settings.timestampPosition == 1 || settings.timestampPosition == 3;
    const bool bottom = settings.timestampPosition >= 2;
    const double x = (right ? 1.0 : -1.0) * (1.0 - 2.0 * settings.timestampMarginX / VideoWidth);
    const double y = (bottom ? -1.0 : 1.0) * (1.0 - 2.0 * settings.timestampMarginY / VideoHeight);
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << "<trt:" << element << " token=\"" << kNameOsdToken << "\">"
        << "<tt:VideoSourceConfigurationToken>VideoSourceConfig_1</tt:VideoSourceConfigurationToken>"
        << "<tt:Type>Text</tt:Type><tt:Position><tt:Type>Custom</tt:Type>"
        << "<tt:Pos x=\"" << std::clamp(x, -1.0, 1.0) << "\" y=\"" << std::clamp(y, -1.0, 1.0) << "\"/></tt:Position>"
        << "<tt:TextString><tt:Type>Plain</tt:Type><tt:FontSize>" << (settings.timestampFontSize * 3 / 4)
        << "</tt:FontSize><tt:FontColor Transparent=\"" << (visible ? 0 : 255) << "\">"
        << "<tt:Color X=\"1\" Y=\"1\" Z=\"1\" Colorspace=\"http://www.onvif.org/ver10/colorspace/RGB\"/></tt:FontColor>"
        << "<tt:BackgroundColor Transparent=\"" << (visible ? 255 - settings.timestampBackgroundOpacity * 255 / 100 : 255)
        << "\"><tt:Color X=\"0\" Y=\"0\" Z=\"0\" Colorspace=\"http://www.onvif.org/ver10/colorspace/RGB\"/></tt:BackgroundColor>"
        << "<tt:PlainText>" << XmlEscape(CameraName) << "</tt:PlainText></tt:TextString></trt:" << element << ">";
    return xml.str();
}

std::string GetOsdOptions()
{
    return "<trt:GetOSDOptionsResponse><trt:OSDOptions>"
        "<tt:MaximumNumberOfOSDs Total=\"1\" PlainText=\"1\"/><tt:Type>Text</tt:Type>"
        "<tt:PositionOption>Custom</tt:PositionOption><tt:TextOption><tt:Type>Plain</tt:Type>"
        "<tt:FontColor><tt:Transparent><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Transparent></tt:FontColor>"
        "<tt:BackgroundColor><tt:Transparent><tt:Min>0</tt:Min><tt:Max>255</tt:Max></tt:Transparent></tt:BackgroundColor>"
        "</tt:TextOption></trt:OSDOptions></trt:GetOSDOptionsResponse>";
}

void LogOnvifOperation(const OnvifRequest& request, bool supported)
{
    if (!EmbeddedLogger) return;
    const auto& name = request.operation;
    if (name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c)
        { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); })) return;
    // At most 32 short diagnostics per process. Never log SOAP bodies or credentials.
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(mutex);
    const std::string key = std::string(request.service == kDeviceNamespace ? "device/" :
                                       request.service == kMediaNamespace ? "media/" : "other/") + name;
    if (logged.size() < 32 && logged.insert(key).second)
        EmbeddedLogger("ONVIF " + name + (supported ? ": served" : ": unsupported (SOAP fault)"));
}
}

std::string BuildSoapResponse(const std::string& xml, const std::string& ip, const std::string& rtspUrl, bool& fault)
{
    std::lock_guard<std::mutex> settingsLock(OnvifSettingsMutex);
    fault = true;
    OnvifRequest request;
    if (!ParseOnvifRequest(xml, request)) return SoapFault("InvalidArgVal", "Invalid SOAP request");
    const std::string& operation = request.operation;
    const bool device = request.service == kDeviceNamespace, media = request.service == kMediaNamespace;
    const auto token = [&](const char* field, const char* expected, bool optional = false)
    { const auto value = request.Parameter(field); return value == expected || (optional && value.empty()); };
    const auto profileToken = request.Parameter("ProfileToken");
    const auto configurationToken = request.Parameter("ConfigurationToken");
    const bool validProfile = profileToken == "Profile_1" ||
                              (OnvifOverlaySettings.subStreamEnabled && profileToken == "Profile_2");
    const bool validEncoder = configurationToken == "VideoEncoderConfig_1" ||
                              (OnvifOverlaySettings.subStreamEnabled && configurationToken == "VideoEncoderConfig_2");
    const bool encoderOperation = operation == "GetVideoEncoderConfiguration" || operation == "GetVideoEncoderConfigurationOptions";
    const unsigned streamIndex = (profileToken == "Profile_2" ||
                                 (encoderOperation && configurationToken == "VideoEncoderConfig_2")) ? 1U : 0U;
    if ((media && (operation == "GetProfile" || operation == "GetCompatibleVideoEncoderConfigurations") && !validProfile) ||
        (media && operation == "GetStreamUri" && !profileToken.empty() && !validProfile) ||
        (media && operation == "GetVideoEncoderConfigurationOptions" &&
            ((!profileToken.empty() && !validProfile) || (!configurationToken.empty() && !validEncoder) ||
             (!profileToken.empty() && !configurationToken.empty() &&
              ((profileToken == "Profile_2") != (configurationToken == "VideoEncoderConfig_2"))))) ||
        (media && operation == "GetVideoSourceConfiguration" && !token("ConfigurationToken", "VideoSourceConfig_1")) ||
        (media && operation == "GetVideoEncoderConfiguration" && !validEncoder) ||
        (media && operation == "GetOSDs" && !token("ConfigurationToken", "VideoSourceConfig_1", true)) ||
        (media && operation == "GetOSDOptions" && !token("ConfigurationToken", "VideoSourceConfig_1")) ||
        (media && operation == "GetOSD" && !token("OSDToken", kNameOsdToken)))
        return SoapFault("InvalidArgVal", "Unknown configuration or profile token");
    std::string body;
    if (device && operation == "GetServices")
        body = GetServices(ip, request.Parameter("IncludeCapability") == "true" || request.Parameter("IncludeCapability") == "1");
    else if (device && operation == "GetDeviceInformation") body = GetDeviceInformation();
    else if (device && operation == "GetCapabilities") body = GetCapabilities(ip);
    else if (device && operation == "GetScopes") body = GetScopes();
    else if (device && operation == "GetHostname") body = GetHostname();
    else if (device && operation == "GetSystemDateAndTime") body = GetSystemDateAndTime();
    else if (device && operation == "GetServiceCapabilities")
        body = "<tds:GetServiceCapabilitiesResponse>" + DeviceServiceCapabilities() + "</tds:GetServiceCapabilitiesResponse>";
    else if (media && operation == "GetServiceCapabilities")
        body = "<trt:GetServiceCapabilitiesResponse>" + MediaServiceCapabilities() + "</trt:GetServiceCapabilitiesResponse>";
    else if (media && operation == "GetProfiles") body = GetProfiles();
    else if (media && operation == "GetProfile") body = GetProfiles(true, streamIndex);
    else if (media && operation == "GetStreamUri")
        body = GetStreamUri(streamIndex ? "rtsp://" + ip + ":" + std::to_string(RtspPort) + "/" +
                            WideToUtf8String(OnvifOverlaySettings.subRtspPath) : rtspUrl);
    else if (media && operation == "GetVideoSources") body = GetVideoSources();
    else if (media && operation == "GetVideoSourceConfigurations") body = GetVideoSourceConfigurations();
    else if (media && operation == "GetVideoSourceConfiguration") body = GetVideoSourceConfigurations(true);
    else if (media && operation == "GetVideoEncoderConfigurations") body = GetVideoEncoderConfigurations();
    else if (media && operation == "GetVideoEncoderConfiguration") body = GetVideoEncoderConfigurations(true, streamIndex);
    else if (media && operation == "GetCompatibleVideoEncoderConfigurations") body = GetVideoEncoderConfigurations(false, 0, true);
    else if (media && operation == "GetVideoEncoderConfigurationOptions") body = GetVideoEncoderConfigurationOptions(streamIndex);
    else if (media && operation == "GetOSDs") body = "<trt:GetOSDsResponse>" + CameraNameOsd("OSDs") + "</trt:GetOSDsResponse>";
    else if (media && operation == "GetOSD") body = "<trt:GetOSDResponse>" + CameraNameOsd("OSD") + "</trt:GetOSDResponse>";
    else if (media && operation == "GetOSDOptions") body = GetOsdOptions();
    LogOnvifOperation(request, !body.empty());
    if (body.empty()) return SoapFault("ActionNotSupported", "Operation not supported; configure Screen2NVR locally");
    fault = false;
    return Soap(body);
}

bool ReceiveHttpRequest(SOCKET client, std::string& request)
{
    constexpr size_t kMaximumRequestSize = 1024 * 1024;
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    request.clear();
    request.reserve(8192);

    size_t expectedSize = std::string::npos;
    while (request.size() < kMaximumRequestSize)
    {
        std::array<char, 8192> buffer{};
        const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) return false;
        request.append(buffer.data(), static_cast<size_t>(received));

        const size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) continue;
        if (expectedSize == std::string::npos)
        {
            std::string lowerHeaders = request.substr(0, headerEnd);
            std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            size_t contentLength = 0;
            const size_t field = lowerHeaders.find("\r\ncontent-length:");
            if (field != std::string::npos)
            {
                const size_t valueStart = lowerHeaders.find_first_not_of(" \t", field + 17);
                if (valueStart == std::string::npos) return false;
                const size_t valueEnd = lowerHeaders.find("\r\n", valueStart);
                try
                {
                    size_t consumed = 0;
                    const std::string value = Trim(lowerHeaders.substr(valueStart, valueEnd - valueStart));
                    if (value.empty() || value.size() > 7 || !std::all_of(value.begin(), value.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
                    contentLength = std::stoul(value, &consumed);
                    if (consumed != value.size()) return false;
                }
                catch (const std::exception&)
                {
                    return false;
                }
            }
            if (headerEnd + 4 > kMaximumRequestSize || contentLength > kMaximumRequestSize - headerEnd - 4) return false;
            expectedSize = headerEnd + 4 + contentLength;
        }
        if (request.size() >= expectedSize)
        {
            request.resize(expectedSize);
            return true;
        }
    }
    return false;
}

void HttpServer(const std::string& ip, const std::string& rtspUrl)
{
    Security::DigestAuthenticator digestAuthentication;
    Security::AuthenticationLogLimiter authenticationLog;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        ReportEmbeddedError("Embedded ONVIF HTTP socket failed: WSA " + std::to_string(WSAGetLastError()));
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)OnvifPort);

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR)
    {
        ReportEmbeddedError("Embedded ONVIF HTTP listen failed on port " + std::to_string(OnvifPort) +
                            ": WSA " + std::to_string(WSAGetLastError()));
        closesocket(server);
        return;
    }

    while (true)
    {
        sockaddr_in remote{}; int remoteLength = sizeof(remote);
        SOCKET client = accept(server, reinterpret_cast<sockaddr*>(&remote), &remoteLength);
        if (client == INVALID_SOCKET) continue;
        char remoteText[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &remote.sin_addr, remoteText, sizeof(remoteText));
        if (!Security::IpAllowed(remoteText, AllowedIpAddresses)) { closesocket(client); continue; }

        std::string request;
        if (!ReceiveHttpRequest(client, request))
        {
            closesocket(client);
            continue;
        }

        const size_t bodyStart = request.find("\r\n\r\n");
        const std::string xml = bodyStart == std::string::npos ? std::string() : request.substr(bodyStart + 4);
        bool authorized = !AuthenticationEnabled;
        std::string reason;
        if (AuthenticationEnabled)
        {
            bool httpPresent = false, tokenPresent = false;
            const std::string authorization = Security::Header(request, "authorization", &httpPresent);
            std::istringstream firstLine(request.substr(0, request.find("\r\n")));
            std::string method, uri, version;
            firstLine >> method >> uri >> version;
            const bool httpValid = httpPresent &&
                (Security::Basic(authorization, AuthenticationUser, AuthenticationPassword) ||
                 digestAuthentication.Validate(authorization, method, uri, AuthenticationUser,
                                               AuthenticationPassword, remoteText, reason));
            std::string tokenReason;
            const bool tokenValid = Security::OnvifToken(xml, AuthenticationUser, AuthenticationPassword,
                                                       &tokenPresent, &tokenReason);
            // Either mechanism is sufficient, but if both are supplied both must be valid.
            authorized = (httpPresent || tokenPresent) && (!httpPresent || httpValid) && (!tokenPresent || tokenValid);
            if (!httpPresent || httpValid) reason = tokenReason;
            if (!httpPresent && !tokenPresent)
            {
                OnvifRequest operation;
                // ONVIF PRE_AUTH discovery/time operations let an NVR obtain UTC time
                // before constructing a time-limited UsernameToken. Media stays protected.
                authorized = method == "POST" && ParseOnvifRequest(xml, operation) &&
                    operation.service == kDeviceNamespace &&
                    (operation.operation == "GetSystemDateAndTime" || operation.operation == "GetServices" ||
                     operation.operation == "GetCapabilities" || operation.operation == "GetServiceCapabilities" ||
                     operation.operation == "GetScopes" || operation.operation == "GetHostname");
            }
            if (!authorized && (httpPresent || tokenPresent) && EmbeddedLogger && authenticationLog.ShouldLog())
                EmbeddedLogger("ONVIF authentication rejected from " + std::string(remoteText) + ": " + reason);
        }
        bool soapFault = false;
        const std::string body = authorized ? BuildSoapResponse(xml, ip, rtspUrl, soapFault) : std::string();
        const std::string challenge = authorized ? std::string() : digestAuthentication.Challenge(remoteText,
            reason == "Digest nonce expired or unknown");
        std::ostringstream response;
        response << (!authorized ? (challenge.empty() ? "HTTP/1.1 503 Service Unavailable\r\n" : "HTTP/1.1 401 Unauthorized\r\n") :
                     soapFault ? "HTTP/1.1 500 Internal Server Error\r\n" : "HTTP/1.1 200 OK\r\n");
        if (!challenge.empty()) response << "WWW-Authenticate: " << challenge << "\r\n";
        response
            << "Content-Type: application/soap+xml; charset=utf-8\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;

        const std::string out = response.str();
        size_t sentTotal = 0;
        while (sentTotal < out.size())
        {
            const int sent = send(client, out.data() + sentTotal,
                                  static_cast<int>(out.size() - sentTotal), 0);
            if (sent <= 0) break;
            sentTotal += static_cast<size_t>(sent);
        }
        closesocket(client);
    }
}

std::string ExtractMessageId(const std::string& request)
{
    std::regex r("<[^>]*MessageID[^>]*>(.*?)</[^>]*MessageID>");
    std::smatch m;

    if (std::regex_search(request, m, r))
        return m[1].str();

    return "";
}

std::string DiscoveryResponse(const std::string& serviceUrl, const std::string& relatesTo)
{
    std::string safeName = UrlEncode(CameraName);

    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope "
        "xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>urn:uuid:" + RandomUuid().substr(9) + "</w:MessageID>"
        "<w:RelatesTo>" + XmlEscape(relatesTo) + "</w:RelatesTo>"
        "<w:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</w:To>"
        "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</w:Action>"
        "</e:Header>"
        "<e:Body>"
        "<d:ProbeMatches>"
        "<d:ProbeMatch>"
        "<w:EndpointReference><w:Address>" + XmlEscape(DeviceUuid) + "</w:Address></w:EndpointReference>"
        "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
        "<d:Scopes>"
        "onvif://www.onvif.org/type/video_encoder "
        "onvif://www.onvif.org/name/" + safeName + " "
        "onvif://www.onvif.org/hardware/Screen2NVR"
        "</d:Scopes>"
        "<d:XAddrs>" + XmlEscape(serviceUrl) + "</d:XAddrs>"
        "<d:MetadataVersion>1</d:MetadataVersion>"
        "</d:ProbeMatch>"
        "</d:ProbeMatches>"
        "</e:Body>"
        "</e:Envelope>";
}

void DiscoveryServer(const std::string& serviceUrl, uint16_t port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        ReportEmbeddedError("Embedded WS-Discovery socket failed: WSA " +
                            std::to_string(WSAGetLastError()));
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);

    if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
    {
        ReportEmbeddedError("Embedded WS-Discovery bind failed: WSA " +
                            std::to_string(WSAGetLastError()));
        closesocket(s);
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    const std::string localIp = GetLocalIp();
    mreq.imr_interface.s_addr = inet_addr(localIp.c_str());

    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) == SOCKET_ERROR)
    {
        ReportEmbeddedError("Embedded WS-Discovery multicast join failed on " + localIp +
                            ": WSA " + std::to_string(WSAGetLastError()));
        closesocket(s);
        return;
    }

    while (true)
    {
        std::vector<char> buffer(65536);
        sockaddr_in from{};
        int fromLen = sizeof(from);

        int len = recvfrom(s, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                           (sockaddr*)&from, &fromLen);
        if (len <= 0) continue;

        std::string request(buffer.data(), static_cast<size_t>(len));

        if (request.find("Probe") == std::string::npos)
            continue;
        char remoteText[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, remoteText, sizeof(remoteText));
        if (!Security::IpAllowed(remoteText, AllowedIpAddresses)) continue;

        std::string relatesTo = ExtractMessageId(request);
        std::string response = DiscoveryResponse(serviceUrl, relatesTo);

        sendto(s, response.c_str(), (int)response.size(), 0, (sockaddr*)&from, fromLen);
    }
}

void UpdateEmbeddedOnvifLiveSettings(const AppSettings& settings)
{
    std::lock_guard<std::mutex> lock(OnvifSettingsMutex);
    CopyLiveSettings(OnvifOverlaySettings, settings);
}

EmbeddedOnvifSettings StartEmbeddedOnvifServer(
    const AppSettings& settings,
    const std::function<void(const std::string&)>& logger, uint16_t discoveryPort)
{
    EmbeddedLogger = logger;
    OnvifOverlaySettings = settings;
    APP_DIR = WideToUtf8String(GetScreen2NvrDataDirectory());
    UUID_PATH = APP_DIR + "\\onvif.uuid";
    CameraName = WideToUtf8String(settings.cameraName);
    const auto& board = GetMotherboardIdentity();
    Manufacturer = WideToUtf8String(board.manufacturer);
    Model = WideToUtf8String(board.model);
    Serial = WideToUtf8String(board.serial);
    OnvifPort = settings.onvifPort;
    RtspPort = settings.rtspPort;
    RtspPath = WideToUtf8String(settings.rtspPath);
    VideoWidth = settings.outputWidth;
    VideoHeight = settings.outputHeight;
    VideoFrameRate = settings.frameRate;
    VideoBitrateKbps = settings.bitrateKbps;
    VideoGop = settings.gopSize;
    AuthenticationEnabled = settings.authenticationEnabled;
    AuthenticationUser = WideToUtf8String(settings.userName);
    AuthenticationPassword = WideToUtf8String(settings.password);
    AllowedIpAddresses = WideToUtf8String(settings.allowedIpAddresses);
    DeviceUuid = LoadOrCreateUuid();

    const std::string ip = GetLocalIp();
    const std::string serviceUrl =
        "http://" + ip + ":" + std::to_string(OnvifPort) + "/onvif/device_service";
    const std::string rtspUrl =
        "rtsp://" + ip + ":" + std::to_string(RtspPort) + "/" + RtspPath;

    std::cout << "Embedded ONVIF: " << serviceUrl << "\n";
    std::cout << "Advertised RTSP: " << rtspUrl << "\n";

    std::thread(DiscoveryServer, serviceUrl, discoveryPort).detach();
    std::thread([ip, rtspUrl]
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) { ReportEmbeddedError("ONVIF COM initialization failed"); return; }
        HttpServer(ip, rtspUrl);
        CoUninitialize();
    }).detach();
    return { static_cast<uint16_t>(RtspPort), RtspPath };
}

