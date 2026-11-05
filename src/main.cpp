// Macro Studio - macro recorder / player / video renderer for Geometry Dash (Geode)
//
// Press P to open or close the menu.
//   Record : saves every click/release (both players, platformer buttons) per physics frame
//   Play   : replays the macro by injecting those inputs on the exact frames
//   Render : plays the macro at a fixed frame rate and pipes every frame to FFmpeg (Windows)
// Macros are stored as .gdr2 files in the mod's config folder (macros/).
// Videos are written to the renders/ folder next to it.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/utils/file.hpp>
#include <imgui-cocos.hpp>
#include <gdr/gdr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;

// ---------------------------------------------------------------- keybind
static constexpr enumKeyCodes kKeyMenu = KEY_P;   // opens / closes the menu (stops a render)

// ------------------------------------------------------------ macro types
using MInput = gdr::Input<>;

struct MacroReplay : gdr::Replay<MacroReplay, MInput> {
    MacroReplay() : Replay("Macro Studio", 1) {}
};

enum class Mode { Idle, Record, Play };
enum class Tab { Macro, Render, Settings, Library };

// ------------------------------------------------------------ render options
static char const* kResNames[] = {"Match window", "1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
static int const kResW[] = {0, 1280, 1920, 2560, 3840};
static int const kResH[] = {0, 720, 1080, 1440, 2160};
static char const* kFpsNames[] = {"30 FPS", "60 FPS", "120 FPS", "240 FPS"};
static int const kFpsV[] = {30, 60, 120, 240};
static char const* kCodecNames[] = {"H.264 (CPU)", "H.265 (CPU)", "H.264 (NVIDIA)", "H.264 (AMD)", "H.264 (Intel)"};
static char const* kCodecLib[] = {"libx264", "libx265", "h264_nvenc", "h264_amf", "h264_qsv"};
static char const* kPresetNames[] = {"ultrafast", "veryfast", "fast", "medium", "slow"};

// ------------------------------------------------------------------ colors
static ImVec4 const kAccent(0.47f, 0.36f, 0.98f, 1.f);
static ImVec4 const kRed(0.93f, 0.28f, 0.33f, 1.f);
static ImVec4 const kGreen(0.20f, 0.78f, 0.52f, 1.f);
static ImVec4 const kCard(0.14f, 0.15f, 0.19f, 1.f);
static ImVec4 const kCardHover(0.20f, 0.21f, 0.27f, 1.f);

// ------------------------------------------------------------ text helpers
#ifdef GEODE_IS_WINDOWS
static std::wstring toWide(std::string const& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string toUtf8(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

static std::filesystem::path fromUtf8(std::string const& s) {
#ifdef GEODE_IS_WINDOWS
    return std::filesystem::path(toWide(s));
#else
    return std::filesystem::path(s);
#endif
}

static std::string pathToUtf8(std::filesystem::path const& p) {
#ifdef GEODE_IS_WINDOWS
    return toUtf8(p.wstring());
#else
    return p.string();
#endif
}

#ifdef GEODE_IS_WINDOWS
static bool writeAll(HANDLE h, uint8_t const* p, size_t n) {
    while (n > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) return false;
        p += written;
        n -= written;
    }
    return true;
}

// Runs a command with no window and waits for it. Returns the exit code, or -1 if it can't start.
static int runQuiet(std::wstring cmd, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return -1;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    return static_cast<int>(code);
}
#endif

struct Injection {
    uint64_t frame;
    bool down;
    int button;
    bool p1;
};

// ------------------------------------------------------------------- state
struct Studio {
    Mode mode = Mode::Idle;
    Tab tab = Tab::Macro;
    bool menuOpen = false;

    // general settings (saved between sessions)
    bool autoSave = true;
    bool practiceFix = true;
    bool ignoreInput = true;
    bool overlay = true;
    bool overlayDetails = true;
    bool notify = true;
    float speed = 1.f;

    // render settings (saved between sessions)
    int resIdx = 0;
    int fpsIdx = 1;
    int codecIdx = 0;
    int presetIdx = 3;
    int bitrate = 30;           // Mbps
    bool renderAudio = true;
    float audioVolume = 1.f;
    float audioOffset = 0.f;    // extra seconds added on top of the level's own song offset
    bool autoOffset = true;     // start the music where the level's song offset says
    int endDelay = 3;           // seconds recorded after the level is completed
    bool hideUi = true;
    char ffmpegPath[260] = "ffmpeg";
    char outDir[260] = "";
    char audioPath[260] = "";

    // runtime
    MacroReplay macro;
    char name[128] = "macro";
    std::string status = "Ready";
    std::vector<std::filesystem::path> files;
    int selected = -1;
    bool needRefresh = true;
    bool deleteArmed = false;
    bool injecting = false;
    std::unordered_map<CheckpointObject*, uint64_t> cpFrames;

    // Frame the current physics step started on. Inputs are stamped with this so
    // recording and playback always use the same frame numbers.
    uint64_t stepFrame = 0;
    bool inStep = false;
    // Playback cursor: next macro input that has not been injected yet.
    size_t playIdx = 0;
    uint64_t lastPlayFrame = 0;
    std::string lastInput = "-";

    // render runtime
    bool rendering = false;
    bool deathStop = false;
    uint64_t renderedFrames = 0;
    uint64_t endAtFrame = 0;
    int capW = 0;
    int capH = 0;
    std::vector<uint8_t> pixels;
    std::string renderFile;
    std::string ffmpegInfo = "Not checked yet";
    std::chrono::steady_clock::time_point renderStart;
#ifdef GEODE_IS_WINDOWS
    HANDLE pipeIn = nullptr;
    HANDLE logH = nullptr;
    PROCESS_INFORMATION pi{};
#endif

    static Studio& get() {
        static Studio s;
        return s;
    }

    // ------------------------------------------------------------ helpers
    uint64_t frame() const {
        auto* pl = PlayLayer::get();
        if (!pl) return 0;
        return static_cast<uint64_t>(std::max(0, static_cast<int>(pl->m_gameState.m_currentProgress)));
    }

    uint64_t curFrame() const { return inStep ? stepFrame : frame(); }

    void rewindPlayback() {
        playIdx = 0;
        lastPlayFrame = UINT64_MAX; // forces the cursor to be recomputed on the next step
    }

    static bool isPlayLayer(GJBaseGameLayer* gl) {
        auto* pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == gl;
    }

    static std::string clean(std::string s) {
        for (auto& ch : s) {
            if (std::string_view("\\/:*?\"<>|").find(ch) != std::string_view::npos) ch = '_';
        }
        if (s.empty()) s = "macro";
        return s;
    }

    std::filesystem::path dir() const {
        auto d = Mod::get()->getConfigDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return d;
    }

    std::filesystem::path defaultRenderDir() const {
        return Mod::get()->getConfigDir() / "renders";
    }

    void toast(std::string const& msg) const {
        if (notify) Notification::create(msg, NotificationIcon::Success, 0.8f)->show();
    }

    void applySpeed() {
        speed = std::clamp(speed, 0.05f, 2.f);
        CCDirector::get()->getScheduler()->setTimeScale(speed);
    }

    float lengthSeconds() const {
        if (macro.inputs.empty() || macro.framerate <= 0) return 0.f;
        return static_cast<float>(macro.inputs.back().frame / macro.framerate);
    }

    uint64_t lastMacroFrame() const {
        return macro.inputs.empty() ? 0 : macro.inputs.back().frame;
    }

    static void copyStr(char* dst, size_t cap, std::string const& src) {
        std::snprintf(dst, cap, "%s", src.c_str());
    }

    // ----------------------------------------------------------- settings
    void loadSettings() {
        auto* m = Mod::get();
        autoSave = m->getSavedValue<bool>("auto-save", true);
        practiceFix = m->getSavedValue<bool>("practice-fix", true);
        ignoreInput = m->getSavedValue<bool>("ignore-input", true);
        overlay = m->getSavedValue<bool>("overlay", true);
        overlayDetails = m->getSavedValue<bool>("overlay-details", true);
        notify = m->getSavedValue<bool>("notify", true);
        speed = m->getSavedValue<float>("speed", 1.f);

        resIdx = std::clamp(m->getSavedValue<int>("r-res", 0), 0, 4);
        fpsIdx = std::clamp(m->getSavedValue<int>("r-fps", 1), 0, 3);
        codecIdx = std::clamp(m->getSavedValue<int>("r-codec", 0), 0, 4);
        presetIdx = std::clamp(m->getSavedValue<int>("r-preset", 3), 0, 4);
        bitrate = std::clamp(m->getSavedValue<int>("r-bitrate", 30), 1, 150);
        renderAudio = m->getSavedValue<bool>("r-audio", true);
        audioVolume = m->getSavedValue<float>("r-volume", 1.f);
        audioOffset = m->getSavedValue<float>("r-offset", 0.f);
        autoOffset = m->getSavedValue<bool>("r-autooffset", true);
        endDelay = std::clamp(m->getSavedValue<int>("r-end", 3), 0, 15);
        hideUi = m->getSavedValue<bool>("r-hideui", true);
        copyStr(ffmpegPath, sizeof(ffmpegPath), m->getSavedValue<std::string>("r-ffmpeg", "ffmpeg"));
        copyStr(outDir, sizeof(outDir), m->getSavedValue<std::string>("r-outdir", pathToUtf8(defaultRenderDir())));
        copyStr(audioPath, sizeof(audioPath), m->getSavedValue<std::string>("r-audiopath", ""));
        applySpeed();
    }

    void saveSettings() {
        auto* m = Mod::get();
        m->setSavedValue("auto-save", autoSave);
        m->setSavedValue("practice-fix", practiceFix);
        m->setSavedValue("ignore-input", ignoreInput);
        m->setSavedValue("overlay", overlay);
        m->setSavedValue("overlay-details", overlayDetails);
        m->setSavedValue("notify", notify);
        m->setSavedValue("speed", speed);

        m->setSavedValue("r-res", resIdx);
        m->setSavedValue("r-fps", fpsIdx);
        m->setSavedValue("r-codec", codecIdx);
        m->setSavedValue("r-preset", presetIdx);
        m->setSavedValue("r-bitrate", bitrate);
        m->setSavedValue("r-audio", renderAudio);
        m->setSavedValue("r-volume", audioVolume);
        m->setSavedValue("r-offset", audioOffset);
        m->setSavedValue("r-autooffset", autoOffset);
        m->setSavedValue("r-end", endDelay);
        m->setSavedValue("r-hideui", hideUi);
        m->setSavedValue("r-ffmpeg", std::string(ffmpegPath));
        m->setSavedValue("r-outdir", std::string(outDir));
        m->setSavedValue("r-audiopath", std::string(audioPath));
    }

    void resetSettings() {
        autoSave = practiceFix = ignoreInput = overlay = overlayDetails = notify = true;
        speed = 1.f;
        applySpeed();
        resetRenderSettings();
    }

    void resetRenderSettings() {
        resIdx = 0;
        fpsIdx = 1;
        codecIdx = 0;
        presetIdx = 3;
        bitrate = 30;
        renderAudio = true;
        audioVolume = 1.f;
        audioOffset = 0.f;
        autoOffset = true;
        endDelay = 3;
        hideUi = true;
        copyStr(ffmpegPath, sizeof(ffmpegPath), "ffmpeg");
        copyStr(outDir, sizeof(outDir), pathToUtf8(defaultRenderDir()));
        audioPath[0] = '\0';
    }

    // ------------------------------------------------------- file handling
    void fillMeta() {
        auto& in = macro.inputs;
        std::stable_sort(in.begin(), in.end(), [](MInput const& a, MInput const& b) { return a.frame < b.frame; });
        macro.duration = lengthSeconds();
        macro.gameVersion = GEODE_COMP_GD_VERSION;
        macro.author = std::string(GJAccountManager::get()->m_username);
        if (auto* pl = PlayLayer::get()) {
            macro.platformer = pl->m_levelSettings->m_platformerMode;
            macro.levelInfo.id = static_cast<uint32_t>(pl->m_level->m_levelID.value());
            macro.levelInfo.name = std::string(pl->m_level->m_levelName);
        }
    }

    bool saveTo(std::filesystem::path const& p) {
        fillMeta();
        auto data = macro.exportData();
        if (data.isErr()) {
            status = "Save failed: " + data.unwrapErr();
            return false;
        }
        auto w = utils::file::writeBinary(p, data.unwrap());
        if (w.isErr()) {
            status = "Save failed: " + w.unwrapErr();
            return false;
        }
        return true;
    }

    void save() {
        if (macro.inputs.empty()) {
            status = "Nothing to save yet";
            return;
        }
        auto p = dir() / (clean(name) + ".gdr2");
        if (saveTo(p)) {
            status = fmt::format("Saved {} inputs to {}", macro.inputs.size(), p.filename().string());
            needRefresh = true;
        }
    }

    void load(std::filesystem::path const& p) {
        if (rendering) {
            status = "Stop the render first";
            return;
        }
        auto bytes = utils::file::readBinary(p);
        if (bytes.isErr()) {
            status = "Load failed: " + bytes.unwrapErr();
            return;
        }
        auto r = MacroReplay::importData(bytes.unwrap());
        if (r.isErr()) {
            status = "Load failed: " + r.unwrapErr();
            return;
        }
        macro = r.unwrap();
        rewindPlayback();
        // GDR2 stores P1 inputs then P2 inputs, so sort after importing.
        std::stable_sort(macro.inputs.begin(), macro.inputs.end(),
            [](MInput const& a, MInput const& b) { return a.frame < b.frame; });
        std::snprintf(name, sizeof(name), "%s", p.stem().string().c_str());
        status = fmt::format("Loaded {} inputs", macro.inputs.size());
    }

    void refresh() {
        files.clear();
        std::error_code ec;
        for (auto const& e : std::filesystem::directory_iterator(dir(), ec)) {
            if (e.is_regular_file() && e.path().extension() == ".gdr2") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        selected = -1;
        deleteArmed = false;
        needRefresh = false;
    }

    std::string autoName() const {
        std::string lvl = "macro";
        if (auto* pl = PlayLayer::get()) lvl = std::string(pl->m_level->m_levelName);
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef GEODE_IS_WINDOWS
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return clean(lvl) + "_" + buf;
    }

    void clearMacro() {
        if (rendering) {
            status = "Stop the render first";
            return;
        }
        macro = MacroReplay();
        rewindPlayback();
        status = "Macro cleared";
    }

    void restart() {
        if (auto* pl = PlayLayer::get()) pl->resetLevel();
        else status = "Open a level first";
    }

    // ----------------------------------------------------------- modes
    void finishRecording() {
        if (macro.inputs.empty()) {
            status = "Nothing was recorded";
            return;
        }
        fillMeta();
        if (autoSave) {
            auto p = dir() / (autoName() + ".gdr2");
            if (saveTo(p)) {
                status = "Recording saved: " + p.filename().string();
                needRefresh = true;
            }
        } else {
            status = fmt::format("Recorded {} inputs (press Save to keep it)", macro.inputs.size());
        }
    }

    void setMode(Mode m) {
        if (rendering) {
            status = "Stop the render first";
            return;
        }
        if (m == mode) m = Mode::Idle; // clicking the active mode turns it off
        bool wasRecording = mode == Mode::Record && m != Mode::Record;
        if (wasRecording) finishRecording();

        if (m == Mode::Play && macro.inputs.empty()) {
            status = "Load or record a macro first";
            mode = Mode::Idle;
            return;
        }

        mode = m;
        if (m == Mode::Record) {
            macro.inputs.clear();
            status = "Recording from the start of the level";
            toast("Recording");
            restart();
        } else if (m == Mode::Play) {
            status = "Playing macro";
            rewindPlayback();
            toast("Playing");
            restart();
        } else if (!wasRecording) {
            status = "Ready";
        }
    }

    // ------------------------------------------------------------ rendering
    bool renderSupported() const {
#ifdef GEODE_IS_WINDOWS
        return true;
#else
        return false;
#endif
    }

    // Looks for FFmpeg: the saved path first, then the mod folder, then the system PATH.
    void detectFfmpeg() {
#ifdef GEODE_IS_WINDOWS
        auto works = [](std::string const& path) {
            return runQuiet(L"\"" + toWide(path) + L"\" -version", 5000) == 0;
        };
        if (works(ffmpegPath)) {
            ffmpegInfo = "FFmpeg found";
            return;
        }
        std::vector<std::filesystem::path> candidates = {
            Mod::get()->getConfigDir() / "ffmpeg" / "ffmpeg.exe",
            Mod::get()->getConfigDir() / "ffmpeg.exe",
            Mod::get()->getConfigDir() / "ffmpeg" / "bin" / "ffmpeg.exe",
        };
        for (auto const& c : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(c, ec)) continue;
            auto s = pathToUtf8(c);
            if (works(s)) {
                copyStr(ffmpegPath, sizeof(ffmpegPath), s);
                ffmpegInfo = "FFmpeg found in the mod folder";
                return;
            }
        }
        if (works("ffmpeg")) {
            copyStr(ffmpegPath, sizeof(ffmpegPath), "ffmpeg");
            ffmpegInfo = "FFmpeg found on your PATH";
            return;
        }
        ffmpegInfo = "FFmpeg not found. Put ffmpeg.exe in the mod folder or set its path below.";
#else
        ffmpegInfo = "Rendering is only available on Windows";
#endif
    }

    // Works out which audio file to mix into the video (empty = no audio).
    std::filesystem::path resolveAudio() const {
        std::error_code ec;
        if (audioPath[0] != '\0') {
            auto p = fromUtf8(audioPath);
            return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
        }
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_level) return {};
        auto* lvl = pl->m_level;

        std::string file;
        if (lvl->m_songID > 0) file = std::string(MusicDownloadManager::sharedState()->pathForSong(lvl->m_songID));
        else file = std::string(LevelTools::getAudioFileName(lvl->m_audioTrack));
        if (file.empty()) return {};

        auto p = fromUtf8(file);
        if (std::filesystem::exists(p, ec)) return p;
        std::string full = CCFileUtils::sharedFileUtils()->fullPathForFilename(file.c_str(), false);
        p = fromUtf8(full);
        return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
    }

#ifdef GEODE_IS_WINDOWS
    std::wstring buildCommand(int fps, int ow, int oh, std::filesystem::path const& out,
                              std::filesystem::path const& audio, float offsetSec) const {
        auto q = [](std::wstring const& s) { return L"\"" + s + L"\""; };
        auto num = [](char const* fmtStr, double v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), fmtStr, v);
            return toWide(buf);
        };
        std::wstring W = std::to_wstring(ow);
        std::wstring H = std::to_wstring(oh);

        std::wstring c = q(toWide(ffmpegPath)) + L" -y -hide_banner -loglevel warning";
        c += L" -f rawvideo -pix_fmt rgba -s " + std::to_wstring(capW) + L"x" + std::to_wstring(capH);
        c += L" -r " + std::to_wstring(fps) + L" -i -";

        bool withAudio = !audio.empty();
        if (withAudio) {
            c += offsetSec >= 0.f ? L" -ss " : L" -itsoffset ";
            c += num("%.3f", std::abs(offsetSec)) + L" -i " + q(audio.wstring());
        }

        // OpenGL frames are upside down, so flip, then fit into the output size.
        c += L" -vf " + q(L"vflip,scale=" + W + L":" + H +
            L":force_original_aspect_ratio=decrease:flags=lanczos,pad=" + W + L":" + H +
            L":(ow-iw)/2:(oh-ih)/2");

        c += L" -c:v " + toWide(kCodecLib[codecIdx]);
        if (codecIdx <= 1) c += L" -preset " + toWide(kPresetNames[presetIdx]);
        if (codecIdx == 1) c += L" -tag:v hvc1";
        c += L" -b:v " + std::to_wstring(bitrate) + L"M -pix_fmt yuv420p";

        if (withAudio) {
            c += L" -map 0:v -map 1:a -af volume=" + num("%.2f", audioVolume);
            c += L" -c:a aac -b:a 192k -shortest";
        }
        c += L" -movflags +faststart " + q(out.wstring());
        return c;
    }
