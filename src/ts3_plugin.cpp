#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"

namespace {

constexpr int kPluginApiVersion = 26;
constexpr int kSampleRate = 48000;
constexpr int kBitsPerSample = 16;
constexpr int kSendMask = 2;

struct FrameMeta {
    std::uint64_t frameIndex{};
    std::uint64_t offsetFrames{};
    int sampleCount{};
    int channels{};
    bool send{};
    double rmsDbfs{};
};

struct CaptureState {
    std::mutex mutex;
    std::atomic<bool> recording{false};
    uint64 serverConnectionHandlerId{};
    int channels{};
    std::uint64_t totalFrames{};
    std::vector<std::int16_t> pcm;
    std::vector<FrameMeta> frames;
    std::string settingsSnapshot;
};

TS3Functions g_ts3{};
CaptureState g_capture;
std::string g_pluginId;

std::thread g_autoVadThread;
std::atomic<bool> g_autoVadStop{false};
std::atomic<bool> g_autoVadEnabled{true};
std::atomic<bool> g_autoSoundpadOnline{false};
std::atomic<bool> g_autoSoundpadPlaying{false};
std::atomic<bool> g_autoVadOverrideActive{false};
std::atomic<std::uint64_t> g_autoVadServerId{0};

void logMessage(const std::string& message, LogLevel level = LogLevel_INFO, uint64 serverId = 0) {
    if (g_ts3.logMessage) {
        g_ts3.logMessage(message.c_str(), level, "Soundpad-TS3-Diag", serverId);
    }
}

void notify(const std::string& message, LogLevel level = LogLevel_INFO, uint64 serverId = 0) {
    logMessage(message, level, serverId);
    if (g_ts3.printMessageToCurrentTab) {
        g_ts3.printMessageToCurrentTab(message.c_str());
    }
}

std::filesystem::path localAppDataPath() {
    wchar_t buffer[32768]{};
    constexpr DWORD capacity = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, capacity);
    if (len > 0 && len < capacity) {
        return std::filesystem::path(buffer);
    }
    return std::filesystem::temp_directory_path();
}

std::string timestampForPath() {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    std::ostringstream out;
    out << std::setfill('0')
        << st.wYear
        << std::setw(2) << st.wMonth
        << std::setw(2) << st.wDay
        << "-"
        << std::setw(2) << st.wHour
        << std::setw(2) << st.wMinute
        << std::setw(2) << st.wSecond;
    return out.str();
}

