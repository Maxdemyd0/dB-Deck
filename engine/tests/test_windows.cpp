// Windows integration tests against real audio devices.
//
// These touch the machine's audio:
//  * Render tests play a -20 dBFS tone for ~2 s to the device whose name
//    contains DBDECK_TEST_RENDER_DEVICE (default "Dummy Output", a silent
//    virtual sink). Tests are skipped if no such device exists.
//  * Master-volume tests change the Windows default playback volume/mute,
//    press the keyboard volume key once, and restore the original values.
//    Set DBDECK_TEST_SKIP_MASTER=1 to skip them.

#include <windows.h>
#include <shellapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "AudioEngine.h"
#include "ComUtil.h"
#include "DeviceManager.h"
#include "EndpointVolumeController.h"
#include "EngineService.h"
#include "PipeServer.h"
#include "SessionManager.h"
#include "TestFramework.h"
#include "TestSignals.h"
#include "WasapiStreams.h"
#include "audio/ToneGenerator.h"
#include "graph/GraphCompiler.h"

using namespace dbdeck;
using namespace dbdeck::win;
using namespace std::chrono_literals;

namespace {

// --- child process mode: hold an audio session open --------------------------
// `dbdeck-wintests.exe --play-silence <seconds>` renders silence to the
// default device so the parent can find and control a real session.
struct ChildMode {
    ChildMode() {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argc >= 3 && std::wstring(argv[1]) == L"--play-silence") {
            const int seconds = _wtoi(argv[2]);
            playSilence(seconds);
            ExitProcess(0);
        }
        LocalFree(argv);
    }
    static void playSilence(int seconds) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> en;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        ComPtr<IMMDevice> dev;
        en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        ComPtr<IAudioClient> client;
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
        ComPtr<IAudioRenderClient> render;
        client->GetService(IID_PPV_ARGS(&render));
        UINT32 frames = 0;
        client->GetBufferSize(&frames);
        client->Start();
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (std::chrono::steady_clock::now() < end) {
            UINT32 pad = 0;
            client->GetCurrentPadding(&pad);
            BYTE* data = nullptr;
            if (frames > pad && SUCCEEDED(render->GetBuffer(frames - pad, &data)))
                render->ReleaseBuffer(frames - pad, AUDCLNT_BUFFERFLAGS_SILENT);
            Sleep(20);
        }
        client->Stop();
        CoTaskMemFree(mix);
    }
} g_childMode;

struct Env {
    ComApartment com;
    DeviceManager dm;
    std::vector<DeviceInfo> devices;
    Env() {
        std::string err;
        dm.init(nullptr, err);
        devices = dm.enumerate();
    }
    const DeviceInfo* renderDeviceForTests() {
        char buf[256] = {};
        std::string want = GetEnvironmentVariableA("DBDECK_TEST_RENDER_DEVICE", buf, sizeof(buf)) ? buf : "Dummy Output";
        for (auto& d : devices)
            if (d.render && d.state == "active" && d.name.find(want) != std::string::npos) return &d;
        return nullptr;
    }
};

bool skipMaster() {
    char buf[8] = {};
    return GetEnvironmentVariableA("DBDECK_TEST_SKIP_MASTER", buf, sizeof(buf)) && buf[0] == '1';
}

// Records loopback packets of a render endpoint with their QPC timestamps.
struct LoopbackRecorder {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    DeviceFormat fmt;
    std::vector<float> samples;   // stereo
    std::vector<double> frameTimes;  // QPC seconds for each captured packet's first frame, per frame index
    std::thread thread;
    std::atomic<bool> stop{false};

    bool start(IMMDevice* dev) {
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))))
            return false;
        WAVEFORMATEX* mix = nullptr;
        client->GetMixFormat(&mix);
        fmt = parseWaveFormat(mix);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return false;
        client->Start();
        thread = std::thread([this] {
            ComApartment com;
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            std::vector<float> tmp(48000 * 2);
            while (!stop) {
                Sleep(2);
                UINT32 packet = 0;
                while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
                    BYTE* data;
                    UINT32 frames;
                    DWORD flags;
                    UINT64 qpc = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
                    if (frames > tmp.size() / 2) tmp.resize(frames * 2);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(tmp.begin(), tmp.begin() + frames * 2, 0.0f);
                    else deviceToStereo(data, frames, fmt, tmp.data());
                    samples.insert(samples.end(), tmp.begin(), tmp.begin() + frames * 2);
                    // qpc is in 100-ns units.
                    for (UINT32 i = 0; i < frames; ++i) frameTimes.push_back(qpc * 1e-7 + i / static_cast<double>(fmt.sampleRate));
                    capture->ReleaseBuffer(frames);
                }
            }
        });
        return true;
    }
    void finish() {
        stop = true;
        if (thread.joinable()) thread.join();
        client->Stop();
    }
};

