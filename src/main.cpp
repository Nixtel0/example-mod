#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <deque>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <iostream>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

using namespace geode::prelude;

enum BufferStage {
    STAGE_OFF = 0,
    STAGE_2_TICKS = 1,
    STAGE_4_TICKS = 2,
    STAGE_8_TICKS = 3,
    STAGE_16_TICKS = 4,
    STAGE_32_TICKS = 5
};

static constexpr int STAGE_COUNT = 6;

// Ticks que simula hacia delante el predictor (F7 lo cambia)
static constexpr int LOOKAHEAD_TICKS = 60;
// El predictor asume un toque corto: suelta el boton tras estos ticks
static constexpr int ASSUMED_HOLD_TICKS = 3;
// Dos posiciones X se consideran el mismo tick si difieren menos de esto
static constexpr float SAME_TICK_TOLERANCE = 0.25f;

struct SimResult {
    bool died = false;
    int deathTick = -1;
    float xStart = 0.f, xEnd = 0.f;
    float yStart = 0.f, yEnd = 0.f;
};

struct PendingInput {
    bool isDown;
    int button;
    bool isPlayer1;
    int ticksWaited;   // ticks esperados hasta ahora
    int delayTarget;   // retraso fijo (predictor). Ignorado si learned == true
    bool learned;      // true: esperar hasta la X objetivo / hasta que la X deje de ser "mortal"
    float targetX;     // X del tick seguro (ventana aprendida). -1 si no hay
    float startX;      // X donde se pulso originalmente
};

class BufferState {
public:
    static BufferState& get() {
        static BufferState instance;
        return instance;
    }

    void initConsole() {
#ifdef GEODE_IS_WINDOWS
        if (!m_initialized) {
            AllocConsole();
            SetConsoleTitleA("Dynamic Prediction Input Buffer - Live Console");

            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);

            std::cout << "========================================================\n";
            std::cout << "[InputBuffer] LOADED\n";
            std::cout << "[InputBuffer] F6: modo OFF/2T/4T/8T/16T/32T | F7: lookahead 15/30/60/120/240\n";
            std::cout << "[InputBuffer] F8: debug | F9: predictor ON/OFF | F10: olvidar lo aprendido\n";
            std::cout << "========================================================\n\n";
            std::cout << std::flush;
            m_initialized = true;
        }
#endif
        if (!m_loaded) {
            m_loaded = true;
            int saved = Mod::get()->getSavedValue<int>("stage", static_cast<int>(STAGE_2_TICKS));
            if (saved < 0 || saved >= STAGE_COUNT) saved = static_cast<int>(STAGE_2_TICKS);
            m_stage = static_cast<BufferStage>(saved);
            printStatus();
        }
    }

    void cycleMode() {
        m_stage = static_cast<BufferStage>((m_stage + 1) % STAGE_COUNT);
        Mod::get()->setSavedValue<int>("stage", static_cast<int>(m_stage));
        if (m_stage == STAGE_OFF) queue.clear();
        printStatus();
    }

    int getMaxTicks() const {
        switch (m_stage) {
            case STAGE_2_TICKS: return 2;
            case STAGE_4_TICKS: return 4;
            case STAGE_8_TICKS: return 8;
            case STAGE_16_TICKS: return 16;
            case STAGE_32_TICKS: return 32;
            default: return 0;
        }
    }

    bool isEnabled() const { return m_stage != STAGE_OFF; }

    void cycleLookahead() {
        switch (lookahead) {
            case 15:  lookahead = 30;  break;
            case 30:  lookahead = 60;  break;
            case 60:  lookahead = 120; break;
            case 120: lookahead = 240; break;
            default:  lookahead = 15;  break;
        }
        float ms = lookahead * (1000.0f / 240.0f);
        std::cout << "[InputBuffer] F7 -> lookahead = " << lookahead
                  << " ticks (" << ms << " ms)\n" << std::flush;
    }

    void toggleDebug() {
        debug = !debug;
        std::cout << "[InputBuffer] F8 -> debug " << (debug ? "ON" : "OFF") << "\n" << std::flush;
    }

    void toggleSim() {
        useSim = !useSim;
        std::cout << "[InputBuffer] F9 -> predictor " << (useSim ? "ON" : "OFF") << "\n" << std::flush;
    }

    // --- Aprendizaje a partir de muertes reales ---
    bool isBadX(float x) const {
        for (float b : badX) {
            if (std::fabs(b - x) < SAME_TICK_TOLERANCE) return true;
        }
        return false;
    }

    void addBadX(float x) {
        if (!isBadX(x)) badX.push_back(x);
    }

    void clearLearned() {
        badX.clear();
        windows.clear();
        confirm.valid = false;
        lastPressValid = false;
        std::cout << "[InputBuffer] F10 -> olvidado todo lo aprendido\n" << std::flush;
    }

    void recordPress(float x) {
        lastPressValid = true;
        lastPressX = x;
        lastPressTick = tickCounter;
    }

    // --- Ventanas aprendidas: todo click en [xLow, xSafe) se retrasa hasta xSafe ---
    struct SafeWindow { float xLow; float xSafe; };
    struct PendingConfirm {
        bool valid = false;
        float xSafe = 0.f;
        float dx = 0.f;       // X que avanza por tick
        int firedTick = 0;
        int maxTicks = 0;
    };

    std::vector<SafeWindow> windows;
    PendingConfirm confirm;

    const SafeWindow* findWindow(float x) const {
        for (auto const& w : windows) {
            if (x >= w.xLow - SAME_TICK_TOLERANCE && x < w.xSafe - SAME_TICK_TOLERANCE) return &w;
        }
        return nullptr;
    }

    void addWindow(float xLow, float xSafe) {
        for (auto& w : windows) {
            if (std::fabs(w.xSafe - xSafe) < SAME_TICK_TOLERANCE) {
                w.xLow = std::min(w.xLow, xLow);
                return;
            }
        }
        windows.push_back({ xLow, xSafe });
    }

    void removeWindowsAt(float xSafe) {
        windows.erase(
            std::remove_if(windows.begin(), windows.end(),
                [&](const SafeWindow& w) { return std::fabs(w.xSafe - xSafe) < SAME_TICK_TOLERANCE; }),
            windows.end());
    }

    // Estado
    std::deque<PendingInput> queue;

    bool isSimulating = false;
    bool simulationDied = false;
    bool isFlushing = false;

    bool debug = false;
    bool useSim = true;
    int lookahead = LOOKAHEAD_TICKS;

    float stepDt = 1.0f / 240.0f;
    int debugCalls = 0;
    int tickCounter = 0;

    std::vector<float> badX;
    bool lastPressValid = false;
    float lastPressX = 0.f;
    int lastPressTick = 0;
    std::string currentLevel;