void writeU16(std::ofstream& out, std::uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void writeU32(std::ofstream& out, std::uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool writeWav(
    const std::filesystem::path& path,
    const std::vector<std::int16_t>& pcm,
    int channels
) {
    if (channels <= 0) {
        return false;
    }

    const auto dataBytes64 = pcm.size() * sizeof(std::int16_t);
    if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() - 44u) {
        return false;
    }

    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
    const auto byteRate = static_cast<std::uint32_t>(
        kSampleRate * channels * (kBitsPerSample / 8)
    );
    const auto blockAlign = static_cast<std::uint16_t>(
        channels * (kBitsPerSample / 8)
    );

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    out.write("RIFF", 4);
    writeU32(out, 36u + dataBytes);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    writeU32(out, 16u);
    writeU16(out, 1u);
    writeU16(out, static_cast<std::uint16_t>(channels));
    writeU32(out, kSampleRate);
    writeU32(out, byteRate);
    writeU16(out, blockAlign);
    writeU16(out, kBitsPerSample);

    out.write("data", 4);
    writeU32(out, dataBytes);
    out.write(
        reinterpret_cast<const char*>(pcm.data()),
        static_cast<std::streamsize>(dataBytes)
    );

    return static_cast<bool>(out);
}

std::string getPreprocessorValue(uint64 serverId, const char* key) {
    if (!g_ts3.getPreProcessorConfigValue || !g_ts3.freeMemory) {
        return "<unavailable>";
    }

    char* value = nullptr;
    const unsigned int error = g_ts3.getPreProcessorConfigValue(serverId, key, &value);
    if (error != ERROR_ok || value == nullptr) {
        return "<error:" + std::to_string(error) + ">";
    }

    std::string result(value);
    g_ts3.freeMemory(value);
    return result;
}

bool setPreprocessorBool(uint64 serverId, const char* key, bool enabled) {
    if (!g_ts3.setPreProcessorConfigValue) {
        notify(
            std::string("Cannot change ") + key + ": TeamSpeak setter unavailable.",
            LogLevel_ERROR,
            serverId
        );
        return false;
    }

    const char* requested = enabled ? "true" : "false";
    const unsigned int error =
        g_ts3.setPreProcessorConfigValue(serverId, key, requested);

    if (error != ERROR_ok) {
        notify(
            std::string("Failed to set ") + key + "=" + requested +
                " (error " + std::to_string(error) + ").",
            LogLevel_ERROR,
            serverId
        );
        return false;
    }

    const std::string actual = getPreprocessorValue(serverId, key);
    notify(
        std::string("Set ") + key + "=" + requested +
            "; TeamSpeak now reports " + key + "=" + actual + ".",
        actual == requested ? LogLevel_INFO : LogLevel_WARNING,
        serverId
    );

    return actual == requested;
}


enum class SoundpadPlayStatus {
    Offline,
    Stopped,
    Playing,
    Paused,
    Seeking,
    Unknown
};

void closeSoundpadPipe(HANDLE& pipe) {
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
}

bool soundpadRequest(HANDLE& pipe, const char* request, std::string& response) {
    response.clear();

    if (pipe == INVALID_HANDLE_VALUE) {
        pipe = CreateFileW(
            L"\\\\.\\pipe\\sp_remote_control",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            return false;
        }
    }

    DWORD written = 0;
    const DWORD requestLength = static_cast<DWORD>(std::strlen(request));
    if (!WriteFile(pipe, request, requestLength, &written, nullptr) ||
        written != requestLength) {
        closeSoundpadPipe(pipe);
        return false;
    }

    char firstByte = 0;
    DWORD firstRead = 0;
    if (!ReadFile(pipe, &firstByte, 1, &firstRead, nullptr) || firstRead != 1) {
        closeSoundpadPipe(pipe);
        return false;
    }

    response.push_back(firstByte);

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            closeSoundpadPipe(pipe);
            return false;
        }

        if (available == 0) {
            break;
        }

        char buffer[256];
        const DWORD toRead =
            (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        DWORD bytesRead = 0;

        if (!ReadFile(pipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            closeSoundpadPipe(pipe);
            return false;
        }

        response.append(buffer, buffer + bytesRead);
    }

    return true;
}

SoundpadPlayStatus querySoundpadPlayStatus(HANDLE& pipe) {
    std::string response;
    if (!soundpadRequest(pipe, "GetPlayStatus()", response)) {
        return SoundpadPlayStatus::Offline;
    }

    if (response == "STOPPED") {
        return SoundpadPlayStatus::Stopped;
    }
    if (response == "PLAYING") {
        return SoundpadPlayStatus::Playing;
    }
    if (response == "PAUSED") {
        return SoundpadPlayStatus::Paused;
    }
    if (response == "SEEKING") {
        return SoundpadPlayStatus::Seeking;
    }

    return SoundpadPlayStatus::Unknown;
}

bool setPreprocessorBoolRaw(
    uint64 serverId,
    const char* key,
    bool enabled,
    std::string* actualValue = nullptr
) {
    if (!g_ts3.setPreProcessorConfigValue) {
        return false;
    }

    const char* requested = enabled ? "true" : "false";
    const unsigned int error =
        g_ts3.setPreProcessorConfigValue(serverId, key, requested);

    if (error != ERROR_ok) {
        return false;
    }

    const std::string actual = getPreprocessorValue(serverId, key);
    if (actualValue) {
        *actualValue = actual;
    }

    return actual == requested;
}

uint64 currentServerConnectionHandlerId() {
    if (!g_ts3.getCurrentServerConnectionHandlerID) {
        return 0;
    }
    return g_ts3.getCurrentServerConnectionHandlerID();
}

void autoVadWorkerMain() {
    HANDLE soundpadPipe = INVALID_HANDLE_VALUE;

    bool sessionActive = false;
    bool changedVad = false;
    bool savedVad = false;
    uint64 sessionServerId = 0;

    auto restoreSession = [&]() {
        if (sessionActive && changedVad && sessionServerId != 0) {
            std::string actual;
            const bool restored =
                setPreprocessorBoolRaw(sessionServerId, "vad", savedVad, &actual);

            if (restored) {
                logMessage(
                    "Soundpad stopped: restored TeamSpeak VAD to " +
                        std::string(savedVad ? "true" : "false") + ".",
                    LogLevel_INFO,
                    sessionServerId
                );
            } else {
                logMessage(
                    "Soundpad stopped: failed to restore TeamSpeak VAD.",
                    LogLevel_WARNING,
                    sessionServerId
                );
            }
        }

        sessionActive = false;
        changedVad = false;
        savedVad = false;
        sessionServerId = 0;
        g_autoVadOverrideActive.store(false, std::memory_order_release);
        g_autoVadServerId.store(0, std::memory_order_release);
    };

    while (!g_autoVadStop.load(std::memory_order_acquire)) {
        if (!g_autoVadEnabled.load(std::memory_order_acquire)) {
            restoreSession();
            g_autoSoundpadOnline.store(false, std::memory_order_release);
            g_autoSoundpadPlaying.store(false, std::memory_order_release);
            closeSoundpadPipe(soundpadPipe);
            Sleep(100);
            continue;
        }

        const SoundpadPlayStatus playStatus = querySoundpadPlayStatus(soundpadPipe);
        const bool online = playStatus != SoundpadPlayStatus::Offline;
        const bool playing =
            playStatus == SoundpadPlayStatus::Playing ||
            playStatus == SoundpadPlayStatus::Seeking;

        g_autoSoundpadOnline.store(online, std::memory_order_release);
        g_autoSoundpadPlaying.store(playing, std::memory_order_release);

        if (!online) {
            restoreSession();
            Sleep(250);
            continue;
        }

        const uint64 serverId = currentServerConnectionHandlerId();

        if (!playing) {
            restoreSession();
        } else if (serverId != 0 &&
                   (!sessionActive || sessionServerId != serverId)) {
            restoreSession();

            const std::string vadValue = getPreprocessorValue(serverId, "vad");
            if (vadValue == "true" || vadValue == "false") {
                savedVad = (vadValue == "true");
                sessionServerId = serverId;
                sessionActive = true;

                if (savedVad) {
                    std::string actual;
                    changedVad =
                        setPreprocessorBoolRaw(serverId, "vad", false, &actual);

                    if (changedVad) {
                        g_autoVadOverrideActive.store(
                            true,
                            std::memory_order_release
                        );
                        g_autoVadServerId.store(
                            serverId,
                            std::memory_order_release
                        );
                        logMessage(
                            "Soundpad PLAYING: temporarily disabled TeamSpeak VAD.",
                            LogLevel_INFO,
                            serverId
                        );
                    } else {
                        logMessage(
                            "Soundpad PLAYING: failed to disable TeamSpeak VAD.",
                            LogLevel_WARNING,
                            serverId
                        );
                    }
                }
            }
        }

        Sleep(25);
    }

    restoreSession();
    g_autoSoundpadOnline.store(false, std::memory_order_release);
    g_autoSoundpadPlaying.store(false, std::memory_order_release);
    closeSoundpadPipe(soundpadPipe);
}

void startAutoVadWorker() {
    g_autoVadStop.store(false, std::memory_order_release);
    if (!g_autoVadThread.joinable()) {
        g_autoVadThread = std::thread(autoVadWorkerMain);
    }
}

void stopAutoVadWorker() {
    g_autoVadStop.store(true, std::memory_order_release);
    if (g_autoVadThread.joinable()) {
        g_autoVadThread.join();
    }
}

std::string captureSettings(uint64 serverId) {
    static constexpr const char* keys[] = {
        "name",
        "denoise",
        "vad",
        "voiceactivation_level",
        "vad_extrabuffersize",
        "agc",
        "agc_level",
        "agc_max_gain",
        "echo_canceling"
    };

    std::ostringstream out;
    out << "server_connection_handler_id=" << serverId << "\n";
    out << "sample_rate=" << kSampleRate << "\n";
    out << "bits_per_sample=" << kBitsPerSample << "\n";

    for (const char* key : keys) {
        out << key << "=" << getPreprocessorValue(serverId, key) << "\n";
    }

    if (g_ts3.getCurrentCaptureDeviceName && g_ts3.freeMemory) {
        char* deviceName = nullptr;
        int isDefault = 0;
        const unsigned int error =
            g_ts3.getCurrentCaptureDeviceName(serverId, &deviceName, &isDefault);

        if (error == ERROR_ok && deviceName != nullptr) {
            out << "capture_device=" << deviceName << "\n";
            out << "capture_device_is_default=" << isDefault << "\n";
            g_ts3.freeMemory(deviceName);
        } else {
            out << "capture_device=<error:" << error << ">\n";
        }
    }

    return out.str();
}

double calculateRmsDbfs(const short* samples, int totalSamples) {
    if (samples == nullptr || totalSamples <= 0) {
        return -120.0;
    }

    long double sum = 0.0;
    for (int i = 0; i < totalSamples; ++i) {
        const long double normalized =
            static_cast<long double>(samples[i]) / 32768.0L;
        sum += normalized * normalized;
    }

    const long double rms = std::sqrt(sum / static_cast<long double>(totalSamples));
    if (rms <= 1.0e-12L) {
        return -120.0;
    }

    return static_cast<double>(20.0L * std::log10(rms));
}

void startCapture(uint64 serverId) {
    std::lock_guard lock(g_capture.mutex);

    g_capture.pcm.clear();
    g_capture.frames.clear();
    g_capture.channels = 0;
    g_capture.totalFrames = 0;
    g_capture.serverConnectionHandlerId = serverId;
    g_capture.settingsSnapshot = captureSettings(serverId);
    g_capture.recording.store(true, std::memory_order_release);

    notify(
        "Diagnostic capture started. Run the problematic Soundpad sound, then use /spdiag stop.",
        LogLevel_INFO,
        serverId
    );
}

void stopCapture() {
    g_capture.recording.store(false, std::memory_order_release);

    std::vector<std::int16_t> pcm;
    std::vector<FrameMeta> frames;
    std::string settings;
    int channels = 0;
    std::uint64_t totalFrames = 0;
    uint64 serverId = 0;

    {
        std::lock_guard lock(g_capture.mutex);
        pcm = g_capture.pcm;
        frames = g_capture.frames;
        settings = g_capture.settingsSnapshot;
        channels = g_capture.channels;
        totalFrames = g_capture.totalFrames;
        serverId = g_capture.serverConnectionHandlerId;
    }

    const auto captureDir =
        localAppDataPath() /
        L"Soundpad-TS3-Diag" /
        L"captures" /
        std::filesystem::path(timestampForPath());

    std::error_code ec;
    std::filesystem::create_directories(captureDir, ec);
    if (ec) {
        notify(
            "Failed to create capture directory: " + ec.message(),
            LogLevel_ERROR,
            serverId
        );
        return;
    }

    const auto wavPath = captureDir / L"ts3_processed.wav";
    const auto csvPath = captureDir / L"ts3_frames.csv";
    const auto settingsPath = captureDir / L"ts3_settings.txt";
    const auto summaryPath = captureDir / L"summary.txt";

    const bool wavOk = writeWav(wavPath, pcm, channels);

    {
        std::ofstream csv(csvPath);
        csv << "frame_index,offset_ms,sample_count,channels,send,rms_dbfs\n";
        csv << std::fixed << std::setprecision(3);

        for (const auto& frame : frames) {
            const double offsetMs =
                static_cast<double>(frame.offsetFrames) * 1000.0 /
                static_cast<double>(kSampleRate);

            csv << frame.frameIndex << ','
                << offsetMs << ','
                << frame.sampleCount << ','
                << frame.channels << ','
                << (frame.send ? 1 : 0) << ','
                << frame.rmsDbfs << '\n';
        }
    }

    {
        std::ofstream settingsFile(settingsPath);
        settingsFile << settings;
    }

    std::size_t sendFrames = 0;
    for (const auto& frame : frames) {
        if (frame.send) {
            ++sendFrames;
        }
    }

    const std::size_t droppedFrames =
        frames.size() >= sendFrames ? frames.size() - sendFrames : 0;
    const double durationSeconds =
        static_cast<double>(totalFrames) / static_cast<double>(kSampleRate);

    {
        std::ofstream summary(summaryPath);
        summary << "Soundpad-TS3-Diag capture\n";
        summary << "========================\n";
        summary << "TeamSpeak plugin API: " << kPluginApiVersion << "\n";
        summary << "Sample rate: " << kSampleRate << " Hz\n";
        summary << "Channels: " << channels << "\n";
        summary << "Duration: " << std::fixed << std::setprecision(3)
                << durationSeconds << " s\n";
        summary << "Callback frames: " << frames.size() << "\n";
        summary << "Frames marked SEND: " << sendFrames << "\n";
        summary << "Frames marked DROP: " << droppedFrames << "\n";

        if (!frames.empty()) {
            summary << "DROP ratio: "
                    << (100.0 * static_cast<double>(droppedFrames) /
                        static_cast<double>(frames.size()))
                    << " %\n";
        }

        summary << "WAV written: " << (wavOk ? "yes" : "no") << "\n\n";
        summary << "Important: ts3_processed.wav is captured AFTER TeamSpeak "
                   "recording preprocessing and BEFORE encoding/transmission.\n";
        summary << "The SEND flag is taken from the TeamSpeak callback's edited "
                   "bit mask (mask 2).\n";
    }

    notify(
        "Diagnostic capture saved to: " + captureDir.string(),
        wavOk ? LogLevel_INFO : LogLevel_WARNING,
        serverId
    );
}

} // namespace

