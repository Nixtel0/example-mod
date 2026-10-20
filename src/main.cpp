#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <deque>
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
    STAGE_8_TICKS = 3
};

// Cuantos ticks hacia delante simulamos tras el press para ver si muere.
// Un salto que cae en un spike tarda bastantes ticks en matar, por eso 1 tick no bastaba.
static constexpr int LOOKAHEAD_TICKS = 60;
// Asumimos que el press es un toque corto: se suelta tras estos ticks en la simulacion.
static constexpr int ASSUMED_HOLD_TICKS = 3;

struct SimResult {
    bool died = false;
    int deathTick = -1;   // tick (desde el inicio de la simulacion) en el que muere
    float xStart = 0.f;
    float xEnd = 0.f;
    float yStart = 0.f;
    float yEnd = 0.f;
};

struct PendingInput {
    bool isDown;
    int button;
    bool isPlayer1;
    int ticksWaited;   // ticks esperados hasta ahora
    int delayTarget;   // ticks que hay que esperar antes de soltar este input (solo presses)
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
            std::cout << "[InputBuffer] LOADED - Lookahead Prediction Engine Active\n";
            std::cout << "[InputBuffer] F6: ciclar OFF -> 2T -> 4T -> 8T | F7: lookahead 15/30/60/120/240 ticks\n";
            std::cout << "========================================================\n\n";
            std::cout << std::flush;
            m_initialized = true;
        }
#endif
        if (!m_loaded) {
            m_loaded = true;
            int saved = Mod::get()->getSavedValue<int>("stage", static_cast<int>(STAGE_2_TICKS));
            if (saved < 0 || saved > 3) saved = static_cast<int>(STAGE_2_TICKS);
            m_stage = static_cast<BufferStage>(saved);
            printStatus();
        }
    }

    void cycleMode() {
        m_stage = static_cast<BufferStage>((m_stage + 1) % 4);
        Mod::get()->setSavedValue<int>("stage", static_cast<int>(m_stage));
        if (m_stage == STAGE_OFF) queue.clear();
        printStatus();
    }

    int getMaxTicks() const {
        switch (m_stage) {
            case STAGE_2_TICKS: return 2;
            case STAGE_4_TICKS: return 4;
            case STAGE_8_TICKS: return 8;
            default: return 0;
        }
    }

    bool isEnabled() const { return m_stage != STAGE_OFF; }

    std::deque<PendingInput> queue;

    bool isSimulating = false;
    bool simulationDied = false;
    bool isFlushing = false;

    // Ticks que se simulan hacia delante tras el press (F7 los cambia: 15 / 30 / 60 / 120 / 240)
    int lookahead = LOOKAHEAD_TICKS;

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

    // dt real de un tick de fisica (se actualiza desde processCommands)
    float stepDt = 1.0f / 240.0f;
    int debugCalls = 0; // para loguear las primeras llamadas a processCommands tras un retraso

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

    // Simula hacia delante: espera `delay` ticks, hace el press, y avanza hasta
    // LOOKAHEAD_TICKS mas. Devuelve si el jugador muere y datos de depuracion.
    // Al terminar restaura el estado del jugador con un PlayerCheckpoint.
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
                player->pushButton(static_cast<PlayerButton>(button));
            } else if (t == delay + ASSUMED_HOLD_TICKS) {
                player->releaseButton(static_cast<PlayerButton>(button));
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
            st.queue.push_back({ isDown, button, isPlayer1, 0, 0 });
            return;
        }

        // Solo gestionamos al jugador 1 (el jugador 2 inactivo estaba en x=0 y daba basura)
        if (isDown && isPlayer1 && m_player1) {
            PlayerObject* target = m_player1;
            int maxTicks = st.getMaxTicks();

            std::cout << "[InputBuffer][DEBUG] PRESS btn=" << button
                      << " x=" << target->getPositionX()
                      << " y=" << target->getPositionY()
                      << " dead=" << target->m_isDead
                      << " dt=" << st.stepDt << "\n";

            // Buscamos el menor retraso d (0..max) con el que NO mueres
            int safeDelay = -1;
            for (int d = 0; d <= maxTicks; ++d) {
                SimResult r = simulateDies(target, d, button);
                std::cout << "    [sim] retraso=" << d
                          << " muere=" << r.died
                          << " en_tick=" << r.deathTick
                          << " x:" << r.xStart << "->" << r.xEnd
                          << " y:" << r.yStart << "->" << r.yEnd << "\n";
                if (!r.died) {
                    safeDelay = d;
                    break;
                }
            }
            std::cout << "    => retraso_seguro=" << safeDelay << " (max " << maxTicks << ")\n" << std::flush;

            if (safeDelay > 0) {
                float ms = safeDelay * (1000.0f / 240.0f);
                std::cout << "[InputBuffer] SAVED: press retrasado " << safeDelay
                          << " ticks (" << ms << " ms)\n" << std::flush;
                st.queue.push_back({ isDown, button, isPlayer1, 0, safeDelay });
                st.debugCalls = 0;
                return;
            }
            // safeDelay == 0 -> ya es seguro; -1 -> ningun retraso salva: no tocamos nada
        }

        GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
    }

    // GD 2.2081: se llama una vez por tick de fisica
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& st = BufferState::get();

        if (!st.isSimulating) st.stepDt = dt > 0.f ? dt : st.stepDt;

        if (st.isEnabled() && !st.queue.empty()) {
            if (st.debugCalls < 12) {
                std::cout << "[InputBuffer][DEBUG] processCommands dt=" << dt
                          << " half=" << isHalfTick << " last=" << isLastTick
                          << " cola=" << st.queue.size() << "\n" << std::flush;
                st.debugCalls++;
            }

            while (!st.queue.empty()) {
                auto& pending = st.queue.front();

                if (pending.isDown) {
                    pending.ticksWaited++;
                    if (pending.ticksWaited < pending.delayTarget) break; // aun toca esperar
                }

                PendingInput p = pending;
                st.queue.pop_front();

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
        BufferState::get().initConsole();
        BufferState::get().queue.clear();
        return true;
    }

    void resetLevel() {
        auto& st = BufferState::get();
        st.queue.clear();
        st.isSimulating = false;
        st.simulationDied = false;
        st.isFlushing = false;
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (BufferState::get().isSimulating) {
            BufferState::get().simulationDied = true;
            return;
        }
        if (player == m_player1) {
            std::cout << "[InputBuffer][DEBUG] MUERTE REAL x=" << player->getPositionX()
                      << " y=" << player->getPositionY() << "\n" << std::flush;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(BufferKeyboardHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && key == enumKeyCodes::KEY_F6) {
            BufferState::get().cycleMode();
        }
        if (isKeyDown && !isKeyRepeat && key == enumKeyCodes::KEY_F7) {
            BufferState::get().cycleLookahead();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