private:
    bool m_initialized = false;
    bool m_loaded = false;
    BufferStage m_stage = STAGE_2_TICKS;

    void printStatus() {
        std::cout << "[InputBuffer] F6 -> ";
        switch (m_stage) {
            case STAGE_OFF:      std::cout << "STAGE 0: OFF\n"; break;
            case STAGE_2_TICKS:  std::cout << "STAGE 1: 2 Ticks (~8.3 ms)\n"; break;
            case STAGE_4_TICKS:  std::cout << "STAGE 2: 4 Ticks (~16.6 ms)\n"; break;
            case STAGE_8_TICKS:  std::cout << "STAGE 3: 8 Ticks (~33.3 ms)\n"; break;
            case STAGE_16_TICKS: std::cout << "STAGE 4: 16 Ticks (~66.7 ms)\n"; break;
            case STAGE_32_TICKS: std::cout << "STAGE 5: 32 Ticks (~133.3 ms)\n"; break;
        }
        std::cout << std::flush;
    }
};

class $modify(PredictivePlayerObject, PlayerObject) {
    void playerDestroyed(bool p0) {
        if (BufferState::get().isSimulating) {
            BufferState::get().simulationDied = true;
            return;
        }
        PlayerObject::playerDestroyed(p0);
    }
};

class $modify(PredictiveBaseLayer, GJBaseGameLayer) {

    // Predictor: simula hacia delante y devuelve si el jugador muere.
    SimResult simulateDies(PlayerObject* player, int delay, int button) {
        SimResult r;
        if (!player || player->m_isDead) return r;

        auto& st = BufferState::get();

        auto* cp = PlayerCheckpoint::create();
        if (!cp) return r;
        cp->retain();
        player->saveToCheckpoint(cp);

        r.xStart = player->getPositionX();
        r.yStart = player->getPositionY();

        st.isSimulating = true;
        st.simulationDied = false;

        const float dt = st.stepDt;
        const int total = delay + st.lookahead;

        for (int t = 0; t < total; ++t) {
            if (t == delay) {
                GJBaseGameLayer::handleButton(true, button, true);
            } else if (t == delay + ASSUMED_HOLD_TICKS) {
                GJBaseGameLayer::handleButton(false, button, true);
            }
            player->update(dt);
            this->checkCollisions(player, dt, false);
            if (st.simulationDied || player->m_isDead) {
                r.died = true;
                r.deathTick = t;
                break;
            }
        }

        r.xEnd = player->getPositionX();
        r.yEnd = player->getPositionY();

        st.isSimulating = false;
        st.simulationDied = false;
        player->loadFromCheckpoint(cp);
        player->m_isDead = false;

        cp->release();
        return r;
    }