#if defined(_WIN32)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PLUGIN_EXPORT extern "C"
#endif

PLUGIN_EXPORT const char* ts3plugin_name() {
    return "Soundpad TS3 Diag";
}

PLUGIN_EXPORT const char* ts3plugin_version() {
    return "0.3.0";
}

PLUGIN_EXPORT int ts3plugin_apiVersion() {
    return kPluginApiVersion;
}

PLUGIN_EXPORT const char* ts3plugin_author() {
    return "naskek / OpenAI";
}

PLUGIN_EXPORT const char* ts3plugin_description() {
    return "Automatically disables TeamSpeak VAD while Soundpad is playing and provides SEND/DROP diagnostics.";
}

PLUGIN_EXPORT void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
    g_ts3 = funcs;
}

PLUGIN_EXPORT int ts3plugin_init() {
    startAutoVadWorker();
    notify(
        "Soundpad TS3 Diag loaded. Auto VAD protection is ON. "
        "Commands: /spdiag start, stop, status, settings, auto on|off|status, "
        "vad on|off, denoise on|off."
    );
    return 0;
}

PLUGIN_EXPORT void ts3plugin_shutdown() {
    stopAutoVadWorker();

    if (g_capture.recording.load(std::memory_order_acquire)) {
        stopCapture();
    }
}

