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

struct PendingInput {
    bool isDown;
    int button;
    bool isPlayer1;
    int ticksDelayed;
};

// Estado global del mod (modo, consola, cola de inputs y flags de simulación)
class BufferState {
public:
    static BufferState& get() {
        static BufferState instance;
        return instance;
    }

    void initConsole() {
#ifdef GEODE_IS_WINDOWS
        if (m_initialized) return;

        AllocConsole();
        SetConsoleTitleA("Dynamic Prediction Input Buffer - Live Console");

        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);

        std::cout << "========================================================\n";
        std::cout << "[InputBuffer] LOADED - Lookahead Prediction Engine Active\n";
        std::cout << "[InputBuffer] Presiona F6 para ciclar: OFF -> 2T -> 4T -> 8T\n";
        std::cout << "========================================================\n\n";
        std::cout << std::flush;

        m_initialized = true;
#endif
        // Restaurar el modo guardado la primera vez
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
        if (m_stage == STAGE_OFF) queue.clear(); // al apagar, no dejamos inputs colgados
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

    void logSavedInput(int ticksDelayed) {
        float msSaved = ticksDelayed * (1000.0f / 240.0f);
        std::cout << "[InputBuffer] SAVED: press retrasado " << ticksDelayed
                  << " ticks (" << msSaved << " ms) para evitar muerte\n" << std::flush;
        log::info("InputBuffer: press retrasado {} ticks ({} ms)", ticksDelayed, msSaved);
    }

    // Cola de inputs pendientes (en orden de llegada)
    std::deque<PendingInput> queue;

    // true mientras estamos en una micro-simulación
    bool isSimulating = false;
    bool simulationDied = false;

    // true mientras soltamos nosotros un input de la cola (evita reentrada en handleButton)
    bool isFlushing = false;

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

// Evita que la simulación mate al jugador de verdad
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

    // Simula 1 tick con el input aplicado y devuelve true si el jugador moriría.
    // Usa un PlayerCheckpoint completo (el mismo que usan los checkpoints del juego)
    // en vez de copiar campos a mano.
    bool testIfInputKills(PlayerObject* player, bool isDown, int button) {
        if (!player || player->m_isDead) return false;

        auto& st = BufferState::get();

        auto* cp = PlayerCheckpoint::create();
        if (!cp) return false;
        cp->retain();
        player->saveToCheckpoint(cp);

        // Guardamos también el estado de pulsación, que el checkpoint no siempre cubre
        bool savedHolding = player->m_isHolding;
        bool savedQueuedHold = player->m_hasJustHeld;

        st.isSimulating = true;
        st.simulationDied = false;

        if (isDown) player->pushButton(static_cast<PlayerButton>(button));
        else player->releaseButton(static_cast<PlayerButton>(button));

        const float stepDt = 1.0f / 240.0f;
        player->update(stepDt);
        this->checkCollisions(player, stepDt, false);

        bool wouldDie = st.simulationDied || player->m_isDead;

        // Restaurar todo
        st.isSimulating = false;
        st.simulationDied = false;
        player->loadFromCheckpoint(cp);
        player->m_isHolding = savedHolding;
        player->m_hasJustHeld = savedQueuedHold;
        player->m_isDead = false;

        cp->release();
        return wouldDie;
    }

    void handleButton(bool isDown, int button, bool isPlayer1) {
        auto& st = BufferState::get();

        // Simulación, modo apagado o nosotros mismos soltando la cola -> comportamiento normal
        if (st.isSimulating || st.isFlushing || !st.isEnabled()) {
            GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
            return;
        }

        // Si ya hay inputs pendientes, todo lo nuevo (press o release) va detrás
        // para mantener el orden y no dejar el botón "pegado"
        if (!st.queue.empty()) {
            st.queue.push_back({ isDown, button, isPlayer1, 0 });
            return;
        }

        PlayerObject* target = isPlayer1 ? m_player1 : m_player2;

        // Press que mataría en este tick -> retrasar
        if (isDown && testIfInputKills(target, true, button)) {
            st.queue.push_back({ isDown, button, isPlayer1, 1 });
            return;
        }

        GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
    }

    // Se llama una vez por tick de física (no una vez por frame)
    // NOTA: en algunas versiones la firma es processCommands(float, bool, bool).
    // Si no compila, ajústala a la de tu binding de Geode.
    void processCommands(float dt) {
        auto& st = BufferState::get();

        if (st.isEnabled() && !st.queue.empty()) {
            int maxTicks = st.getMaxTicks();

            while (!st.queue.empty()) {
                auto& pending = st.queue.front();
                PlayerObject* target = pending.isPlayer1 ? m_player1 : m_player2;

                bool release = !pending.isDown;
                bool canFire = release;

                if (!release) {
                    bool stillKills = testIfInputKills(target, true, pending.button);
                    canFire = !stillKills || pending.ticksDelayed >= maxTicks;
                }

                if (!canFire) {
                    pending.ticksDelayed++;
                    break; // el press sigue esperando; lo de detrás también
                }

                if (!release && pending.ticksDelayed > 0) {
                    st.logSavedInput(pending.ticksDelayed);
                }

                PendingInput p = pending;
                st.queue.pop_front();

                st.isFlushing = true;
                GJBaseGameLayer::handleButton(p.isDown, p.button, p.isPlayer1);
                st.isFlushing = false;
            }
        }

        GJBaseGameLayer::processCommands(dt);
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
        PlayLayer::destroyPlayer(player, object);
    }
};

// F6 para ciclar el modo
class $modify(BufferKeyboardHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && key == enumKeyCodes::KEY_F6) {
            BufferState::get().cycleMode();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
