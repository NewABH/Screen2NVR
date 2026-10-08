// Exercise the production encoder's output path without depending on a GPU driver.
#define SCREEN2NVR_ENCODER_TEST
#define wmain Screen2NvrEntryForTest
#include "../ScreenCapture.cpp"
#undef wmain
#include <wrl/implements.h>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TestTransform final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFTransform>
{
public:
    DWORD allocationFlags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
    unsigned changes = 1, outputCalls = 0, acceptedTypes = 0;
    bool incompatibleFirst = false, incompatibleOnly = false, currentOnly = false;
    bool rejectTypes = false, failOutput = false, needInput = false;
    DWORD lastCapacity = 0;
    bool aligned = false;
    ComPtr<IMFMediaType> current;
    ComPtr<IMFSample> lastSample;

    TestTransform()
    {
        CheckHr(MFCreateMediaType(&current), "Test media type");
        CheckHr(current->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Test major type");
        CheckHr(current->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Test subtype");
        const BYTE header[] = { 0, 0, 0, 1, 0x67, 0x42, 0, 0x28, 0, 0, 0, 1, 0x68, 0xCE };
        CheckHr(current->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header, sizeof(header)), "Test SPS/PPS");
    }

    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO* info) override
    {
        *info = { allocationFlags, acceptedTypes ? 3'000'123U : 4096U, acceptedTypes ? 64U : 16U };
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOutputAvailableType(DWORD, DWORD index, IMFMediaType** type) override
    {
        if (currentOnly) return E_NOTIMPL;
        if (index >= (incompatibleFirst ? 2U : 1U)) return MF_E_NO_MORE_TYPES;
        if (incompatibleOnly || (incompatibleFirst && index == 0))
        {
            ComPtr<IMFMediaType> bad;
            HRESULT hr = MFCreateMediaType(&bad);
            if (FAILED(hr)) return hr;
            bad->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            *type = bad.Detach();
            return S_OK;
        }
        return current.CopyTo(type); // Deliberately partial: no size/FPS/bitrate.
    }
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD, IMFMediaType** type) override
    {
        if (incompatibleOnly) return MF_E_TRANSFORM_TYPE_NOT_SET;
        return current.CopyTo(type);
    }
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD, IMFMediaType* type, DWORD) override
    {
        if (rejectTypes) return MF_E_INVALIDMEDIATYPE;
        UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
        if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)) ||
            width != kOutputWidth || height != kOutputHeight ||
            FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) ||
            numerator != kFrameRate || denominator != 1)
            return MF_E_INVALIDMEDIATYPE;
        current = type;
        ++acceptedTypes;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* output, DWORD* status) override
    {
        ++outputCalls;
        if (count != 1) return E_INVALIDARG;
        *status = 0;
        if (needInput) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        // Include owned samples/events even on failure to test cleanup on every path.
        MFCreateCollection(&output->pEvents);
        ComPtr<IMFSample> sample;
        if (output->pSample) sample = output->pSample;
        else
        {
            MFCreateSample(&sample);
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateMemoryBuffer(64, &buffer);
            sample->AddBuffer(buffer.Get());
            sample.CopyTo(&output->pSample);
        }
        lastSample = sample;
        if (changes)
        {
            --changes;
            output->dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
            return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        if (failOutput) return E_FAIL;
        ComPtr<IMFMediaBuffer> buffer;
        sample->GetBufferByIndex(0, &buffer);
        buffer->GetMaxLength(&lastCapacity);
        BYTE* bytes = nullptr;
        buffer->Lock(&bytes, nullptr, nullptr);
        aligned = (reinterpret_cast<uintptr_t>(bytes) & 63U) == 0;
        const BYTE frame[] = { 0, 0, 0, 1, 0x65, 0x88 };
        memcpy(bytes, frame, sizeof(frame));
        buffer->Unlock();
        buffer->SetCurrentLength(sizeof(frame));
        sample->SetSampleTime(123456);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetStreamLimits(DWORD*, DWORD*, DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD, MFT_INPUT_STREAM_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAttributes(IMFAttributes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputStreamAttributes(DWORD, IMFAttributes**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeleteInputStream(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddInputStreams(DWORD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputAvailableType(DWORD, DWORD, IMFMediaType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD, IMFMediaType*, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD, IMFMediaType**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetOutputStatus(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetOutputBounds(LONGLONG, LONGLONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessEvent(DWORD, IMFMediaEvent*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessMessage(MFT_MESSAGE_TYPE, ULONG_PTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD, IMFSample*, DWORD) override { return E_NOTIMPL; }
};

struct Result { unsigned frames = 0, headers = 0; bool failCallback = false; };
void Collect(void* context, const uint8_t* bytes, size_t size, LONGLONG time, bool header)
{
    auto& result = *static_cast<Result*>(context);
    if (result.failCallback) throw std::runtime_error("Test callback failure");
    Require(time == 123456, "Sample time must survive a type change");
    Require(size >= 6 && bytes[0] == 0 && bytes[3] == 1, "Valid Annex B output");
    if (header) ++result.headers;
    else ++result.frames;
}

struct H264EncoderTestAccess
{
    static void Attach(H264Encoder& encoder, IMFTransform* transform, Result& result)
    {
        encoder.transform_ = transform;
        encoder.outputType_ = encoder.CreateVideoType(MFVideoFormat_H264);
        encoder.callback_ = Collect;
        encoder.callbackContext_ = &result;
    }
    static HRESULT Output(H264Encoder& encoder) { return encoder.ProduceOutput(); }
};

ULONG References(IUnknown* object)
{
    const ULONG references = object->AddRef();
    object->Release();
    return references - 1;
}

void Tests()
{
    for (DWORD flags : { DWORD(0), DWORD(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES), DWORD(MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES) })
    {
        auto transform = Microsoft::WRL::Make<TestTransform>();
        transform->allocationFlags = flags;
        transform->incompatibleFirst = true;
        Result result;
        H264Encoder encoder;
        H264EncoderTestAccess::Attach(encoder, transform.Get(), result);
        Require(H264EncoderTestAccess::Output(encoder) == S_OK, "Stream change is recoverable");
        Require(transform->acceptedTypes == 1 && transform->outputCalls == 1, "One output call per async event");
        Require(result.frames == 0 && result.headers == 0, "Format change is not a video heartbeat");
        Require(References(transform->lastSample.Get()) == 1, "Old sample released on format change");
        Require(H264EncoderTestAccess::Output(encoder) == S_OK, "Output resumes after renegotiation");
        Require(result.frames == 1 && result.headers == 1, "Updated SPS/PPS accompany first real frame");
        if (flags == 0)
            Require(transform->lastCapacity >= 3'000'123 && transform->aligned, "New buffer size and alignment honored");
        else
            Require(References(transform->lastSample.Get()) == 1, "Driver-owned output sample released");
        for (int i = 0; i < 1000; ++i) H264EncoderTestAccess::Output(encoder);
        Require(result.frames == 1001 && result.headers == 1, "Steady output without repeated configuration");
        if (flags != 0) Require(References(transform->lastSample.Get()) == 1, "No per-frame sample leak");
        std::cout << "PASS: stream change, allocation mode " << flags << ", 1001 output frames\n";
    }
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        auto transform = Microsoft::WRL::Make<TestTransform>();
        Result result;
        if (scenario == 0) transform->currentOnly = true;
        if (scenario == 1) transform->incompatibleOnly = true;
        if (scenario == 2) transform->rejectTypes = true;
        if (scenario == 3) transform->changes = 9;
        if (scenario == 4) { transform->changes = 0; transform->failOutput = true; }
        if (scenario == 5) { transform->changes = 0; result.failCallback = true; }
        H264Encoder encoder;
        H264EncoderTestAccess::Attach(encoder, transform.Get(), result);
        bool failed = false;
        try
        {
            for (int i = 0; i < (scenario == 3 ? 9 : 1); ++i) H264EncoderTestAccess::Output(encoder);
        }
        catch (const std::exception&) { failed = true; }
        Require(failed == (scenario != 0), "Expected recovery or bounded diagnostic failure");
        Require(References(transform->lastSample.Get()) == 1, "Sample released on error/callback exception");
        std::cout << "PASS: negotiation/error scenario " << scenario << '\n';
    }
    auto transform = Microsoft::WRL::Make<TestTransform>();
    transform->needInput = true;
    Result result;
    H264Encoder encoder;
    H264EncoderTestAccess::Attach(encoder, transform.Get(), result);
    Require(H264EncoderTestAccess::Output(encoder) == MF_E_TRANSFORM_NEED_MORE_INPUT, "Synchronous drain ends");
    std::cout << "PASS: synchronous NEED_MORE_INPUT\n";
}
}

int main(int argc, char* argv[])
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    int status = 0;
    try
    {
        if (argc > 1)
        {
            // Real GPU/encoder test with in-memory settings and isolated diagnostic ports.
            // No production configuration, registry or autostart writes.
            AppSettings settings;
            Require(sscanf_s(argv[1], "%ux%u,%ux%u", &settings.outputWidth, &settings.outputHeight,
                             &settings.subWidth, &settings.subHeight) == 4, "Expected mainWxH,subWxH");
            settings.subStreamEnabled = true;
            settings.onvifPort = 18004; settings.rtspPort = 18558;
            settings.allowedIpAddresses = L"127.0.0.1";
            settings.adaptiveFrameRate = false; settings.adaptiveLoad = false;
            settings.overlayTemplate.clear(); settings.showCursor = false;
            settings.privacyMasks = L"0,0," + std::to_wstring(settings.outputWidth / 6) + L"," + std::to_wstring(settings.outputHeight / 6);
            const bool software = argc > 2 && std::string(argv[2]) == "--software";
            settings.encoderPreference = software ? 2 : 1;
            settings.allowSoftwareEncoder = software;
            RuntimeOptions options;
            options.pipelineTest = true; options.dualStreamTest = true; options.maxFrames = 16;
            WSADATA wsa{};
            Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock startup failed");
            ApplySettings(settings);
            Require(Run(options, settings) == 0, "Resolution pipeline failed");
            std::cout << "PASS: real " << (software ? "software" : "hardware") << " dual encoder " << argv[1] << '\n';
            WSACleanup();
        }
        else Tests();
    }
    catch (const HrError& error) { std::cerr << "FAIL: " << error.what() << " HRESULT=" << HresultText(error.hr) << '\n'; status = 1; }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; status = 1; }
    MFShutdown();
    CoUninitialize();
    return status;
}
