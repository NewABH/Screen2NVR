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

// Independent Windows decoder verifies real encoded pictures, not merely RTP/NAL headers.
// Only synthetic GPU pixels are used; no desktop recording or deployed settings are touched.
class DecoderCheck
{
public:
    DecoderCheck()
    {
        MFT_REGISTER_TYPE_INFO inputInfo{ MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** activations = nullptr; UINT32 count = 0;
        CheckHr(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &inputInfo, nullptr, &activations, &count), "Find Windows H.264 decoder");
        HRESULT activation = count ? activations[0]->ActivateObject(IID_PPV_ARGS(&decoder_)) : E_NOINTERFACE;
        for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
        CoTaskMemFree(activations);
        CheckHr(activation, "Create Windows H.264 decoder");
        ComPtr<IMFMediaType> input;
        CheckHr(MFCreateMediaType(&input), "Create decoder input type");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, 1920, 1080);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 12, 1);
        CheckHr(decoder_->SetInputType(0, input.Get(), 0), "Set decoder H.264 input");
        SelectOutput();
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    static void Encoded(void* context, const uint8_t* bytes, size_t size,
                        LONGLONG time, bool header)
    {
        auto& self = *static_cast<DecoderCheck*>(context);
        if (header) { self.header_.assign(bytes, bytes + size); return; }
        ++self.encodedFrames;
        std::vector<uint8_t> accessUnit = self.header_;
        self.header_.clear();
        accessUnit.insert(accessUnit.end(), bytes, bytes + size);
        ComPtr<IMFMediaBuffer> buffer;
        CheckHr(MFCreateMemoryBuffer(static_cast<DWORD>(accessUnit.size()), &buffer), "Create decoder input buffer");
        BYTE* data = nullptr;
        CheckHr(buffer->Lock(&data, nullptr, nullptr), "Lock decoder input");
        memcpy(data, accessUnit.data(), accessUnit.size());
        buffer->Unlock(); buffer->SetCurrentLength(static_cast<DWORD>(accessUnit.size()));
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample); sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(time); sample->SetSampleDuration(10'000'000 / 12);
        CheckHr(self.decoder_->ProcessInput(0, sample.Get(), 0), "Decode actual encoder output");
        self.Drain();
    }

    void Finish()
    {
        CheckHr(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "Drain H.264 decoder");
        Drain();
        Require(encodedFrames == 48 && decodedFrames == encodedFrames, "Real decoder lost encoded pictures");
        Require(changedFrames > 24 && nonGrayFrames == decodedFrames, "Decoded pictures are gray or frozen");
        std::cout << "PASS: independently decoded " << decodedFrames << " synthetic H.264 pictures; "
                  << changedFrames << " changing pictures, no gray/frozen frames\n";
    }
    unsigned encodedFrames = 0, decodedFrames = 0, nonGrayFrames = 0, changedFrames = 0;
private:
    void SelectOutput()
    {
        for (DWORD index = 0; ; ++index)
        {
            ComPtr<IMFMediaType> type;
            CheckHr(decoder_->GetOutputAvailableType(0, index, &type), "Enumerate decoder output");
            GUID subtype{}; type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            CheckHr(decoder_->SetOutputType(0, type.Get(), 0), "Set decoder NV12 output");
            return;
        }
    }
    void Drain()
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            CheckHr(decoder_->GetOutputStreamInfo(0, &info), "Read decoder buffer size");
            ComPtr<IMFSample> sample;
            if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
            {
                ComPtr<IMFMediaBuffer> buffer;
                CheckHr(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 1920 * 1088 * 3 / 2),
                            info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer), "Allocate decoder output");
                MFCreateSample(&sample); sample->AddBuffer(buffer.Get());
            }
            MFT_OUTPUT_DATA_BUFFER output{ 0, sample.Get(), 0, nullptr };
            DWORD flags = 0;
            const HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &flags);
            ComPtr<IMFCollection> events; events.Attach(output.pEvents);
            ComPtr<IMFSample> owned;
            if (output.pSample != sample.Get()) owned.Attach(output.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { SelectOutput(); continue; }
            CheckHr(hr, "Read decoded H.264 picture");
            Require(output.pSample != nullptr, "Decoder returned no picture");
            ComPtr<IMFMediaBuffer> buffer;
            CheckHr(output.pSample->ConvertToContiguousBuffer(&buffer), "Read decoded NV12 pixels");
            BYTE* pixels = nullptr; DWORD length = 0;
            CheckHr(buffer->Lock(&pixels, nullptr, &length), "Lock decoded picture");
            unsigned low = 255, high = 0; uint64_t sum = 0;
            for (DWORD i = 0; i < std::min<DWORD>(length, 1920 * 1080); i += 64)
            { low = std::min(low, unsigned(pixels[i])); high = std::max(high, unsigned(pixels[i])); sum += pixels[i]; }
            buffer->Unlock();
            if (high - low > 15) ++nonGrayFrames;
            if (decodedFrames && sum != previousSum_) ++changedFrames;
            previousSum_ = sum; ++decodedFrames;
        }
    }
    ComPtr<IMFTransform> decoder_;
    std::vector<uint8_t> header_;
    uint64_t previousSum_ = 0;
};

void RealDecodeCheck(bool software)
{
    AppSettings settings;
    settings.overlayTemplate.clear(); settings.showCursor = false;
    settings.privacyMasks = L"0,0,320,180";
    settings.encoderPreference = software ? 2 : 1; settings.allowSoftwareEncoder = software;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    CheckHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Create decode-check GPU device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1920; desc.Height = 1080; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source; ComPtr<ID3D11RenderTargetView> view;
    CheckHr(device->CreateTexture2D(&desc, nullptr, &source), "Create synthetic decode-check source");
    CheckHr(device->CreateRenderTargetView(source.Get(), nullptr, &view), "Create synthetic source view");
    GpuProcessor processor;
    RECT desktop{ 0, 0, 1920, 1080 };
    processor.Initialize(device.Get(), context.Get(), desc, DXGI_MODE_ROTATION_IDENTITY, settings, desktop);
    DecoderCheck check;
    H264Encoder encoder;
    InitializeH264Encoder(encoder, device.Get(), DecoderCheck::Encoded, &check, settings);
    for (unsigned frame = 0; frame < 48; ++frame)
    {
        const float phase = (frame % 12) / 12.0f;
        const float color[] = { 0.1f + phase * 0.5f, 0.15f, 0.45f - phase * 0.2f, 1.0f };
        context->ClearRenderTargetView(view.Get(), color);
        ComPtr<ID3D11Texture2D> input; UINT subresource = 0;
        auto sample = encoder.AllocateInput(&input, &subresource);
        processor.Process(source.Get(), input.Get(), subresource);
        encoder.Submit(sample.Get(), LONGLONG(frame) * (10'000'000 / 12));
    }
    encoder.Finalize(); check.Finish();
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
        if (argc > 1 && std::string(argv[1]) == "--decode")
            RealDecodeCheck(argc > 2 && std::string(argv[2]) == "--software");
        else if (argc > 1)
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