double qpcSeconds() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / f.QuadPart;
}

// Tone that starts at a known time, recording when its first frame was generated.
class OnsetTone final : public PullSource {
public:
    explicit OnsetTone(size_t delayFrames) : delay_(delayFrames), tone_(48000, 1000, 0.1f) {}
    void pull(float* out, size_t frames) override {
        const double now = qpcSeconds();
        for (size_t i = 0; i < frames; ++i, ++pos_) {
            if (pos_ < delay_) {
                out[i * 2] = out[i * 2 + 1] = 0;
            } else {
                if (pos_ == delay_) onset_.store(now);
                tone_.pull(out + i * 2, 1);
            }
        }
    }
    std::atomic<double> onset_{0};

private:
    size_t delay_;
    size_t pos_ = 0;
    ToneGenerator tone_;
};

}  // namespace

// ---------------------------------------------------------------------------

TEST("win: enumerates real playback and recording endpoints") {
    Env env;
    int render = 0, capture = 0;
    for (auto& d : env.devices) {
        CHECK(!d.id.empty());
        CHECK(!d.name.empty());
        (d.render ? render : capture)++;
    }
    std::cout << "       " << render << " playback / " << capture << " recording endpoints\n";
    CHECK(render > 0);
    CHECK(!env.dm.defaultDeviceId(true).empty());
}

TEST("win: session volume and mute change the real Windows session (Milestone 1)") {
    Env env;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --play-silence 6";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    UniqueHandle proc(pi.hProcess), thr(pi.hThread);

    SessionManager sm(env.dm, nullptr);
    AppMatchRule rule = ruleForExe(std::filesystem::path(self).filename().string());
    const SessionInfo* found = nullptr;
    for (int i = 0; i < 40 && !found; ++i) {
        std::this_thread::sleep_for(100ms);
        sm.refresh(env.devices, GetCurrentProcessId());
        for (auto* s : sm.matching(rule))
            if (s->pid == pi.dwProcessId) found = s;
    }
    CHECK(found != nullptr);
    std::cout << "       child session on: " << found->deviceName << "\n";

    auto r = sm.apply(rule, 0.37f, true);
    CHECK(r.matched >= 1);
    // Verify through an independent enumeration (fresh COM objects).
    SessionManager verify(env.dm, nullptr);
    verify.refresh(env.devices, GetCurrentProcessId());
    bool ok = false;
    for (auto* s : verify.matching(rule))
        if (s->pid == pi.dwProcessId) {
            CHECK_NEAR(s->volume, 0.37, 0.001);
            CHECK(s->mute);
            ok = true;
        }
    CHECK(ok);
    sm.apply(rule, 1.0f, false);
    TerminateProcess(pi.hProcess, 0);
}