    void handleButton(bool isDown, int button, bool isPlayer1) {
        auto& st = BufferState::get();

        if (st.isSimulating || st.isFlushing || !st.isEnabled()) {
            GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
            return;
        }

        // Si ya hay algo pendiente, lo nuevo va detras para mantener el orden
        if (!st.queue.empty()) {
            st.queue.push_back({ isDown, button, isPlayer1, 0, 0, false, -1.f, 0.f });
            return;
        }

        if (isDown && isPlayer1 && m_player1) {
            PlayerObject* target = m_player1;
            const float x = target->getPositionX();
            const int maxTicks = st.getMaxTicks();

            // 1) Predictor (simulacion)
            int safeDelay = 0;
            if (st.useSim) {
                safeDelay = -1;
                for (int d = 0; d <= maxTicks; ++d) {
                    SimResult r = simulateDies(target, d, button);
                    if (st.debug) {
                        std::cout << "    [sim] retraso=" << d
                                  << " muere=" << r.died
                                  << " en_tick=" << r.deathTick
                                  << " x:" << r.xStart << "->" << r.xEnd
                                  << " y:" << r.yStart << "->" << r.yEnd << "\n";
                    }
                    if (!r.died) { safeDelay = d; break; }
                }
            }

            // 2) Lo aprendido de muertes reales: un click en esta X ya te mato antes
            const bool learnedBad = st.isBadX(x);

            if (st.debug) {
                std::cout << "[InputBuffer][DEBUG] PRESS x=" << x
                          << " predictor_retraso=" << safeDelay
                          << " aprendido_mortal=" << learnedBad
                          << " (max " << maxTicks << ")\n" << std::flush;
            }

            if (safeDelay > 0) {
                std::cout << "[InputBuffer] CLICK RETENIDO en x=" << x
                          << " [predictor] (retraso previsto " << safeDelay << " ticks)\n" << std::flush;
                st.queue.push_back({ isDown, button, isPlayer1, 0, safeDelay, false, -1.f, x });
                st.debugCalls = 0;
                return;
            }
            // 3) Ventana aprendida: este click cae antes de un tick seguro conocido
            const auto* win = st.findWindow(x);
            if (win || learnedBad) {
                float targetX = win ? win->xSafe : -1.f;
                std::cout << "[InputBuffer] CLICK RETENIDO en x=" << x
                          << (win ? " [ventana] (hay un tick seguro conocido mas adelante)"
                                  : " [aprendido] (esa posicion te mato antes)") << "\n" << std::flush;
                st.queue.push_back({ isDown, button, isPlayer1, 0, 0, true, targetX, x });
                st.debugCalls = 0;
                return;
            }

            // Pasa directo: guardamos donde se hizo el click para poder aprender si mueres
            st.recordPress(x);
        }

        GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
    }

    // GD 2.2081: se llama una vez por tick de fisica (y por medio tick)
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& st = BufferState::get();

        if (!st.isSimulating) {
            st.stepDt = dt > 0.f ? dt : st.stepDt;
            st.tickCounter++;

            // Si el click retrasado sobrevivio el tiempo suficiente, el tick donde se solto
            // era el bueno: todos los ticks anteriores (hasta el maximo del modo) apuntan a el
            if (st.confirm.valid && st.tickCounter - st.confirm.firedTick > st.lookahead) {
                float xLow = st.confirm.xSafe - st.confirm.maxTicks * st.confirm.dx - SAME_TICK_TOLERANCE;
                st.addWindow(xLow, st.confirm.xSafe);
                std::cout << "[InputBuffer] Aprendido: tick seguro en x=" << st.confirm.xSafe
                          << " -> cualquier click en los " << st.confirm.maxTicks
                          << " ticks anteriores se retrasara hasta ahi\n" << std::flush;
                st.confirm.valid = false;
            }
        }

