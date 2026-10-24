// Raptor Bot - Liveplay assist for Geometry Dash (Geode)
//
// Liveplay: you play normally. When one of your clicks lands inside the early
// buffer window of a click from the loaded .gdr2 replay, the click is put "on
// probation" and a checkpoint is taken just before it is applied. If you die
// while it is on probation, the death is cancelled, the game rewinds to that
// checkpoint, your click is nullified and the replay's click is registered at
// the replay's frame instead.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/utils/file.hpp>
#include <imgui-cocos.hpp>
#include <gdr/gdr.hpp>

#include <algorithm>
#include <deque>
#include <optional>

using namespace geode::prelude;

// ---------------------------------------------------------------- keybinds
static constexpr enumKeyCodes kKeyLiveplay = KEY_L;   // toggles Liveplay mode
static constexpr enumKeyCodes kKeyMenu     = KEY_F8;  // shows / hides the window

// ------------------------------------------------------------ replay types
using RInput = gdr::Input<>;

struct RaptorReplay : gdr::Replay<RaptorReplay, RInput> {
    RaptorReplay() : Replay("Raptor Bot", 1) {}
};

enum class Mode { Disabled, Record, Playback, Liveplay };

struct Injection {
    uint64_t frame;
    bool down;
    int button;
    bool p1;
};

// A player click that was matched to a replay click and is being watched.
struct Probation {
    Ref<CheckpointObject> checkpoint; // state right before the click was applied
    uint64_t clickFrame;              // frame the player clicked on
    size_t replayIdx;                 // matched replay input
    Injection substitute;             // the replay's click, used if the player's kills
};

// ------------------------------------------------------------------- state
struct Bot {
    // ---- UI settings (same order as the window)
    Mode mode = Mode::Disabled;
    bool ignorePlayerInputs = true;
    bool hud = true;
    bool markers = true;
    bool releaseMarkers = true;
    int  lookahead = 240;
    bool assistClicks = true;
    bool assistWindows = true;
    int  earlyBuffer = 30;
    bool smartNearest = true;
    char path[512] = {};
    std::string status = "Ready";
    bool menuOpen = true;

    // ---- runtime
    RaptorReplay replay;
    std::vector<int64_t> consumedAt;   // per replay input: frame it was matched on, -1 = free
    std::deque<Probation> probation;
    std::vector<Injection> queue;
    std::optional<Probation> rewind;
    bool injecting = false;
    int assisted = 0;

    // A click stays on probation until its replay frame + this many frames.
    static constexpr uint64_t kSettleFrames = 30;
    static constexpr size_t kMaxProbation = 8;

