#include "WasapiStreams.h"

#include <audioclientactivationparams.h>
#include <mmreg.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstring>

#include "util/Log.h"

namespace dbdeck::win {

using namespace Microsoft::WRL;

const char* streamStatusName(StreamStatus s) {
    switch (s) {
        case StreamStatus::Running: return "running";
        case StreamStatus::Lost: return "lost";
        case StreamStatus::Failed: return "failed";
        case StreamStatus::Stopped: return "stopped";
    }
    return "stopped";
}

namespace {

constexpr HRESULT kDeviceInvalidated = static_cast<HRESULT>(0x88890004);  // AUDCLNT_E_DEVICE_INVALIDATED
constexpr HRESULT kResourcesInvalidated = static_cast<HRESULT>(0x88890026);

bool isLost(HRESULT hr) { return hr == kDeviceInvalidated || hr == kResourcesInvalidated; }

WAVEFORMATEX engineFloatFormat() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    f.nChannels = 2;
    f.nSamplesPerSec = static_cast<DWORD>(kEngineSampleRate);
    f.wBitsPerSample = 32;
    f.nBlockAlign = 8;
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

class ActivationHandler
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(done_.get());
        return S_OK;
    }
    HANDLE event() const { return done_.get(); }

private:
    UniqueHandle done_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Capture

bool CaptureStream::initClient(ComPtr<IAudioClient> client, const WAVEFORMATEX* fmt, DWORD flags,
                               const BufferSettings& buf, std::string& error) {
    client_ = std::move(client);
    format_ = parseWaveFormat(fmt);
    if (!format_.supported()) {
        error = "unsupported capture format";
        return false;
    }
    HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     buf.captureBuffer(), 0, fmt, nullptr);
    if (FAILED(hr)) {
        error = "IAudioClient::Initialize (capture): " + hrToString(hr);
        return false;
    }
    event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(hr = client_->SetEventHandle(event_.get()))) {
        error = "SetEventHandle: " + hrToString(hr);
        return false;
    }
    if (FAILED(hr = client_->GetService(IID_PPV_ARGS(&capture_)))) {
        error = "IAudioCaptureClient: " + hrToString(hr);
        return false;
    }
    UINT32 frames = 0;
    client_->GetBufferSize(&frames);
    bufferMs_ = frames * 1000.0 / format_.sampleRate;
    scratch_.assign(static_cast<size_t>(std::max<UINT32>(frames, 4096)) * 2, 0.0f);
    const size_t target = static_cast<size_t>(buf.inputTargetMs() * format_.sampleRate / 1000.0);
    stream_ = std::make_shared<AdaptiveStream>(format_.sampleRate, kEngineSampleRate, std::max<size_t>(target, 64));
    if (FAILED(hr = client_->Start())) {
        error = "IAudioClient::Start (capture): " + hrToString(hr);
        return false;
    }
    status_ = StreamStatus::Running;
    thread_ = std::thread([this] { run(); });
    return true;
}

std::shared_ptr<CaptureStream> CaptureStream::openDevice(DeviceManager& dm, const std::string& deviceId,
                                                         bool loopback, const BufferSettings& buf,
                                                         std::string& error) {
    std::shared_ptr<CaptureStream> s(new CaptureStream());
    s->kind_ = loopback ? Kind::Loopback : Kind::Device;
    s->deviceId_ = deviceId;
    ComPtr<IMMDevice> dev = dm.device(deviceId);
    if (!dev) {
        error = "device not found";
        return nullptr;
    }
    DWORD state = 0;
    dev->GetState(&state);
    if (state != DEVICE_STATE_ACTIVE) {
        error = "device not active";
        return nullptr;
    }
    ComPtr<IAudioClient> client;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
        error = "Activate IAudioClient: " + hrToString(hr);
        return nullptr;
    }
    CoTaskPtr<WAVEFORMATEX> mix;
    if (FAILED(hr = client->GetMixFormat(mix.put()))) {
        error = "GetMixFormat: " + hrToString(hr);
        return nullptr;
    }
    if (!s->initClient(client, mix.get(), loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, buf, error)) return nullptr;
    return s;
}