#endif

    void startRender() {
#ifdef GEODE_IS_WINDOWS
        if (rendering) return;
        auto* pl = PlayLayer::get();
        if (!pl) {
            status = "Open a level first";
            return;
        }
        if (macro.inputs.empty()) {
            status = "Load or record a macro first";
            return;
        }
        if (mode == Mode::Record) finishRecording();

        auto sz = CCEGLView::get()->getFrameSize();
        capW = static_cast<int>(sz.width);
        capH = static_cast<int>(sz.height);
        if (capW < 16 || capH < 16) {
            status = "The game window is too small to capture";
            return;
        }

        int fps = kFpsV[fpsIdx];
        int ow = kResW[resIdx];
        int oh = kResH[resIdx];
        if (ow == 0) {
            ow = capW & ~1;
            oh = capH & ~1;
        }

        auto outFolder = fromUtf8(outDir);
        if (outFolder.empty()) outFolder = defaultRenderDir();
        std::error_code ec;
        std::filesystem::create_directories(outFolder, ec);
        auto outPath = outFolder / (autoName() + ".mp4");

        std::filesystem::path audio;
        if (renderAudio) audio = resolveAudio();

        // The level's own song start time (the Song Offset in the level settings) plus your fine-tuning.
        // A custom audio file is used as-is, so only your own offset applies to it.
        float offsetSec = audioOffset;
        if (autoOffset && audioPath[0] == '\0' && pl->m_levelSettings) offsetSec += pl->m_levelSettings->m_songOffset;

        auto cmd = buildCommand(fps, ow, oh, outPath, audio, offsetSec);

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE rd = nullptr;
        HANDLE wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) {
            status = "Could not create the video pipe";
            return;
        }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0); // the child must only inherit the read end

        auto logPath = Mod::get()->getConfigDir() / "render.log";
        logH = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = logH == INVALID_HANDLE_VALUE ? nullptr : logH;
        si.hStdError = logH == INVALID_HANDLE_VALUE ? nullptr : logH;
        pi = {};
        bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi) != 0;
        CloseHandle(rd);
        if (!ok) {
            CloseHandle(wr);
            if (logH != INVALID_HANDLE_VALUE && logH) CloseHandle(logH);
            logH = nullptr;
            status = "Could not start FFmpeg. Check the FFmpeg path on the Render tab.";
            return;
        }
        pipeIn = wr;

        renderFile = outPath.filename().string();
        renderedFrames = 0;
        endAtFrame = 0;
        deathStop = false;
        renderStart = std::chrono::steady_clock::now();
        rendering = true;

        CCDirector::get()->getScheduler()->setTimeScale(1.f); // the render uses its own fixed time step
        if (hideUi) menuOpen = false;

        mode = Mode::Play;
        rewindPlayback();
        if (!renderAudio) status = "Rendering (no audio)";
        else if (audio.empty()) status = "Rendering (no audio file found)";
        else status = fmt::format("Rendering (music starts at {:.2f}s)", offsetSec);
        restart();
