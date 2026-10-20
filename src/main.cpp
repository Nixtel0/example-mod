#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
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
        std::cout << "[InputBuffer] LOADED (Lookahead Trajectory Console)\n";
        std::cout << "[InputBuffer] Default Mode: STAGE 1 (2 Ticks / ~8.3ms)\n";
        std::cout << "[InputBuffer] Presiona F6 para cambiar: OFF -> 2T -> 4T -> 8T\n";
        std::cout << "========================================================\n\n";

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
                  << " delayed to avoid death\n";
    }

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
    }
};

struct DeferredInput {
    bool isDown;
    bool isPlayer2;
    int ticksDelayed;
};

class $modify(LookaheadPlayLayer, PlayLayer) {
    struct Fields {
        std::deque<DeferredInput> pendingInputs;
        bool isSimulating = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ConsoleManager::get().initConsole();
        return true;
    }

    struct PlayerSnapshot {
        cocos2d::CCPoint position;
        double yVelocity;
        double xVelocity;
        float rotation;
        bool isDead;
        bool isOnGround;
    };

    PlayerSnapshot takeSnapshot(PlayerObject* player) {
        return {
            player->getPosition(),
            player->m_yVelocity,
            player->m_xVelocity,
            player->getRotation(),
            player->m_isDead,
            player->m_isOnGround
        };
    }

    void restoreSnapshot(PlayerObject* player, const PlayerSnapshot& snap) {
        player->setPosition(snap.position);
        player->m_yVelocity = snap.yVelocity;
        player->m_xVelocity = snap.xVelocity;
        player->setRotation(snap.rotation);
        player->m_isDead = snap.isDead;
        player->m_isOnGround = snap.isOnGround;
    }

    bool wouldInputKill(PlayerObject* player, bool isDown, float dt) {
        if (!player || player->m_isDead) return false;

        PlayerSnapshot snap = takeSnapshot(player);
        m_fields->isSimulating = true;

        if (isDown) {
            player->pushButton(PlayerButton::Jump);
        }

        player->update(dt);
        bool died = player->m_isDead;

        restoreSnapshot(player, snap);
        m_fields->isSimulating = false;

        return died;
    }

    void pushButton(int state, bool isPlayer2) {
        if (m_fields->isSimulating || !ConsoleManager::get().isEnabled()) {
            PlayLayer::pushButton(state, isPlayer2);
            return;
        }

        PlayerObject* target = isPlayer2 ? m_player2 : m_player1;
        float stepDt = 1.0f / 240.0f;

        if (wouldInputKill(target, true, stepDt)) {
            m_fields->pendingInputs.push_back({ true, isPlayer2, 0 });
            return;
        }

        PlayLayer::pushButton(state, isPlayer2);
    }

    void update(float dt) {
        if (ConsoleManager::get().isEnabled() && !m_fields->pendingInputs.empty()) {
            auto& nextInput = m_fields->pendingInputs.front();
            PlayerObject* target = nextInput.isPlayer2 ? m_player2 : m_player1;
            int maxTicks = ConsoleManager::get().getMaxTicks();

            if (!wouldInputKill(target, nextInput.isDown, dt) || nextInput.ticksDelayed >= maxTicks) {
                if (nextInput.ticksDelayed > 0) {
                    ConsoleManager::get().logSavedInput(nextInput.ticksDelayed, nextInput.isDown);
                }
                PlayLayer::pushButton(0, !nextInput.isPlayer2);
                m_fields->pendingInputs.pop_front();
            } else {
                nextInput.ticksDelayed++;
            }
        }

        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (m_fields && m_fields->isSimulating) {
            player->m_isDead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(BufferKeyboardHook, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (isKeyDown && !isKeyRepeat && key == enumKeyCodes::KEY_F6) {
            ConsoleManager::get().cycleMode();
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);
    }
};