std::shared_ptr<CaptureStream> CaptureStream::openProcess(uint32_t pid, const BufferSettings& buf,
                                                          std::string& error) {
    std::shared_ptr<CaptureStream> s(new CaptureStream());
    s->kind_ = Kind::Process;
    s->pid_ = pid;

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = Make<ActivationHandler>();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                             handler.Get(), &op);
    if (FAILED(hr)) {
        error = "ActivateAudioInterfaceAsync: " + hrToString(hr);
        return nullptr;
    }
    if (WaitForSingleObject(handler->event(), 3000) != WAIT_OBJECT_0) {
        error = "process loopback activation timed out";
        return nullptr;
    }
    HRESULT actHr = E_FAIL;
    ComPtr<IUnknown> unk;
    if (FAILED(hr = op->GetActivateResult(&actHr, &unk)) || FAILED(actHr)) {
        error = "process loopback activation failed: " + hrToString(FAILED(hr) ? hr : actHr);
        return nullptr;
    }
    ComPtr<IAudioClient> client;
    if (FAILED(hr = unk.As(&client))) {
        error = "IAudioClient: " + hrToString(hr);
        return nullptr;
    }
    // The process-loopback virtual device has no mix format; it converts to
    // whatever PCM format we request. Use the engine format directly.
    const WAVEFORMATEX fmt = engineFloatFormat();
    if (!s->initClient(client, &fmt, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, buf, error))
        return nullptr;
    return s;
}

CaptureStream::~CaptureStream() { stop(); }

void CaptureStream::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    if (status_ == StreamStatus::Running) status_ = StreamStatus::Stopped;
}

void CaptureStream::run() {
    ComApartment com;
    HANDLE mmcss = enterRealtimeAudioThread();
    const size_t maxFrames = scratch_.size() / 2;
    while (!stop_) {
        // Short timeout: loopback streams may not signal while the source is silent.
        WaitForSingleObject(event_.get(), 10);
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) {
                status_ = isLost(hr) ? StreamStatus::Lost : StreamStatus::Failed;
                error_ = hrToString(hr);
                leaveRealtimeAudioThread(mmcss);
                return;
            }
            if (packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                status_ = isLost(hr) ? StreamStatus::Lost : StreamStatus::Failed;
                error_ = hrToString(hr);
                leaveRealtimeAudioThread(mmcss);
                return;
            }
            size_t done = 0;
            while (done < frames) {
                const size_t n = std::min<size_t>(frames - done, maxFrames);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(scratch_.data(), 0, n * 2 * sizeof(float));
                else deviceToStereo(data + done * format_.blockAlign(), n, format_, scratch_.data());
                stream_->write(scratch_.data(), n);
                done += n;
            }
            capture_->ReleaseBuffer(frames);
        }
    }
    leaveRealtimeAudioThread(mmcss);
}

// ---------------------------------------------------------------------------
// Render