#else
        status = "Rendering is only available on Windows";
#endif
    }

    void stopRender(std::string const& reason, bool failed) {
#ifdef GEODE_IS_WINDOWS
        if (!rendering) return;
        rendering = false;
        applySpeed();

        if (pipeIn) {
            CloseHandle(pipeIn); // tells FFmpeg the video is over
            pipeIn = nullptr;
        }
        auto proc = pi;
        auto log = logH;
        pi = {};
        logH = nullptr;
        auto file = renderFile;
        auto frames = renderedFrames;
        status = failed ? reason : "Finishing the video...";
        menuOpen = true;

        std::thread([proc, log, file, frames, reason, failed]() {
            DWORD code = 1;
            if (WaitForSingleObject(proc.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(proc.hProcess, &code);
            else TerminateProcess(proc.hProcess, 1);
            CloseHandle(proc.hProcess);
            CloseHandle(proc.hThread);
            if (log && log != INVALID_HANDLE_VALUE) CloseHandle(log);
            Loader::get()->queueInMainThread([code, file, frames, reason, failed]() {
                Studio::get().onRenderFinished(code == 0 && !failed, file, frames, reason);
            });
        }).detach();
#else
        (void)reason;
        (void)failed;
#endif
    }

    void onRenderFinished(bool ok, std::string const& file, uint64_t frames, std::string const& reason) {
        std::string msg;
        if (ok) msg = fmt::format("Render saved: {} ({} frames)", file, frames);
        else if (reason.empty() || reason == "Stopped by user") msg = "FFmpeg reported an error. Open the mod folder and read render.log";
        else msg = "Render failed: " + reason;
        status = msg;
        Notification::create(msg, ok ? NotificationIcon::Success : NotificationIcon::Error, 3.f)->show();
    }

    // Called from the buffer-swap hook once per displayed frame.
    void captureFrame() {
#ifdef GEODE_IS_WINDOWS
        if (!rendering || !pipeIn) return;
        if (!PlayLayer::get()) {
            stopRender("Left the level", false);
            return;
        }

        auto sz = CCEGLView::get()->getFrameSize();
        int w = static_cast<int>(sz.width);
        int h = static_cast<int>(sz.height);
        if (w != capW || h != capH) {
            stopRender("The game window changed size during the render", true);
            return;
        }

        pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        if (!writeAll(pipeIn, pixels.data(), pixels.size())) {
            stopRender("FFmpeg stopped unexpectedly. Check render.log", true);
            return;
        }
        ++renderedFrames;

        int fps = kFpsV[fpsIdx];
        // Safety net: macro ended long ago and the level never finished.
        if (!endAtFrame && frame() > lastMacroFrame() + static_cast<uint64_t>(120.0 * std::max(1.0, static_cast<double>(macro.framerate)))) {
            endAtFrame = renderedFrames;
        }
        if (endAtFrame && renderedFrames >= endAtFrame) {
            stopRender(deathStop ? "Player died during the render" : "Render finished", false);
        }
        (void)fps;
#endif
    }

    void onDeath() {
        if (!rendering || endAtFrame) return;
        deathStop = true;
        endAtFrame = renderedFrames + static_cast<uint64_t>(kFpsV[fpsIdx]); // keep 1 second of the death
        status = "Player died: finishing the render";
    }

    // --------------------------------------------------- game-side events
    // Returns false to drop the input.
    bool onInput(GJBaseGameLayer* gl, bool down, int button, bool p1) {
        if (injecting || !isPlayLayer(gl)) return true;
        if (mode == Mode::Play) return !(ignoreInput || rendering);
        if (mode == Mode::Record) {
            auto f = curFrame();
            macro.inputs.emplace_back(f, static_cast<uint8_t>(button), !p1, down);
            lastInput = fmt::format("F {} {} {}", f, down ? "down" : "up", p1 ? "P1" : "P2");
        }
        return true;
    }

    // Called at the start of every physics step.
    void beginStep(GJBaseGameLayer* gl) {
        if (!isPlayLayer(gl)) return;
        stepFrame = frame();
        inStep = true;
        if (mode != Mode::Play) return;

        auto f = stepFrame;
        auto& in = macro.inputs;

        // Time went backwards (restart or checkpoint): find where we are in the macro again.
        if (f < lastPlayFrame) {
            playIdx = static_cast<size_t>(std::lower_bound(in.begin(), in.end(), f,
                [](MInput const& a, uint64_t v) { return a.frame < v; }) - in.begin());
        }
        lastPlayFrame = f;

        // Catch-up: inject every input that is due, so a skipped frame can never lose one.
        while (playIdx < in.size() && in[playIdx].frame <= f) {
            auto const& i = in[playIdx];
            injecting = true;
            gl->handleButton(i.down, static_cast<int>(i.button), !i.player2);
            injecting = false;
            ++playIdx;
        }
    }

    void endStep() {
        inStep = false;
    }

    void onLevelReset(PlayLayer* pl) {
        inStep = false;
        rewindPlayback();
        // A restart from the very beginning means a fresh attempt.
        if (!pl->m_isPracticeMode || frame() == 0) {
            cpFrames.clear();
            if (mode == Mode::Record) macro.inputs.clear();
        }
    }

    void onCheckpoint(CheckpointObject* cp) {
        if (cp) cpFrames[cp] = frame();
    }

    // Practice fix: going back to a checkpoint throws away inputs recorded after it.
    void onCheckpointLoad(CheckpointObject* cp) {
        if (mode != Mode::Record || !practiceFix) return;
        auto it = cpFrames.find(cp);
        if (it == cpFrames.end()) return;
        auto f = it->second;
        std::erase_if(macro.inputs, [&](MInput const& i) { return i.frame >= f; });
    }

    void onComplete() {
        if (rendering) {
            if (!endAtFrame) {
                endAtFrame = renderedFrames + static_cast<uint64_t>(endDelay) * static_cast<uint64_t>(kFpsV[fpsIdx]);
                status = "Level complete: finishing the render";
            }
            return;
        }
        if (mode != Mode::Record) return;
        finishRecording();
        mode = Mode::Idle;
        toast("Recording finished");
    }

    void onQuit() {
        if (rendering) stopRender("Left the level", false);
        if (mode == Mode::Record) {
            finishRecording();
            mode = Mode::Idle;
        }
        cpFrames.clear();
    }
};

// -------------------------------------------------------------- UI styling
static void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.f;
    st.ChildRounding = 10.f;
    st.FrameRounding = 8.f;
    st.PopupRounding = 8.f;
    st.GrabRounding = 8.f;
    st.ScrollbarRounding = 8.f;
    st.WindowPadding = ImVec2(16.f, 14.f);
    st.FramePadding = ImVec2(10.f, 7.f);
    st.ItemSpacing = ImVec2(10.f, 9.f);
    st.WindowBorderSize = 1.f;
    st.FrameBorderSize = 0.f;
    st.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.97f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.57f, 0.64f, 1.f);
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.080f, 0.100f, 0.97f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.095f, 0.12f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.f, 1.f, 1.f, 0.07f);
    c[ImGuiCol_FrameBg] = kCard;
    c[ImGuiCol_FrameBgHovered] = kCardHover;
    c[ImGuiCol_FrameBgActive] = kCardHover;
    c[ImGuiCol_TitleBg] = ImVec4(0.075f, 0.080f, 0.100f, 1.f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.080f, 0.100f, 1.f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.075f, 0.080f, 0.100f, 1.f);
    c[ImGuiCol_Button] = kCard;
    c[ImGuiCol_ButtonHovered] = kCardHover;
    c[ImGuiCol_ButtonActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.50f, 1.f, 1.f);
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.50f);
    c[ImGuiCol_HeaderActive] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.70f);
    c[ImGuiCol_Separator] = ImVec4(1.f, 1.f, 1.f, 0.08f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.f, 1.f, 1.f, 0.15f);
}