        if (st.isEnabled() && !st.queue.empty()) {
            if (st.debug && st.debugCalls < 12) {
                std::cout << "[InputBuffer][DEBUG] processCommands dt=" << dt
                          << " half=" << isHalfTick << " last=" << isLastTick
                          << " cola=" << st.queue.size() << "\n" << std::flush;
                st.debugCalls++;
            }

            while (!st.queue.empty()) {
                auto& pending = st.queue.front();

                if (pending.isDown) {
                    pending.ticksWaited++;

                    if (pending.learned) {
                        // Esperar mientras esta X siga marcada como mortal (hasta el maximo del modo)
                        float x = m_player1 ? m_player1->getPositionX() : 0.f;
                        bool waitTarget = pending.targetX >= 0.f && x < pending.targetX - SAME_TICK_TOLERANCE;
                        bool waitBad = st.isBadX(x);
                        if ((waitTarget || waitBad) && pending.ticksWaited < st.getMaxTicks()) break;
                    } else {
                        if (pending.ticksWaited < pending.delayTarget) break;
                    }
                }

                PendingInput p = pending;
                st.queue.pop_front();

                if (p.isDown && p.isPlayer1 && m_player1) {
                    const float fx = m_player1->getPositionX();
                    st.recordPress(fx);
                    if (p.ticksWaited > 0) {
                        float ms = p.ticksWaited * (1000.0f / 240.0f);
                        std::cout << "[InputBuffer] CLICK DEVUELTO AL JUEGO en x=" << fx
                                  << " tras " << p.ticksWaited << " ticks (" << ms << " ms)"
                                  << (p.targetX >= 0.f ? " [ventana]" : (p.learned ? " [aprendido]" : " [predictor]"))
                                  << "\n" << std::flush;

                        // Pendiente de confirmar que sobrevives; si es asi se aprende la ventana
                        float dx = (fx - p.startX) / static_cast<float>(p.ticksWaited);
                        if (dx < 0.05f) dx = 1.3f;
                        st.confirm.valid = true;
                        st.confirm.xSafe = fx;
                        st.confirm.dx = dx;
                        st.confirm.firedTick = st.tickCounter;
                        st.confirm.maxTicks = st.getMaxTicks();
                    }
                }

                st.isFlushing = true;
                GJBaseGameLayer::handleButton(p.isDown, p.button, p.isPlayer1);
                st.isFlushing = false;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(PredictivePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto& st = BufferState::get();
        st.initConsole();
        st.queue.clear();
        st.lastPressValid = false;

        // Si cambias de nivel, se olvida lo aprendido del anterior
        std::string name = level ? std::string(level->m_levelName) : std::string();
        if (name != st.currentLevel) {
            st.badX.clear();
            st.windows.clear();
            st.currentLevel = name;
        }
        return true;
    }

    void resetLevel() {
        auto& st = BufferState::get();
        st.queue.clear();
        st.isSimulating = false;
        st.simulationDied = false;
        st.isFlushing = false;
        st.lastPressValid = false;
        st.confirm.valid = false;
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& st = BufferState::get();

        if (st.isSimulating) {
            st.simulationDied = true;
            return;
        }

        // Muerte real: si ocurrio poco despues de un click, ese click en esa X era mortal
        if (st.isEnabled() && player == m_player1 && st.confirm.valid) {
            // Moriste antes de confirmar: ese tick no era seguro
            st.removeWindowsAt(st.confirm.xSafe);
            st.confirm.valid = false;
        }

        if (st.isEnabled() && player == m_player1 && st.lastPressValid) {
            int ticksSincePress = st.tickCounter - st.lastPressTick;
            if (ticksSincePress <= st.lookahead) {
                st.addBadX(st.lastPressX);
                st.removeWindowsAt(st.lastPressX); // si fue un tick "seguro" aprendido, ya no lo es
                std::cout << "[InputBuffer] Aprendido: click en x=" << st.lastPressX
                          << " te mato " << ticksSincePress
                          << " ticks despues -> la proxima vez se retrasara\n" << std::flush;
            }
            st.lastPressValid = false;
        }

        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(BufferKeyboardHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat) {
            if (key == enumKeyCodes::KEY_F6) BufferState::get().cycleMode();
            else if (key == enumKeyCodes::KEY_F7) BufferState::get().cycleLookahead();
            else if (key == enumKeyCodes::KEY_F8) BufferState::get().toggleDebug();
            else if (key == enumKeyCodes::KEY_F9) BufferState::get().toggleSim();
            else if (key == enumKeyCodes::KEY_F10) BufferState::get().clearLearned();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