TEST("win: engine renders real PCM to a playback endpoint (verified by loopback, latency measured)") {
    Env env;
    const DeviceInfo* dev = env.renderDeviceForTests();
    if (!dev) {
        std::cout << "       SKIPPED: no test render device\n";
        return;
    }
    std::cout << "       device: " << dev->name << "\n";
    ComPtr<IMMDevice> imm = env.dm.device(dev->id);
    LoopbackRecorder rec;
    CHECK(rec.start(imm.Get()));

    Mixer mixer;
    RuntimeRegistry reg;
    ProfileConfig p = makeDefaultProfile("t");
    p.mainBus()->deviceId = dev->id;
    ChannelConfig c = makeChannel(ChannelType::TestTone, "tone", p);
    p.channels.push_back(c);
    auto tone = std::make_shared<OnsetTone>(9600);  // 200 ms of silence first
    BufferSettings buf;
    std::string err;
    auto rs = RenderStream::open(env.dm, dev->id, buf, err);
    CHECK(rs != nullptr);
    CompileContext ctx;
    ctx.sourcesFor = [&](const ChannelConfig&) { return std::vector<std::shared_ptr<PullSource>>{tone}; };
    ctx.outputFor = [&](const BusConfig& b) -> std::optional<OutputBinding> { return OutputBinding{b.deviceId, nullptr}; };
    reg.applyParams(p);
    mixer.publish(compileGraph(p, reg, ctx, mixer.nextEpoch()));
    mixer.setClockRunning(true);
    rs->startMaster(&mixer);
    std::this_thread::sleep_for(1500ms);
    rs->stop();
    mixer.setClockRunning(false);
    rec.finish();

    CHECK(rs->status() != StreamStatus::Failed);
    CHECK(!rec.samples.empty());
    // Find onset in the loopback capture.
    size_t onsetIdx = 0;
    for (size_t i = 0; i < rec.samples.size() / 2; ++i)
        if (std::fabs(rec.samples[i * 2]) > 0.02f) {
            onsetIdx = i;
            break;
        }
    CHECK(onsetIdx > 0);
    std::vector<float> tail(rec.samples.begin() + (onsetIdx + 4800) * 2,
                            rec.samples.begin() + std::min(rec.samples.size() / 2, onsetIdx + 4800 + 24000) * 2);
    const double amp = testsig::toneAmplitude(tail, 0, rec.fmt.sampleRate, 1000);
    const double freq = testsig::estimateFrequency(tail, 0, rec.fmt.sampleRate);
    const double latencyMs = (rec.frameTimes[onsetIdx] - tone->onset_.load()) * 1000.0;
    std::cout << "       loopback 1 kHz amplitude " << amp << " (expected 0.1), frequency " << freq
              << " Hz, render latency " << latencyMs << " ms (engine buffer " << rs->latencyMs() << " ms)\n";
    CHECK_NEAR(freq, 1000, 10);
    CHECK(amp > 0.05 && amp < 0.15);
    CHECK(latencyMs > 0 && latencyMs < 100);
}

TEST("win: hardware/loopback capture delivers real PCM into the engine") {
    Env env;
    const DeviceInfo* dev = env.renderDeviceForTests();
    if (!dev) {
        std::cout << "       SKIPPED: no test render device\n";
        return;
    }
    // Play a tone through the engine and capture the same endpoint with the
    // engine's own CaptureStream (endpoint loopback) to verify the input path.
    BufferSettings buf;
    std::string err;
    auto cap = CaptureStream::openDevice(env.dm, dev->id, true, buf, err);
    CHECK(cap != nullptr);
    Mixer mixer;
    RuntimeRegistry reg;
    ProfileConfig p = makeDefaultProfile("t");
    p.mainBus()->deviceId = dev->id;
    p.channels.push_back(makeChannel(ChannelType::TestTone, "tone", p));
    auto tone = std::make_shared<ToneGenerator>(48000, 750, 0.1f);
    auto rs = RenderStream::open(env.dm, dev->id, buf, err);
    CHECK(rs != nullptr);
    CompileContext ctx;
    ctx.sourcesFor = [&](const ChannelConfig&) { return std::vector<std::shared_ptr<PullSource>>{tone}; };
    ctx.outputFor = [&](const BusConfig& b) -> std::optional<OutputBinding> { return OutputBinding{b.deviceId, nullptr}; };
    reg.applyParams(p);
    mixer.publish(compileGraph(p, reg, ctx, mixer.nextEpoch()));
    mixer.setClockRunning(true);
    rs->startMaster(&mixer);
    std::vector<float> got, block(480 * 2);
    for (int i = 0; i < 150; ++i) {  // 1.5 s, pulled at the engine rate
        std::this_thread::sleep_for(10ms);
        cap->stream()->pull(block.data(), 480);
        if (i > 50) got.insert(got.end(), block.begin(), block.end());
    }
    rs->stop();
    mixer.setClockRunning(false);
    cap->stop();
    const double amp = testsig::toneAmplitude(got, 0, 48000, 750);
    std::cout << "       captured 750 Hz amplitude " << amp << ", format " << cap->format().describe() << "\n";
    CHECK(amp > 0.05);
}