// -------------------------------------------------------------- UI helpers
static void tip(char const* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static bool coloredButton(char const* label, bool active, ImVec4 const& col, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, active ? col : kCard);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? col : kCardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return pressed;
}

static void tabButton(char const* label, Tab t, float w) {
    auto& s = Studio::get();
    if (coloredButton(label, s.tab == t, kAccent, ImVec2(w, 32.f))) s.tab = t;
}

static void stat(char const* label, std::string const& value) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TextUnformatted(value.c_str());
}

// ----------------------------------------------------------------- tabs
static void drawMacroTab() {
    auto& s = Studio::get();
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w3 = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
    float w2 = (ImGui::GetContentRegionAvail().x - gap) / 2.f;

    if (coloredButton("Idle", s.mode == Mode::Idle, kAccent, ImVec2(w3, 44.f))) s.setMode(Mode::Idle);
    ImGui::SameLine();
    if (coloredButton("Record", s.mode == Mode::Record, kRed, ImVec2(w3, 44.f))) s.setMode(Mode::Record);
    ImGui::SameLine();
    if (coloredButton("Play", s.mode == Mode::Play, kGreen, ImVec2(w3, 44.f))) s.setMode(Mode::Play);

    ImGui::Spacing();
    if (ImGui::BeginTable("stats", 4)) {
        stat("Frame", fmt::format("{}", s.frame()));
        stat("Inputs", fmt::format("{}", s.macro.inputs.size()));
        stat("FPS", fmt::format("{:.0f}", s.macro.framerate));
        stat("Length", fmt::format("{:.1f}s", s.lengthSeconds()));
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Last input: %s", s.lastInput.c_str());

    ImGui::Separator();
    if (ImGui::Button("Restart level", ImVec2(-1.f, 36.f))) s.restart();
    tip("Starts the level over from frame 0");

    ImGui::InputText("Name", s.name, sizeof(s.name));
    if (ImGui::Button("Save", ImVec2(w2, 34.f))) s.save();
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(w2, 34.f))) s.clearMacro();

    if (s.macro.inputs.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No macro loaded. Record one, or open the Library tab.");
    }
}