    static Bot& get() {
        static Bot b;
        return b;
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

    void sync() { consumedAt.resize(replay.inputs.size(), -1); }

    char const* modeName() const {
        switch (mode) {
            case Mode::Record:   return "Record";
            case Mode::Playback: return "Playback";
            case Mode::Liveplay: return "Liveplay";
            default:             return "Disabled";
        }
    }

    std::string nextText() {
        sync();
        auto const& in = replay.inputs;
        auto cur = frame();
        auto it = std::lower_bound(in.begin(), in.end(), cur,
            [](RInput const& a, uint64_t v) { return a.frame < v; });
        for (size_t i = it - in.begin(); i < in.size(); ++i) {
            if (mode == Mode::Liveplay && consumedAt[i] >= 0) continue;
            return fmt::format("{} {} (+{})", in[i].frame, in[i].down ? "down" : "up", in[i].frame - cur);
        }
        return "none";
    }

    void clearRun() {
        queue.clear();
        probation.clear();
        rewind.reset();
    }

    void resetRun() {
        sync();
        clearRun();
        auto f = static_cast<int64_t>(frame());
        for (auto& c : consumedAt) if (c >= f) c = -1;
    }

    void setMode(Mode m) {
        mode = m;
        assisted = 0;
        resetRun();
        status = (m == Mode::Record) ? "Recording: press Restart to start a fresh take" : "Ready";
    }

    void inject(GJBaseGameLayer* gl, Injection const& i) {
        injecting = true;
        gl->handleButton(i.down, i.button, i.p1);
        injecting = false;
    }

    // ------------------------------------------------------- liveplay core
    // Finds the replay input a player click should be paired with.
    std::optional<size_t> findMatch(uint64_t f, bool down, int button, bool p1) {
        auto const& in = replay.inputs;
        auto it = std::lower_bound(in.begin(), in.end(), f,
            [](RInput const& a, uint64_t v) { return a.frame < v; });
        for (size_t i = it - in.begin(); i < in.size(); ++i) {
            if (consumedAt[i] >= 0) continue;
            if (in[i].frame > f + static_cast<uint64_t>(earlyBuffer)) break;
            if (in[i].down == down && in[i].button == button && in[i].player2 == !p1) return i;
            // Smart nearest: skip inputs of another type and keep looking inside the
            // window. Otherwise only the very next free input may be matched.
            if (!smartNearest) break;
        }
        return std::nullopt;
    }

    void trackClick(PlayLayer* pl, bool down, int button, bool p1) {
        auto f = frame();
        auto idx = findMatch(f, down, button, p1);
        if (!idx) return;

        auto const& in = replay.inputs[*idx];
        consumedAt[*idx] = static_cast<int64_t>(f);
        if (in.frame == f) return; // already on the replay's frame, nothing to fix

        auto* cp = pl->createCheckpoint(); // taken BEFORE the click is applied
        if (!cp) return;
        probation.push_back(Probation{
            Ref<CheckpointObject>(cp), f, *idx, Injection{ in.frame, down, button, p1 }
        });
        if (probation.size() > kMaxProbation) probation.pop_front();
    }

    // Called from the GJBaseGameLayer::handleButton hook. false = drop the input.
    bool onPlayerInput(GJBaseGameLayer* gl, bool down, int button, bool p1) {
        if (injecting || !isPlayLayer(gl)) return true;
        auto* pl = PlayLayer::get();
        sync();
        switch (mode) {
            case Mode::Playback:
                return !ignorePlayerInputs;
            case Mode::Record:
                replay.inputs.emplace_back(frame(), static_cast<uint8_t>(button), !p1, down);
                return true;
            case Mode::Liveplay:
                if (assistClicks) trackClick(pl, down, button, p1);
                return true;
            default:
                return true;
        }
    }

    // Called from the PlayLayer::destroyPlayer hook. true = swallow the death.
    bool tryRescue(PlayLayer* pl, GameObject* obj) {
        if (mode != Mode::Liveplay || !assistClicks) return false;
        if (obj && obj == pl->m_anticheatSpike) return false;
        if (rewind) return true;            // already rewinding, ignore extra hits
        if (probation.empty()) return false; // nothing to blame, die normally
        rewind = std::move(probation.back()); // blame the most recent click
        probation.pop_back();
        return true;
    }

    void performRewind(PlayLayer* pl) {
        auto p = std::move(*rewind);
        rewind.reset();

        pl->loadFromCheckpoint(p.checkpoint);
        pl->m_gameState.m_currentProgress = static_cast<int>(p.clickFrame);

        // The timeline after the click was overwritten: forget everything matched after it.
        for (auto& c : consumedAt) if (c >= static_cast<int64_t>(p.clickFrame)) c = -1;
        if (p.replayIdx < consumedAt.size()) consumedAt[p.replayIdx] = static_cast<int64_t>(p.clickFrame);
        std::erase_if(probation, [&](Probation const& o) { return o.clickFrame >= p.clickFrame; });

        queue.clear();
        queue.push_back(p.substitute); // the passable click, fired on the replay's frame
        ++assisted;
        status = fmt::format("Fixed click: frame {} -> {}", p.clickFrame, p.substitute.frame);
    }

    // Called at the start of every physics step.
    void beginStep(GJBaseGameLayer* gl) {
        if (mode == Mode::Disabled || mode == Mode::Record || !isPlayLayer(gl)) return;
        auto* pl = PlayLayer::get();
        sync();

        if (rewind) performRewind(pl);
        auto f = frame();

        if (mode == Mode::Liveplay) {
            std::erase_if(probation, [&](Probation const& p) {
                return f > p.substitute.frame + kSettleFrames;
            });
            for (size_t i = 0; i < queue.size();) {
                if (queue[i].frame <= f) {
                    auto inj = queue[i];
                    queue.erase(queue.begin() + i);
                    inject(gl, inj);
                } else ++i;
            }
        } else { // Playback
            auto& in = replay.inputs;
            auto it = std::lower_bound(in.begin(), in.end(), f,
                [](RInput const& a, uint64_t v) { return a.frame < v; });
            for (; it != in.end() && it->frame == f; ++it) {
                inject(gl, Injection{ f, it->down, static_cast<int>(it->button), !it->player2 });
            }
        }
    }

    void onLevelReset(PlayLayer* pl) {
        if (mode == Mode::Record && !pl->m_isPracticeMode) replay.inputs.clear();
        resetRun();
    }

    // ----------------------------------------------------------- buttons
    void applyAssistProfile() {
        assistClicks = assistWindows = smartNearest = true;
        hud = markers = releaseMarkers = true;
        lookahead = 240;
        earlyBuffer = 30;
        status = "Liveplay assist profile applied";
    }

    void newClear() {
        replay = RaptorReplay();
        consumedAt.clear();
        resetRun();
        status = "Replay cleared";
    }

    void save() {
        auto& in = replay.inputs;
        std::stable_sort(in.begin(), in.end(), [](RInput const& a, RInput const& b) { return a.frame < b.frame; });
        replay.duration = in.empty() ? 0.f : static_cast<float>(in.back().frame / replay.framerate);
        replay.gameVersion = GEODE_COMP_GD_VERSION;
        replay.author = std::string(GJAccountManager::get()->m_username);
        if (auto* pl = PlayLayer::get()) {
            replay.platformer = pl->m_levelSettings->m_platformerMode;
            replay.levelInfo.id = static_cast<uint32_t>(pl->m_level->m_levelID.value());
            replay.levelInfo.name = std::string(pl->m_level->m_levelName);
        }

        auto data = replay.exportData();
        if (data.isErr()) { status = "Save failed: " + data.unwrapErr(); return; }

        std::filesystem::path p = path;
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        auto w = utils::file::writeBinary(p, data.unwrap());
        status = w.isOk() ? fmt::format("Saved {} inputs", in.size()) : "Save failed: " + w.unwrapErr();
    }

    void load() {
        auto bytes = utils::file::readBinary(std::filesystem::path(path));
        if (bytes.isErr()) { status = "Load failed: " + bytes.unwrapErr(); return; }

        auto r = RaptorReplay::importData(bytes.unwrap());
        if (r.isErr()) { status = "Load failed: " + r.unwrapErr(); return; }

        replay = r.unwrap();
        // GDR2 stores P1 inputs then P2 inputs, so sorting after import is required.
        std::stable_sort(replay.inputs.begin(), replay.inputs.end(),
            [](RInput const& a, RInput const& b) { return a.frame < b.frame; });
        consumedAt.assign(replay.inputs.size(), -1);
        resetRun();
        status = fmt::format("Loaded {} inputs", replay.inputs.size());
    }

    void restart() {
        if (auto* pl = PlayLayer::get()) pl->resetLevel();
        else status = "Open a level first";
    }
};

// -------------------------------------------------------------------- UI
static void drawWindow() {
    auto& b = Bot::get();

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(365, 455), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Raptor Bot")) { ImGui::End(); return; }

    int m = static_cast<int>(b.mode);
    bool changed = false;
    changed |= ImGui::RadioButton("Disabled", &m, 0); ImGui::SameLine();
    changed |= ImGui::RadioButton("Record",   &m, 1); ImGui::SameLine();
    changed |= ImGui::RadioButton("Playback", &m, 2); ImGui::SameLine();
    changed |= ImGui::RadioButton("Liveplay", &m, 3);
    if (changed) b.setMode(static_cast<Mode>(m));

    ImGui::Text("State: %s", b.modeName());
    ImGui::Text("Frame: %llu", static_cast<unsigned long long>(b.frame()));
    ImGui::Text("Inputs: %zu", b.replay.inputs.size());
    ImGui::Text("Replay FPS: %.3f", b.replay.framerate);
    ImGui::Text("Next: %s", b.nextText().c_str());
    ImGui::Text("Queued: %zu", b.queue.size());

    if (ImGui::Button("Liveplay assist profile")) b.applyAssistProfile();
    ImGui::SameLine();
    if (ImGui::Button("Clear queue")) { b.queue.clear(); b.probation.clear(); }

    ImGui::Checkbox("Ignore player inputs during playback", &b.ignorePlayerInputs);
    ImGui::Checkbox("Liveplay HUD", &b.hud);
    ImGui::Checkbox("Render replay markers", &b.markers);
    ImGui::Checkbox("Show release markers", &b.releaseMarkers);
    ImGui::SliderInt("Marker lookahead", &b.lookahead, 1, 960, "%d frames");
    ImGui::Checkbox("Assist clicks in liveplay", &b.assistClicks);
    ImGui::Checkbox("Render assist windows", &b.assistWindows);
    ImGui::SliderInt("Early buffer window", &b.earlyBuffer, 0, 60, "%d frames");
    ImGui::Checkbox("Smart nearest windows", &b.smartNearest);

    ImGui::Separator();
    ImGui::InputText("Replay path", b.path, sizeof(b.path));
    if (ImGui::Button("New/Clear")) b.newClear();
    ImGui::SameLine();
    if (ImGui::Button("Save")) b.save();
    ImGui::SameLine();
    if (ImGui::Button("Load")) b.load();
    ImGui::SameLine();
    if (ImGui::Button("Restart")) b.restart();

    ImGui::Separator();
    ImGui::TextUnformatted(b.status.c_str());

    ImGui::End();
}

// Timeline along the bottom of the screen: "now" on the left, upcoming replay
// inputs to the right, shaded boxes = the early buffer window of each input.
static void drawHud() {
    auto& b = Bot::get();
    if (!PlayLayer::get()) return;
    if (b.mode != Mode::Liveplay && b.mode != Mode::Playback) return;
    b.sync();

    bool live = b.mode == Mode::Liveplay;
    auto const& in = b.replay.inputs;
    uint64_t cur = b.frame();
    uint64_t span = static_cast<uint64_t>(std::max(b.lookahead, 1));

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float w = ds.x * 0.5f;
    float x0 = (ds.x - w) * 0.5f;
    float y = ds.y - 60.f;
    auto* dl = ImGui::GetBackgroundDrawList();

    auto xOf = [&](uint64_t f) { return x0 + w * static_cast<float>(f - cur) / static_cast<float>(span); };

    if (b.markers || (live && b.assistWindows)) {
        dl->AddRectFilled(ImVec2(x0 - 4, y - 14), ImVec2(x0 + w + 4, y + 14), IM_COL32(0, 0, 0, 110), 4.f);
        dl->AddLine(ImVec2(x0, y - 16), ImVec2(x0, y + 16), IM_COL32(255, 255, 255, 255), 2.f);

        auto it = std::lower_bound(in.begin(), in.end(), cur,
            [](RInput const& a, uint64_t v) { return a.frame < v; });
        for (; it != in.end() && it->frame <= cur + span; ++it) {
            size_t i = it - in.begin();
            if (!it->down && !b.releaseMarkers) continue;

            bool used = live && b.consumedAt[i] >= 0;
            float x = xOf(it->frame);

            if (live && b.assistWindows && !used) {
                uint64_t early = static_cast<uint64_t>(b.earlyBuffer);
                uint64_t ws = std::max<uint64_t>(it->frame >= early ? it->frame - early : 0, cur);
                dl->AddRectFilled(ImVec2(xOf(ws), y - 10), ImVec2(x, y + 10),
                    it->down ? IM_COL32(80, 255, 120, 55) : IM_COL32(255, 170, 60, 55));
            }
            if (b.markers) {
                ImU32 col = used ? IM_COL32(130, 130, 130, 200)
                          : it->down ? IM_COL32(80, 255, 120, 255) : IM_COL32(255, 170, 60, 255);
                float h = it->down ? 14.f : 9.f;
                dl->AddLine(ImVec2(x, y - h), ImVec2(x, y + h), col, 2.f);
            }
        }
    }

    if (live && b.hud) {
        auto txt = fmt::format("LIVEPLAY  frame {}  next {}  queued {}  fixed {}{}",
            cur, b.nextText(), b.queue.size(), b.assisted,
            b.probation.empty() ? "" : "  [watching click]");
        dl->AddText(ImVec2(x0, y - 40), IM_COL32(255, 255, 255, 230), txt.c_str());
    }
}

$on_mod(Loaded) {
    auto& b = Bot::get();
    auto def = (Mod::get()->getConfigDir() / "replay.gdr2").string();
    std::snprintf(b.path, sizeof(b.path), "%s", def.c_str());

    ImGuiCocos::get().setup([] {
        // default Dear ImGui dark style + default font, same look as the screenshot
    }).draw([] {
        if (Bot::get().menuOpen) drawWindow();
        drawHud();
    });
}

// ----------------------------------------------------------------- hooks
class $modify(RaptorBaseGameLayer, GJBaseGameLayer) {
        void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick) Bot::get().beginStep(this);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!Bot::get().onPlayerInput(this, down, button, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(RaptorPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (Bot::get().tryRescue(this, object)) return; // death cancelled, rewind pending
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Bot::get().onLevelReset(this);
    }

    void onQuit() {
        Bot::get().clearRun(); // release checkpoints before the layer goes away
        PlayLayer::onQuit();
    }
};

class $modify(RaptorKeys, CCKeyboardDispatcher) {
    // If your bindings declare dispatchKeyboardMSG without the trailing double, drop it.
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        bool typing = ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
        if (down && !repeat && !typing) {
            auto& b = Bot::get();
            if (key == kKeyLiveplay) {
                b.setMode(b.mode == Mode::Liveplay ? Mode::Disabled : Mode::Liveplay);
                Notification::create(
                    b.mode == Mode::Liveplay ? "Raptor Bot: Liveplay ON" : "Raptor Bot: Liveplay OFF",
                    NotificationIcon::Success, 0.8f
                )->show();
            } else if (key == kKeyMenu) {
                b.menuOpen = !b.menuOpen;
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