TEST("win: Windows master volume sync - dB-Deck to Windows, Windows to dB-Deck, keys, mute") {
    if (skipMaster()) {
        std::cout << "       SKIPPED (DBDECK_TEST_SKIP_MASTER=1)\n";
        return;
    }
    Env env;
    const std::string defId = env.dm.defaultDeviceId(true);
    CHECK(!defId.empty());

    std::mutex m;
    std::condition_variable cv;
    struct Note {
        float v;
        bool mute;
        bool ours;
    };
    std::vector<Note> notes;
    EndpointVolumeController* self = nullptr;
    EndpointVolumeController ctl([&](const std::string&, float v, bool mu, bool ours) {
        if (self) self->onNotification(v, mu, ours);  // the engine does this on its control thread
        std::lock_guard l(m);
        notes.push_back({v, mu, ours});
        cv.notify_all();
    });
    self = &ctl;
    ComPtr<IMMDevice> dev = env.dm.device(defId);
    ctl.bind(dev.Get(), defId, "default");
    EndpointVolumeState st = ctl.state();
    if (!st.available) {
        std::cout << "       SKIPPED: default endpoint has no software volume (" << st.unavailableReason << ")\n";
        return;
    }
    // An independent controller, as Quick Settings would use.
    ComPtr<IAudioEndpointVolume> external;
    CHECK(SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(external.GetAddressOf()))));
    float original = 0;
    BOOL originalMute = FALSE;
    external->GetMasterVolumeLevelScalar(&original);
    external->GetMute(&originalMute);
    // Always restore the user's volume, even if a check fails.
    struct Restore {
        IAudioEndpointVolume* v;
        float vol;
        BOOL mute;
        ~Restore() {
            v->SetMasterVolumeLevelScalar(vol, nullptr);
            v->SetMute(mute, nullptr);
        }
    } restore{external.Get(), original, originalMute};
    auto waitFor = [&](auto pred) {
        std::unique_lock l(m);
        return cv.wait_for(l, 2s, [&] { return std::any_of(notes.begin(), notes.end(), pred); });
    };
    const float a = original > 0.5f ? 0.31f : 0.62f;

    // dB-Deck -> Windows
    std::string err;
    CHECK(ctl.setVolume(a, err));
    float seen = 0;
    external->GetMasterVolumeLevelScalar(&seen);
    CHECK_NEAR(seen, a, 0.01);
    CHECK(waitFor([&](const Note& n) { return n.ours && std::fabs(n.v - a) < 0.01f; }));
    CHECK(ctl.state().lastChangeSource == "dbdeck");

    // Windows (external, e.g. Quick Settings) -> dB-Deck
    const float b = a + 0.12f;
    CHECK(SUCCEEDED(external->SetMasterVolumeLevelScalar(b, nullptr)));
    CHECK(waitFor([&](const Note& n) { return !n.ours && std::fabs(n.v - b) < 0.01f; }));
    CHECK_NEAR(ctl.state().volume, b, 0.01);
    CHECK(ctl.state().lastChangeSource == "external");

    // Keyboard volume key (the same path Windows uses for hardware keys).
    {
        std::lock_guard l(m);
        notes.clear();
    }
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_VOLUME_DOWN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    const bool keySeen = waitFor([&](const Note& n) { return !n.ours && n.v < b - 0.005f; });
    std::cout << "       volume key notification " << (keySeen ? "received" : "NOT received (no interactive desktop?)") << "\n";

    // Redundant set does not reach Windows (no notification).
    ctl.refresh();
    {
        std::lock_guard l(m);
        notes.clear();
    }
    CHECK(ctl.setVolume(ctl.state().volume, err));
    std::this_thread::sleep_for(300ms);
    {
        std::lock_guard l(m);
        CHECK(notes.empty());
    }

    // Mute both directions.
    CHECK(ctl.setMute(!originalMute, err));
    BOOL mu = FALSE;
    external->GetMute(&mu);
    CHECK((mu != FALSE) == (originalMute == FALSE));
    CHECK(SUCCEEDED(external->SetMute(originalMute, nullptr)));
    CHECK(waitFor([&](const Note& n) { return !n.ours && n.mute == (originalMute != FALSE); }));

    std::cout << "       restored volume " << original << " mute " << (originalMute ? "on" : "off") << "\n";
}

TEST("win: endpoint without a device is reported unavailable, not faked") {
    EndpointVolumeController ctl(nullptr);
    ctl.bind(nullptr, "{missing}", "Missing");
    CHECK(!ctl.state().available);
    std::string err;
    CHECK(!ctl.setVolume(0.5f, err));
    CHECK(!err.empty());
}