static void drawRenderTab() {
    auto& s = Studio::get();

    if (!s.renderSupported()) {
        ImGui::TextWrapped("Rendering is only available on Windows.");
        return;
    }

    // While a render is running, show progress instead of settings.
    if (s.rendering) {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.renderStart).count();
        double videoSec = static_cast<double>(s.renderedFrames) / kFpsV[s.fpsIdx];
        float frac = s.endAtFrame
            ? 1.f
            : std::clamp(static_cast<float>(s.frame()) / static_cast<float>(s.lastMacroFrame() + 1), 0.f, 1.f);

        ImGui::TextColored(kRed, "Rendering...");
        ImGui::ProgressBar(frac, ImVec2(-1.f, 0.f));
        ImGui::Text("Frames: %llu    Video: %.1fs    Elapsed: %.0fs",
            static_cast<unsigned long long>(s.renderedFrames), videoSec, elapsed);
        ImGui::TextDisabled("Speed: %.2fx realtime", elapsed > 0.1 ? videoSec / elapsed : 0.0);
        ImGui::Spacing();
        if (coloredButton("Stop render", true, kRed, ImVec2(-1.f, 40.f))) s.stopRender("Stopped by user", false);
        ImGui::TextDisabled("You can also press P to stop. Don't resize the game window.");
        return;
    }

    auto sz = CCEGLView::get()->getFrameSize();
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w2 = (ImGui::GetContentRegionAvail().x - gap) / 2.f;

    ImGui::BeginDisabled(s.macro.inputs.empty());
    if (coloredButton("Start render", true, kAccent, ImVec2(-1.f, 44.f))) s.startRender();
    ImGui::EndDisabled();
    if (s.macro.inputs.empty()) ImGui::TextDisabled("Load or record a macro first (Macro / Library tab).");
    ImGui::TextDisabled("Captures the game window: %d x %d. Use a bigger window for sharper video.",
        static_cast<int>(sz.width), static_cast<int>(sz.height));
    ImGui::TextDisabled("Tip: set the game's music volume to 0 while rendering. Audio is added from the song file.");

    ImGui::SeparatorText("Video");
    ImGui::Combo("Resolution", &s.resIdx, kResNames, IM_ARRAYSIZE(kResNames));
    ImGui::Combo("Frame rate", &s.fpsIdx, kFpsNames, IM_ARRAYSIZE(kFpsNames));
    tip("Should divide the physics rate (240) evenly. 60 is the usual choice.");
    ImGui::Combo("Encoder", &s.codecIdx, kCodecNames, IM_ARRAYSIZE(kCodecNames));
    tip("NVIDIA / AMD / Intel encoders are much faster if you have that graphics card.");
    if (s.codecIdx <= 1) ImGui::Combo("Preset", &s.presetIdx, kPresetNames, IM_ARRAYSIZE(kPresetNames));
    ImGui::SliderInt("Bitrate", &s.bitrate, 1, 150, "%d Mbps");
    tip("Higher = better quality and bigger files. 20-40 is good for 1080p60.");

    ImGui::SeparatorText("Audio");
    ImGui::Checkbox("Add level music", &s.renderAudio);
    if (s.renderAudio) {
        ImGui::SliderFloat("Volume", &s.audioVolume, 0.f, 2.f, "%.2fx");
        ImGui::Checkbox("Use level song offset", &s.autoOffset);
        tip("Starts the music where the level starts it (the Song Offset set in the level editor).");
        if (auto* pl = PlayLayer::get()) {
            if (pl->m_levelSettings) ImGui::TextDisabled("This level starts its song at %.2f s", pl->m_levelSettings->m_songOffset);
        }
        ImGui::SliderFloat("Extra offset", &s.audioOffset, -10.f, 60.f, "%.2f s");
        tip("Fine-tune on top of the level offset. Use it if the music is slightly out of sync.");
        ImGui::InputText("Audio file", s.audioPath, sizeof(s.audioPath));
        tip("Optional. Leave empty to use the level's own song.");
    }

    ImGui::SeparatorText("Output");
    ImGui::InputText("Folder", s.outDir, sizeof(s.outDir));
    if (ImGui::Button("Open renders folder", ImVec2(w2, 32.f))) {
        auto d = fromUtf8(s.outDir);
        if (d.empty()) d = s.defaultRenderDir();
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        utils::file::openFolder(d);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open mod folder", ImVec2(w2, 32.f))) utils::file::openFolder(Mod::get()->getConfigDir());
    ImGui::SliderInt("End delay", &s.endDelay, 0, 15, "%d s");
    tip("Seconds kept after the level is completed (the end screen).");
    ImGui::Checkbox("Hide menu while rendering", &s.hideUi);
    tip("Keeps the menu out of the video. Press P to stop a render.");

    ImGui::SeparatorText("FFmpeg");
    ImGui::InputText("Path", s.ffmpegPath, sizeof(s.ffmpegPath));
    tip("Full path to ffmpeg.exe, or just 'ffmpeg' if it is on your PATH.");
    if (ImGui::Button("Detect / test FFmpeg", ImVec2(-1.f, 32.f))) s.detectFfmpeg();
    ImGui::TextWrapped("%s", s.ffmpegInfo.c_str());
    ImGui::TextDisabled("Get FFmpeg from gyan.dev (Windows builds) and put ffmpeg.exe in the mod folder.");

    ImGui::Spacing();
    if (ImGui::Button("Reset render settings", ImVec2(-1.f, 32.f))) s.resetRenderSettings();
}