std::unique_ptr<RenderStream> RenderStream::open(DeviceManager& dm, const std::string& deviceId,
                                                 const BufferSettings& buf, std::string& error) {
    std::unique_ptr<RenderStream> r(new RenderStream());
    r->deviceId_ = deviceId;
    ComPtr<IMMDevice> dev = dm.device(deviceId);
    if (!dev) {
        error = "device not found";
        return nullptr;
    }
    DWORD state = 0;
    dev->GetState(&state);
    if (state != DEVICE_STATE_ACTIVE) {
        error = "device not active";
        return nullptr;
    }
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(r->client_.GetAddressOf()));
    if (FAILED(hr)) {
        error = "Activate IAudioClient: " + hrToString(hr);
        return nullptr;
    }
    CoTaskPtr<WAVEFORMATEX> mix;
    if (FAILED(hr = r->client_->GetMixFormat(mix.put()))) {
        error = "GetMixFormat: " + hrToString(hr);
        return nullptr;
    }
    DeviceFormat mixFmt = parseWaveFormat(mix.get());
    bool initialized = false;

    if (mixFmt.supported() && mixFmt.sampleRate == static_cast<uint32_t>(kEngineSampleRate)) {
        // Native 48 kHz: use the mix format and convert channels/sample type ourselves.
        if (buf.lowLatency()) {
            ComPtr<IAudioClient3> c3;
            UINT32 def = 0, fund = 0, mn = 0, mx = 0;
            if (SUCCEEDED(r->client_.As(&c3)) &&
                SUCCEEDED(c3->GetSharedModeEnginePeriod(mix.get(), &def, &fund, &mn, &mx)) &&
                SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, mn, mix.get(), nullptr))) {
                initialized = true;
                r->periodFrames_ = mn;
            }
        }
        if (!initialized) {
            hr = r->client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        buf.renderBuffer() ? buf.renderBuffer() : 100000, 0, mix.get(), nullptr);
            if (FAILED(hr)) {
                error = "IAudioClient::Initialize (render): " + hrToString(hr);
                return nullptr;
            }
            initialized = true;
        }
        r->format_ = mixFmt;
    } else {
        // Different device rate: let the Windows audio engine convert from 48 kHz float stereo.
        const WAVEFORMATEX f = engineFloatFormat();
        hr = r->client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    buf.renderBuffer() ? buf.renderBuffer() : 100000, 0, &f, nullptr);
        if (FAILED(hr)) {
            error = "IAudioClient::Initialize (render, autoconvert): " + hrToString(hr);
            return nullptr;
        }
        r->format_ = parseWaveFormat(&f);
    }

    r->event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(hr = r->client_->SetEventHandle(r->event_.get()))) {
        error = "SetEventHandle: " + hrToString(hr);
        return nullptr;
    }
    if (FAILED(hr = r->client_->GetService(IID_PPV_ARGS(&r->render_)))) {
        error = "IAudioRenderClient: " + hrToString(hr);
        return nullptr;
    }
    r->client_->GetBufferSize(&r->bufferFrames_);
    r->client_->GetStreamLatency(&r->streamLatency_);
    if (!r->periodFrames_) {
        REFERENCE_TIME def = 0, mn = 0;
        r->client_->GetDevicePeriod(&def, &mn);
        r->periodFrames_ = static_cast<uint32_t>(def * kEngineSampleRate / 10000000.0);
    }
    r->scratch_.assign(static_cast<size_t>(r->bufferFrames_) * 2, 0.0f);
    return r;
}

double RenderStream::latencyMs() const {
    return bufferFrames_ * 1000.0 / kEngineSampleRate + streamLatency_ / 10000.0;
}

RenderStream::~RenderStream() { stop(); }

void RenderStream::startMaster(Mixer* mixer) {
    mixer_ = mixer;
    status_ = StreamStatus::Running;
    thread_ = std::thread([this] { run(); });
}

void RenderStream::startSlave(std::shared_ptr<AdaptiveStream> ring) {
    ring_ = std::move(ring);
    status_ = StreamStatus::Running;
    thread_ = std::thread([this] { run(); });
}

void RenderStream::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
    if (status_ == StreamStatus::Running) status_ = StreamStatus::Stopped;
}

void RenderStream::run() {
    ComApartment com;
    HANDLE mmcss = enterRealtimeAudioThread();
    const float fadeCoeff = smoothingCoeff(15.0f, kEngineSampleRate);
    fade_.resetTo(0.0f);
    fade_.setTarget(1.0f);  // fade in on (re)connect: no pop

    auto fail = [&](HRESULT hr) {
        status_ = isLost(hr) ? StreamStatus::Lost : StreamStatus::Failed;
        error_ = hrToString(hr);
    };

    // Prime with silence so the device starts cleanly.
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &data))) render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        fail(hr);
        leaveRealtimeAudioThread(mmcss);
        return;
    }

    while (!stop_) {
        const DWORD w = WaitForSingleObject(event_.get(), 200);
        if (stop_) break;
        if (w != WAIT_OBJECT_0) glitches_.fetch_add(1, std::memory_order_relaxed);  // device stalled
        UINT32 padding = 0;
        if (FAILED(hr = client_->GetCurrentPadding(&padding))) {
            fail(hr);
            break;
        }
        const UINT32 avail = bufferFrames_ - padding;
        if (avail == 0) continue;
        if (FAILED(hr = render_->GetBuffer(avail, &data))) {
            fail(hr);
            break;
        }
        float* buf = scratch_.data();
        if (mixer_) mixer_->process(buf, avail);
        else ring_->pull(buf, avail);
        if (!fade_.settled()) {
            for (UINT32 i = 0; i < avail; ++i) {
                const float g = fade_.next(fadeCoeff);
                buf[i * 2] *= g;
                buf[i * 2 + 1] *= g;
            }
        }
        stereoToDevice(buf, avail, format_, data);
        render_->ReleaseBuffer(avail, 0);
    }
    leaveRealtimeAudioThread(mmcss);
}

}  // namespace dbdeck::win
