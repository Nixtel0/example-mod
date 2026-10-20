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

class ConsoleManager {
public:
    static ConsoleManager& get() {
        static ConsoleManager instance;
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
        std::cout << "[InputBuffer] Initial Mode: STAGE 1 (2 Ticks / ~8.3ms)\n";
        std::cout << "[InputBuffer] Presiona F6 para ciclar: OFF -> 2T -> 4T -> 8T\n";
        std::cout << "========================================================\n\n";
        std::cout << std::flush;

        m_initialized = true;
#endif
    }

    void cycleMode() {
        m_currentStage = static_cast<BufferStage>((m_currentStage + 1) % 4);
        printStatus();
    }

    int getMaxTicks() const {
        switch (m_currentStage) {
            case STAGE_2_TICKS: return 2;
            case STAGE_4_TICKS: return 4;
            case STAGE_8_TICKS: return 8;
            case STAGE_OFF:
            default: return 0;
        }
    }

    bool isEnabled() const {
        return m_currentStage != STAGE_OFF;
    }

    void logSavedInput(int ticksDelayed, bool isPress) {
        float msSaved = ticksDelayed * (1000.0f / 240.0f);
        std::cout << "[InputBuffer] SAVED " << ticksDelayed << " TICKS (" 
                  << msSaved << " ms) | " 
                  << (isPress ? "PRESS" : "RELEASE") 
                  << " delayed to avoid death\n" << std::flush;
    }

    // Bandera global para saber si estamos ejecutando una micro-simulación
    bool isSimulating = false;
    bool simulationDied = false;

private:
    bool m_initialized = false;
    BufferStage m_currentStage = STAGE_2_TICKS;

    void printStatus() {
        std::cout << "[InputBuffer] F6 -> ";
        switch (m_currentStage) {
            case STAGE_OFF:
                std::cout << "STAGE 0: OFF (Sin asistencia)\n";
                break;
            case STAGE_2_TICKS:
                std::cout << "STAGE 1: 2 Ticks (~8.3 ms buffer)\n";
                break;
            case STAGE_4_TICKS:
                std::cout << "STAGE 2: 4 Ticks (~16.6 ms buffer)\n";
                break;
            case STAGE_8_TICKS:
                std::cout << "STAGE 3: 8 Ticks (~33.3 ms buffer)\n";
                break;
        }
        std::cout << std::flush;
    }
};

struct PendingInput {
    bool isDown;
    int button;
    bool isPlayer1;
    int ticksDelayed;
};

// Hook sobre PlayerObject para evitar que muera visual/físicamente durante la predicción
class $modify(PredictivePlayerObject, PlayerObject) {
    void playerDestroyed(bool p0) {
        if (ConsoleManager::get().isSimulating) {
            ConsoleManager::get().simulationDied = true;
            return;
        }
        PlayerObject::playerDestroyed(p0);
    }
};

// Hook principal de físicas y gestión de inputs
class $modify(PredictiveBaseLayer, GJBaseGameLayer) {
    struct Fields {
        std::deque<PendingInput> queue;
    };

    struct FullPlayerState {
        cocos2d::CCPoint pos;
        double yVel;
        float rot;
        bool isDead;
        bool isOnGround;
        bool isDashing;
    };

    FullPlayerState capturePlayerState(PlayerObject* p) {
        return {
            p->getPosition(),
            p->m_yVelocity,
            p->getRotation(),
            p->m_isDead,
            p->m_isOnGround,
            p->m_isDashing
        };
    }

    void restorePlayerState(PlayerObject* p, const FullPlayerState& s) {
        p->setPosition(s.pos);
        p->m_yVelocity = s.yVel;
        p->setRotation(s.rot);
        p->m_isDead = s.isDead;
        p->m_isOnGround = s.isOnGround;
        p->m_isDashing = s.isDashing;
    }

    bool testIfInputKills(PlayerObject* player, bool isDown, int button) {
        if (!player || player->m_isDead) return false;

        FullPlayerState state = capturePlayerState(player);
        ConsoleManager::get().isSimulating = true;
        ConsoleManager::get().simulationDied = false;

        // 1. Aplicamos el salto tentativo
        if (isDown) {
            player->pushButton(static_cast<PlayerButton>(button));
        } else {
            player->releaseButton(static_cast<PlayerButton>(button));
        }

        // 2. Evaluamos la física y colisiones reales con los 3 argumentos requeridos
        float stepDt = 1.0f / 240.0f;
        player->update(stepDt);
        this->checkCollisions(player, stepDt, false);

        bool wouldDie = ConsoleManager::get().simulationDied || player->m_isDead;

        // 3. Restauramos el estado del jugador intacto
        restorePlayerState(player, state);
        ConsoleManager::get().isSimulating = false;
        ConsoleManager::get().simulationDied = false;

        return wouldDie;
    }

    void handleButton(bool isDown, int button, bool isPlayer1) {
        if (ConsoleManager::get().isSimulating || !ConsoleManager::get().isEnabled()) {
            GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
            return;
        }

        PlayerObject* target = isPlayer1 ? m_player1 : m_player2;

        // Si es una pulsación y detecta que el salto en este tick mataría:
        if (isDown && testIfInputKills(target, true, button)) {
            m_fields->queue.push_back({ isDown, button, isPlayer1, 1 });
            return;
        }

        GJBaseGameLayer::handleButton(isDown, button, isPlayer1);
    }

    void update(float dt) {
        if (ConsoleManager::get().isEnabled() && !m_fields->queue.empty()) {
            auto& pending = m_fields->queue.front();
            PlayerObject* target = pending.isPlayer1 ? m_player1 : m_player2;
            int maxTicks = ConsoleManager::get().getMaxTicks();

            // Comprobamos si en el tick actual ya es seguro saltar
            bool stillKills = testIfInputKills(target, pending.isDown, pending.button);

            if (!stillKills || pending.ticksDelayed >= maxTicks) {
                if (pending.ticksDelayed > 0) {
                    ConsoleManager::get().logSavedInput(pending.ticksDelayed, pending.isDown);
                }
                GJBaseGameLayer::handleButton(pending.isDown, pending.button, pending.isPlayer1);
                m_fields->queue.pop_front();
            } else {
                pending.ticksDelayed++;
            }
        }

        GJBaseGameLayer::update(dt);
    }
};

// Hook en PlayLayer para inicializar la consola y proteger destroyPlayer
class $modify(PredictivePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ConsoleManager::get().initConsole();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (ConsoleManager::get().isSimulating) {
            ConsoleManager::get().simulationDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

// Hook de teclado F6
class $modify(BufferKeyboardHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && key == enumKeyCodes::KEY_F6) {
            ConsoleManager::get().cycleMode();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};