TEST("win: IPC requires hello, rejects malformed input, serves state") {
    const auto dir = std::filesystem::temp_directory_path() / "dbdeck-tests" / "ipc";
    std::filesystem::remove_all(dir);
    EngineOptions opts;
    opts.configDir = dir;
    opts.pipeName = L"\\\\.\\pipe\\dbdeck-test-" + std::to_wstring(GetCurrentProcessId());
    std::atomic<bool> started{false};
    EngineService* svcPtr = nullptr;
    std::thread engine([&] {
        EngineService svc(opts);
        svcPtr = &svc;
        std::string err;
        if (!svc.start(err)) {
            std::cout << "engine start failed: " << err << "\n";
            started = true;
            return;
        }
        started = true;
        svc.run();
    });
    while (!started) std::this_thread::sleep_for(10ms);

    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50 && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileW(opts.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(50ms);
    }
    CHECK(h != INVALID_HANDLE_VALUE);
    UniqueHandle pipe(h);
    std::string inbox;
    auto sendLine = [&](const std::string& s) {
        std::string l = s + "\n";
        DWORD n;
        WriteFile(pipe.get(), l.data(), static_cast<DWORD>(l.size()), &n, nullptr);
    };
    auto readReply = [&](int64_t id) {
        for (;;) {
            size_t pos;
            while ((pos = inbox.find('\n')) != std::string::npos) {
                json j = json::parse(inbox.substr(0, pos));
                inbox.erase(0, pos + 1);
                if (j.contains("id") && j["id"] == id) return j;
            }
            char buf[65536];
            DWORD n = 0;
            if (!ReadFile(pipe.get(), buf, sizeof(buf), &n, nullptr) || n == 0) return json();
            inbox.append(buf, n);
        }
    };
    sendLine(R"({"id":1,"cmd":"getState"})");
    json r1 = readReply(1);
    CHECK(r1["ok"] == false);  // hello required first
    sendLine("this is not json");
    json r0 = readReply(0);
    CHECK(r0["ok"] == false);
    CHECK(r0["error"]["code"] == "bad_request");
    sendLine(R"({"id":2,"cmd":"hello","args":{"protocol":99}})");
    CHECK(readReply(2)["ok"] == false);
    sendLine(R"({"id":3,"cmd":"hello","args":{"protocol":1}})");
    CHECK(readReply(3)["ok"] == true);
    sendLine(R"({"id":4,"cmd":"getState"})");
    json st = readReply(4);
    CHECK(st["ok"] == true);
    CHECK(st["result"]["profile"]["buses"].size() == 1);
    sendLine(R"({"id":5,"cmd":"createChannel","args":{"type":"testTone","toneHz":440}})");
    json ch = readReply(5);
    CHECK(ch["ok"] == true);
    CHECK(ch["result"]["sends"][0]["busId"] == "main");  // defaults to Main
    sendLine(R"({"id":6,"cmd":"updateChannel","args":{"id":"nope","volume":0.5}})");
    CHECK(readReply(6)["error"]["code"] == "not_found");
    sendLine(R"({"id":7,"cmd":"updateChannel","args":{"id":")" + ch["result"]["id"].get<std::string>() +
             R"(","volume":5}})");
    CHECK(readReply(7)["error"]["code"] == "bad_args");
    // Feedback loop is rejected by the engine.
    std::string anyRender;
    for (auto& d : st["result"]["devices"])
        if (d["flow"] == "render" && d["state"] == "active") anyRender = d["id"];
    if (!anyRender.empty()) {
        sendLine(R"({"id":8,"cmd":"setMainOutput","args":{"deviceId":")" + anyRender + R"("}})");
        CHECK(readReply(8)["ok"] == true);
        sendLine(R"({"id":9,"cmd":"createChannel","args":{"type":"endpointLoopback","deviceId":")" + anyRender + R"("}})");
        json fb = readReply(9);
        CHECK(fb["ok"] == false);
        CHECK(fb["error"]["code"] == "rejected");
    }
    sendLine(R"({"id":10,"cmd":"undo"})");
    CHECK(readReply(10)["ok"] == true);
    sendLine(R"({"id":11,"cmd":"shutdown"})");
    readReply(11);
    engine.join();
    CHECK(std::filesystem::exists(dir / "config.json"));
}