static void drawSettingsTab() {
    auto& s = Studio::get();

    ImGui::SeparatorText("Recording");
    ImGui::Checkbox("Auto-save recordings", &s.autoSave);
    tip("Saves the macro by itself when recording ends");
    ImGui::Checkbox("Practice mode fix", &s.practiceFix);
    tip("Drops inputs recorded after a checkpoint when you respawn on it (basic)");

    ImGui::SeparatorText("Playback");
    ImGui::Checkbox("Ignore my clicks while playing", &s.ignoreInput);
    tip("Blocks your own inputs while a macro is playing");

    ImGui::SeparatorText("Speed");
    if (ImGui::SliderFloat("Speed", &s.speed, 0.05f, 2.f, "%.2fx")) s.applySpeed();
    tip("Game speed. Slow speeds make hard sections easier to record");
    if (ImGui::Button("Reset speed")) {
        s.speed = 1.f;
        s.applySpeed();
    }

    ImGui::SeparatorText("Display");
    ImGui::Checkbox("Status overlay", &s.overlay);
    ImGui::Checkbox("Overlay details", &s.overlayDetails);
    tip("Shows the frame number in the overlay");
    ImGui::Checkbox("Notifications", &s.notify);

    ImGui::Spacing();
    if (ImGui::Button("Reset all settings", ImVec2(-1.f, 34.f))) s.resetSettings();
}

