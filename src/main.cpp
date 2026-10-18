#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <cmath>
#include <cstdio>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;

namespace {
    struct BufferedInput {
        bool active = false;
        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
        uint64_t pressedTick = 0;
        int maxTicks = 0;
    };

    BufferedInput g_bufferP1;
    BufferedInput g_bufferP2;
    uint64_t g_currentTick = 0;
    bool g_replayingInput = false;

    // F6 etapas:
    // Stage 1 = 2 ticks | Stage 2 = 4 ticks | Stage 3 = 8 ticks
    // Stage 4 = 9 ticks (convierte 1 tick en 10) | Stage 5 = 16 ticks | Stage 6 = OFF
    int g_stage = 2; // Empieza en 4 ticks por defecto
    const int kStageTicks[] = { 0, 2, 4, 8, 9, 16, 0 };
    const int kLookahead = 22;

    void initConsole() {
#ifdef GEODE_IS_WINDOWS
        static bool opened = false;
        if (!opened) {
            if (AllocConsole()) {
                FILE* fDummy = nullptr;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
            }
            opened = true;
            std::printf("[InputBuffer] v1.0 LOADED (Wave Spikes/Slopes Trajectory Fix)\n");
            std::printf("[InputBuffer] Pulsa F6 en un nivel para cambiar: 2 -> 4 -> 8 -> 9 (1 a 10 ticks) -> 16 -> OFF\n");
        }
#endif
    }

    void printLog(std::string const& msg) {
        log::info("{}", msg);
#ifdef GEODE_IS_WINDOWS
        std::printf("%s\n", msg.c_str());
#endif
    }

    float getStepX(PlayerObject* p) {
        return p->m_playerSpeed * 1.29825f;
    }

    bool collidesAt(PlayLayer* pl, PlayerObject* p, cocos2d::CCPoint pos) {
        if (!pl || !pl->m_objects) return false;

        cocos2d::CCRect pRect = p->getObjectRect();
        float halfW = pRect.size.width * 0.45f;
        float halfH = pRect.size.height * 0.45f;
        cocos2d::CCRect simRect(pos.x - halfW, pos.y - halfH, halfW * 2.f, halfH * 2.f);

        if (p->m_isDart && (pos.y - halfH < 0.0f)) {
            return true;
        }

        CCObject* obj = nullptr;
        CCARRAY_FOREACH(pl->m_objects, obj) {
            auto* go = typeinfo_cast<GameObject*>(obj);
            if (!go) continue;

            if (std::fabs(go->getPositionX() - pos.x) > 90.0f) continue;

            auto type = go->m_objectType;
            bool isHazard = (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard);
            bool isSolid  = (type == GameObjectType::Solid || type == GameObjectType::Slope);
            if (!isHazard && !isSolid) continue;

            cocos2d::CCRect objRect = go->getObjectRect();
            if (!simRect.intersectsRect(objRect)) continue;

            if (isHazard) return true;

            if (isSolid) {
                if (p->m_isDart) return true;
                float playerBottom = pos.y - halfH;
                float blockTop = objRect.getMaxY();
                if (!p->m_isUpsideDown && playerBottom < blockTop - 6.0f) return true;

                float playerTop = pos.y + halfH;
                float blockBottom = objRect.getMinY();
                if (p->m_isUpsideDown && playerTop > blockBottom + 6.0f) return true;
            }
        }
        return false;
    }

    bool isTouchingOrb(PlayLayer* pl, PlayerObject* p) {
        if (!pl || !pl->m_objects) return false;
        cocos2d::CCRect pRect = p->getObjectRect();

        CCObject* obj = nullptr;
        CCARRAY_FOREACH(pl->m_objects, obj) {
            auto* go = typeinfo_cast<GameObject*>(obj);
            if (!go) continue;
            if (std::fabs(go->getPositionX() - p->getPositionX()) > 60.0f) continue;

            auto t = go->m_objectType;
            if (t == GameObjectType::YellowOrb || t == GameObjectType::PinkOrb ||
                t == GameObjectType::RedOrb || t == GameObjectType::BlueOrb ||
                t == GameObjectType::GreenOrb || t == GameObjectType::BlackOrb ||
                t == GameObjectType::DashOrb || t == GameObjectType::SpiderOrb) {
                if (pRect.intersectsRect(go->getObjectRect())) return true;
            }
        }
        return false;
    }