PLUGIN_EXPORT int ts3plugin_requestAutoload() {
    return 1;
}

PLUGIN_EXPORT void ts3plugin_registerPluginID(const char* id) {
    g_pluginId = id ? id : "";
}

PLUGIN_EXPORT const char* ts3plugin_commandKeyword() {
    return "spdiag";
}

PLUGIN_EXPORT int ts3plugin_processCommand(
    uint64 serverConnectionHandlerID,
    const char* command
) {
    const std::string value = command ? command : "";

    if (value == "start") {
        if (g_capture.recording.load(std::memory_order_acquire)) {
            notify("Capture is already running.", LogLevel_WARNING, serverConnectionHandlerID);
            return 0;
        }

        startCapture(serverConnectionHandlerID);
        return 0;
    }

    if (value == "stop") {
        if (!g_capture.recording.load(std::memory_order_acquire)) {
            notify("Capture is not running.", LogLevel_WARNING, serverConnectionHandlerID);
            return 0;
        }

        stopCapture();
        return 0;
    }

    if (value == "status") {
        std::ostringstream status;
        status
            << "Capture status: "
            << (g_capture.recording.load(std::memory_order_acquire)
                    ? "RUNNING"
                    : "STOPPED")
            << "\nAuto VAD protection: "
            << (g_autoVadEnabled.load(std::memory_order_acquire)
                    ? "ON"
                    : "OFF")
            << "\nSoundpad: "
            << (g_autoSoundpadOnline.load(std::memory_order_acquire)
                    ? "ONLINE"
                    : "OFFLINE")
            << "\nSoundpad playback: "
            << (g_autoSoundpadPlaying.load(std::memory_order_acquire)
                    ? "PLAYING"
                    : "IDLE")
            << "\nVAD override: "
            << (g_autoVadOverrideActive.load(std::memory_order_acquire)
                    ? "ACTIVE"
                    : "INACTIVE");

        notify(status.str(), LogLevel_INFO, serverConnectionHandlerID);
        return 0;
    }

    if (value == "settings") {
        notify(
            "Current TeamSpeak preprocessing settings:\n" +
                captureSettings(serverConnectionHandlerID),
            LogLevel_INFO,
            serverConnectionHandlerID
        );
        return 0;
    }

    if (value == "auto on") {
        g_autoVadEnabled.store(true, std::memory_order_release);
        notify(
            "Auto VAD protection enabled. It will run automatically on every TeamSpeak start.",
            LogLevel_INFO,
            serverConnectionHandlerID
        );
        return 0;
    }

    if (value == "auto off") {
        g_autoVadEnabled.store(false, std::memory_order_release);
        notify(
            "Auto VAD protection disabled for this TeamSpeak session.",
            LogLevel_INFO,
            serverConnectionHandlerID
        );
        return 0;
    }

    if (value == "auto status") {
        std::ostringstream status;
        status
            << "Auto VAD protection: "
            << (g_autoVadEnabled.load(std::memory_order_acquire) ? "ON" : "OFF")
            << "\nSoundpad: "
            << (g_autoSoundpadOnline.load(std::memory_order_acquire)
                    ? "ONLINE"
                    : "OFFLINE")
            << "\nSoundpad playback: "
            << (g_autoSoundpadPlaying.load(std::memory_order_acquire)
                    ? "PLAYING"
                    : "IDLE")
            << "\nVAD override: "
            << (g_autoVadOverrideActive.load(std::memory_order_acquire)
                    ? "ACTIVE"
                    : "INACTIVE");

        notify(status.str(), LogLevel_INFO, serverConnectionHandlerID);
        return 0;
    }

    if (value == "vad off") {
        setPreprocessorBool(serverConnectionHandlerID, "vad", false);
        return 0;
    }

    if (value == "vad on") {
        setPreprocessorBool(serverConnectionHandlerID, "vad", true);
        return 0;
    }

    if (value == "denoise off") {
        setPreprocessorBool(serverConnectionHandlerID, "denoise", false);
        return 0;
    }

    if (value == "denoise on") {
        setPreprocessorBool(serverConnectionHandlerID, "denoise", true);
        return 0;
    }

    notify(
        "Usage: /spdiag start | stop | status | settings | auto on|off|status | vad on|off | denoise on|off",
        LogLevel_INFO,
        serverConnectionHandlerID
    );
    return 0;
}

