// Macro Studio - macro recorder / player for Geometry Dash (Geode)
//
// Press P to open or close the menu.
//   Record : saves every click/release (both players, platformer buttons) per physics frame
//   Play   : replays the macro by injecting those inputs on the exact frames
// Macros are stored as .gdr2 files in the mod's config folder (macros/).

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/utils/file.hpp>
#include <imgui-cocos.hpp>
#include <gdr/gdr.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string_view>
#include <unordered_map>

using namespace geode::prelude;

// ---------------------------------------------------------------- keybind
static constexpr enumKeyCodes kKeyMenu = KEY_P;   // opens / closes the menu

// ------------------------------------------------------------ macro types
using MInput = gdr::Input<>;

struct MacroReplay : gdr::Replay<MacroReplay, MInput> {
    MacroReplay() : Replay("Macro Studio", 1) {}
};

enum class Mode { Idle, Record, Play };
enum class Tab { Macro, Settings, Library };

struct Injection {
    uint64_t frame;
    bool down;
    int button;
    bool p1;
};

// ------------------------------------------------------------------ colors
static ImVec4 const kAccent(0.47f, 0.36f, 0.98f, 1.f);
static ImVec4 const kRed(0.93f, 0.28f, 0.33f, 1.f);
static ImVec4 const kGreen(0.20f, 0.78f, 0.52f, 1.f);
static ImVec4 const kCard(0.14f, 0.15f, 0.19f, 1.f);
static ImVec4 const kCardHover(0.20f, 0.21f, 0.27f, 1.f);

// ------------------------------------------------------------------- state
struct Studio {
    Mode mode = Mode::Idle;
    Tab tab = Tab::Macro;
    bool menuOpen = false;

    // settings (saved between sessions)
    bool autoSave = true;
    bool practiceFix = true;
    bool ignoreInput = true;
    bool overlay = true;
    bool overlayDetails = true;
    bool notify = true;
    float speed = 1.f;

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
    }

    void resetSettings() {
        autoSave = practiceFix = ignoreInput = overlay = overlayDetails = notify = true;
        speed = 1.f;
        applySpeed();
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
        macro = MacroReplay();
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
            toast("Playing");
            restart();
        } else if (!wasRecording) {
            status = "Ready";
        }
    }

    // --------------------------------------------------- game-side events
    // Returns false to drop the input.
    bool onInput(GJBaseGameLayer* gl, bool down, int button, bool p1) {
        if (injecting || !isPlayLayer(gl)) return true;
        if (mode == Mode::Play) return !ignoreInput;
        if (mode == Mode::Record) {
            macro.inputs.emplace_back(frame(), static_cast<uint8_t>(button), !p1, down);
        }
        return true;
    }

    // Called at the start of every physics step.
    void beginStep(GJBaseGameLayer* gl) {
        if (mode != Mode::Play || !isPlayLayer(gl)) return;
        auto f = frame();
        auto& in = macro.inputs;
        auto it = std::lower_bound(in.begin(), in.end(), f,
            [](MInput const& a, uint64_t v) { return a.frame < v; });
        for (; it != in.end() && it->frame == f; ++it) {
            Injection inj{ f, it->down, static_cast<int>(it->button), !it->player2 };
            injecting = true;
            gl->handleButton(inj.down, inj.button, inj.p1);
            injecting = false;
        }
    }

    void onLevelReset(PlayLayer* pl) {
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
        if (mode != Mode::Record) return;
        finishRecording();
        mode = Mode::Idle;
        toast("Recording finished");
    }

    void onQuit() {
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
    ImGui::SetNextWindowSize(ImVec2(400.f, 500.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Macro Studio", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    float gap = ImGui::GetStyle().ItemSpacing.x;
    float w3 = (ImGui::GetContentRegionAvail().x - gap * 2.f) / 3.f;
    tabButton("Macro", Tab::Macro, w3);
    ImGui::SameLine();
    tabButton("Settings", Tab::Settings, w3);
    ImGui::SameLine();
    tabButton("Library", Tab::Library, w3);
    ImGui::Spacing();

    switch (s.tab) {
        case Tab::Macro: drawMacroTab(); break;
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
        if (Studio::get().menuOpen) drawWindow();
        drawOverlay();
    });
}

$on_mod(DataSaved) {
    Studio::get().saveSettings();
}



// ------------------------------------------------------------------- hooks
class $modify(StudioBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick) Studio::get().beginStep(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
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

class $modify(StudioKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
        if (down && !repeat && !typing && key == kKeyMenu) {
            auto& s = Studio::get();
            s.menuOpen = !s.menuOpen;
            return true; // swallow P so the game doesn't also react to it
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