static void drawLibraryTab() {
    auto& s = Studio::get();
    if (s.needRefresh) s.refresh();

    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w2 = (ImGui::GetContentRegionAvail().x - gap) / 2.f;
    if (ImGui::Button("Refresh", ImVec2(w2, 32.f))) s.refresh();
    ImGui::SameLine();
    if (ImGui::Button("Open folder", ImVec2(w2, 32.f))) utils::file::openFolder(s.dir());

    ImGui::BeginChild("files", ImVec2(0.f, 230.f), true);
    if (s.files.empty()) ImGui::TextDisabled("No macros saved yet.");
    for (int i = 0; i < static_cast<int>(s.files.size()); ++i) {
        auto label = fmt::format("{}##{}", s.files[i].stem().string(), i);
        if (ImGui::Selectable(label.c_str(), s.selected == i)) {
            s.selected = i;
            s.deleteArmed = false;
        }
    }
    ImGui::EndChild();

    bool has = s.selected >= 0 && s.selected < static_cast<int>(s.files.size());
    if (ImGui::Button("Load", ImVec2(w2, 34.f)) && has) s.load(s.files[s.selected]);
    ImGui::SameLine();
    if (ImGui::Button(s.deleteArmed ? "Confirm delete" : "Delete", ImVec2(w2, 34.f)) && has) {
        if (!s.deleteArmed) {
            s.deleteArmed = true;
        } else {
            std::error_code ec;
            std::filesystem::remove(s.files[s.selected], ec);
            s.status = ec ? "Delete failed" : "Macro deleted";
            s.refresh();
        }
    }
}