PLUGIN_EXPORT void ts3plugin_onEditCapturedVoiceDataEvent(
    uint64 serverConnectionHandlerID,
    short* samples,
    int sampleCount,
    int channels,
    int* edited
) {
    if (!g_capture.recording.load(std::memory_order_acquire)) {
        return;
    }
    if (samples == nullptr || edited == nullptr || sampleCount <= 0 || channels <= 0) {
        return;
    }

    const int totalSamples = sampleCount * channels;
    const bool send = ((*edited & kSendMask) != 0);
    const double rmsDbfs = calculateRmsDbfs(samples, totalSamples);

    std::lock_guard lock(g_capture.mutex);

    if (!g_capture.recording.load(std::memory_order_relaxed)) {
        return;
    }

    if (g_capture.channels == 0) {
        g_capture.channels = channels;
    }

    if (g_capture.channels != channels) {
        return;
    }

    FrameMeta meta{};
    meta.frameIndex = g_capture.frames.size();
    meta.offsetFrames = g_capture.totalFrames;
    meta.sampleCount = sampleCount;
    meta.channels = channels;
    meta.send = send;
    meta.rmsDbfs = rmsDbfs;

    g_capture.frames.push_back(meta);
    g_capture.pcm.insert(
        g_capture.pcm.end(),
        samples,
        samples + totalSamples
    );
    g_capture.totalFrames += static_cast<std::uint64_t>(sampleCount);

    (void)serverConnectionHandlerID;
}