    bool isTrajectorySafe(PlayLayer* pl, PlayerObject* p, bool down, int lookahead) {
        if (!pl || !p) return true;

        float x = p->getPositionX();
        float y = p->getPositionY();
        float stepX = getStepX(p);
        float gravSign = p->m_isUpsideDown ? -1.0f : 1.0f;
        float miniScale = (p->m_vehicleSize < 1.0f) ? 0.8f : 1.0f;

        if (p->m_isDart) {
            float dir = (down ? 1.0f : -1.0f) * gravSign;
            float slope = (p->m_vehicleSize < 1.0f) ? 2.0f : 1.0f;
            float stepY = stepX * slope * dir;

            for (int i = 1; i <= lookahead; ++i) {
                x += stepX;
                y += stepY;
                if (collidesAt(pl, p, { x, y })) return false;
            }
            return true;
        }

        if (!p->m_isShip && !p->m_isBird && !p->m_isSwing) {
            if (!down) return true;
            if (!p->m_isOnGround && !isTouchingOrb(pl, p)) return false;

            float vy = 10.8f * miniScale * gravSign;
            float gravity = -0.45f * gravSign;
            for (int i = 1; i <= lookahead; ++i) {
                x += stepX;
                y += vy * 0.25f;
                vy += gravity;
                if (collidesAt(pl, p, { x, y })) return false;
            }
            return true;
        }

        float vy = static_cast<float>(p->m_yVelocity);
        float accel = (down ? 0.35f : -0.30f) * gravSign;
        for (int i = 1; i <= lookahead; ++i) {
            x += stepX;
            vy += accel;
            y += vy * 0.25f;
            if (collidesAt(pl, p, { x, y })) return false;
        }
        return true;
    }
}

class $modify(IBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        initConsole();
        g_bufferP1.active = false;
        g_bufferP2.active = false;
        g_currentTick = 0;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_bufferP1.active = false;
        g_bufferP2.active = false;
        g_currentTick = 0;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        ++g_currentTick;

        auto checkBuffer = [&](BufferedInput& buf, PlayerObject* player) {
            if (!buf.active || !player || player->m_isDead) return;

            uint64_t elapsed = g_currentTick - buf.pressedTick;
            bool safeNow = isTrajectorySafe(this, player, buf.down, kLookahead);

            bool emergencyWave = false;
            if (player->m_isDart && !safeNow) {
                bool staySafe1 = isTrajectorySafe(this, player, !buf.down, 1);
                bool switchSafe2 = isTrajectorySafe(this, player, buf.down, 2);
                if (!staySafe1 && switchSafe2) emergencyWave = true;
            }

            if (safeNow || emergencyWave || static_cast<int>(elapsed) >= buf.maxTicks) {
                buf.active = false;
                g_replayingInput = true;
                this->handleButton(buf.down, buf.button, buf.isPlayer1);
                g_replayingInput = false;

                if (elapsed > 0 && (safeNow || emergencyWave)) {
                    printLog(fmt::format(
                        "[InputBuffer] >> SAVED {} TICKS ({:.2f} frames @ 60fps) {}! (Frame#: {} -> Delayed to {}) [Look: {}]",
                        elapsed,
                        elapsed * 0.25f,
                        buf.down ? "PRESS" : "RELEASE",
                        buf.pressedTick,
                        g_currentTick,
                        kLookahead
                    ));
                }
            }
        };

        checkBuffer(g_bufferP1, this->m_player1);
        checkBuffer(g_bufferP2, this->m_player2);
    }
};

class $modify(IBBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* pl = PlayLayer::get();
        if (g_replayingInput || !pl || this != static_cast<GJBaseGameLayer*>(pl) || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int maxTicks = kStageTicks[g_stage];
        PlayerObject* player = isPlayer1 ? this->m_player1 : this->m_player2;
        if (maxTicks <= 0 || !player || player->m_isDead) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        BufferedInput& buf = isPlayer1 ? g_bufferP1 : g_bufferP2;
        if (isTrajectorySafe(pl, player, down, kLookahead)) {
            buf.active = false;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        buf.active = true;
        buf.down = down;
        buf.button = button;
        buf.isPlayer1 = isPlayer1;
        buf.pressedTick = g_currentTick;
        buf.maxTicks = maxTicks;
    }
};

class $modify(IBKeyDispatcher, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(cocos2d::enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (isKeyDown && !isKeyRepeat && key == cocos2d::enumKeyCodes::KEY_F6) {
            g_stage = (g_stage % 6) + 1;
            int t = kStageTicks[g_stage];
            if (t == 0) {
                printLog("[InputBuffer] F6 Cycle -> STAGE 6: OFF (100% Raw Input)");
            } else {
                printLog(fmt::format(
                    "[InputBuffer] F6 Cycle -> STAGE {}: {} Ticks All | {} Ticks Wave ({:.2f}f @ 60fps)",
                    g_stage, t, t, t * 0.25f
                ));
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);
    }
};