static void drawWindow() {
    auto& s = Studio::get();

    ImGui::SetNextWindowPos(ImVec2(40.f, 40.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440.f, 560.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Macro Studio", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w4 = (ImGui::GetContentRegionAvail().x - gap * 3.f) / 4.f;
    tabButton("Macro", Tab::Macro, w4);
    ImGui::SameLine();
    tabButton("Render", Tab::Render, w4);
    ImGui::SameLine();
    tabButton("Settings", Tab::Settings, w4);
    ImGui::SameLine();
    tabButton("Library", Tab::Library, w4);
    ImGui::Spacing();

    switch (s.tab) {
        case Tab::Macro: drawMacroTab(); break;
        case Tab::Render: drawRenderTab(); break;
        case Tab::Settings: drawSettingsTab(); break;
        case Tab::Library: drawLibraryTab(); break;
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", s.status.c_str());
    ImGui::End();
}

// Small pill at the top of the screen while recording / playing.
static void drawOverlay() {
    auto& s = Studio::get();
    if (!s.overlay || !PlayLayer::get()) return;

    bool rec = s.mode == Mode::Record;
    bool play = s.mode == Mode::Play;
    bool sped = std::abs(s.speed - 1.f) > 0.001f;
    if (!rec && !play && !sped) return;

    std::string txt = rec ? "REC" : (play ? "PLAY" : "");
    if (s.overlayDetails && (rec || play)) txt += fmt::format("   F {}", s.frame());
    if (sped) txt += fmt::format("{}{:.2f}x", txt.empty() ? "" : "   ", s.speed);

    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float padX = 14.f, padY = 7.f;
    float dot = (rec || play) ? 18.f : 0.f;
    float w = ts.x + padX * 2.f + dot;
    float h = ts.y + padY * 2.f;
    ImVec2 p0((ds.x - w) * 0.5f, 12.f);
    ImVec2 p1(p0.x + w, p0.y + h);

    auto* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(15, 16, 22, 205), h * 0.5f);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 26), h * 0.5f);
    if (rec || play) {
        float pulse = rec ? 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.f) : 1.f;
        ImU32 col = rec ? IM_COL32(237, 71, 84, static_cast<int>(pulse * 255.f)) : IM_COL32(51, 199, 133, 255);
        dl->AddCircleFilled(ImVec2(p0.x + padX + 5.f, p0.y + h * 0.5f), 5.f, col);
    }
    dl->AddText(ImVec2(p0.x + padX + dot, p0.y + padY), IM_COL32(240, 242, 247, 255), txt.c_str());
}

// -------------------------------------------------------------- mod events
$on_mod(Loaded) {
    Studio::get().loadSettings();
    ImGuiCocos::get().setup([] {
        applyStyle();
    }).draw([] {
        auto& s = Studio::get();
        bool hidden = s.rendering && s.hideUi; // keep the menu and overlay out of the video
        if (s.menuOpen && !hidden) drawWindow();
        if (!hidden) drawOverlay();
    });
}

$on_mod(DataSaved) {
    Studio::get().saveSettings();
}

// ------------------------------------------------------------------- hooks
class $modify(StudioBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        Studio::get().beginStep(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        Studio::get().endStep();
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!Studio::get().onInput(this, down, button, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(StudioPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        Studio::get().onLevelReset(this);
    }

    void onQuit() {
        Studio::get().onQuit();
        PlayLayer::onQuit();
    }

    void levelComplete() {
        Studio::get().onComplete();
        PlayLayer::levelComplete();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object != m_anticheatSpike) Studio::get().onDeath();
        PlayLayer::destroyPlayer(player, object);
    }

    CheckpointObject* createCheckpoint() {
        auto* cp = PlayLayer::createCheckpoint();
        Studio::get().onCheckpoint(cp);
        return cp;
    }

    void loadFromCheckpoint(CheckpointObject* cp) {
        PlayLayer::loadFromCheckpoint(cp);
        Studio::get().onCheckpointLoad(cp);
    }
};

// While rendering, every game frame advances by exactly one video frame.
class $modify(StudioScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = Studio::get();
        if (s.rendering) dt = 1.f / static_cast<float>(kFpsV[s.fpsIdx]);
        CCScheduler::update(dt);
    }
};

// Grab the finished frame right before it is shown on screen.
class $modify(StudioView, CCEGLView) {
    void swapBuffers() {
        Studio::get().captureFrame();
        CCEGLView::swapBuffers();
    }
};

class $modify(StudioKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
        if (down && !repeat && !typing && key == kKeyMenu) {
            auto& s = Studio::get();
            if (s.rendering) s.stopRender("Stopped by user", false);
            else s.menuOpen = !s.menuOpen;
            return true; // swallow P so the game doesn't also react to it
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
