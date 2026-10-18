#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;

namespace {
    struct LocalObject {
        int kind; // 0 = Solid, 1 = Hazard, 2 = Slope, 3 = DBlock, 4 = Orb/Pad
        cocos2d::CCRect rect;
        float rot;
        bool flipX;
        bool flipY;
    };

    struct PendingInput {
        bool active = false;
        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
        uint64_t pressedTick = 0;
        int maxTicks = 0;
        bool queuedOpposite = false;
        int oppositeHoldTicks = 1;
    };

    PendingInput g_pendingP1;
    PendingInput g_pendingP2;
    bool g_holdingP1 = false;
    bool g_holdingP2 = false;
    bool g_replaying = false;

    uint64_t g_currentTick = 0;
    float g_lastX = 0.0f;
    float g_stepX = 1.4425f;
    bool g_f6WasDown = false;

    // Las 6 etapas exactas del video al pulsar F6:
    // Stage 1: 2 Ticks All | 2 Ticks Wave
    // Stage 2: 4 Ticks All | 4 Ticks Wave
    // Stage 3: 8 Ticks All | 8 Ticks Wave
    // Stage 4: 16 Ticks All | 8 Ticks Wave
    // Stage 5: 16 Ticks All | 16 Ticks Wave
    // Stage 6: OFF (100% Raw Input)
    int g_stage = 3;
    const int kStageAllTicks[]  = { 0, 2, 4, 8, 16, 16, 0 };
    const int kStageWaveTicks[] = { 0, 2, 4, 8,  8, 16, 0 };
    const int kWaveLookahead = 22;
    const int kCubeLookahead = 56;

    void initConsole() {
#ifdef GEODE_IS_WINDOWS
        static bool opened = false;
        if (!opened) {
            if (AllocConsole()) {
                FILE* fDummy = nullptr;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                SetConsoleTitleA("InputBuffer v1.0");
            }
            opened = true;
            std::printf("[InputBuffer] v1.0 LOADED (Wave Spikes/Slopes Trajectory Fix)\n");
            std::printf("[InputBuffer] Starting Mode: STAGE 3 (8 All / 8 Wave)\n");
        }
#endif
    }

    void printLog(std::string const& msg) {
        log::info("{}", msg);
#ifdef GEODE_IS_WINDOWS
        std::printf("%s\n", msg.c_str());
#endif
    }

    int getMaxBufferTicks(PlayerObject* p) {
        if (!p) return 0;
        return p->m_isDart ? kStageWaveTicks[g_stage] : kStageAllTicks[g_stage];
    }

    bool isInteractiveOrbOrPad(int id) {
        return (id == 36 || id == 84 || id == 141 || id == 1022 ||
                id == 1330 || id == 1333 || id == 1704 || id == 1751 ||
                id == 3004 || id == 3027 || id == 35 || id == 67 ||
                id == 140 || id == 1332);
    }

    void gatherNearbyObjects(PlayLayer* pl, PlayerObject* p, std::vector<LocalObject>& out) {
        out.clear();
        if (!pl || !pl->m_objects || !p) return;

        float px = p->getPositionX();
        float py = p->getPositionY();
        cocos2d::CCRect pRect = p->getObjectRect();
        // Cualquier objeto que ya nos solape estando vivos es decoracion pasable
        cocos2d::CCRect safeNowRect(
            px - pRect.size.width * 0.30f,
            py - pRect.size.height * 0.30f,
            pRect.size.width * 0.60f,
            pRect.size.height * 0.60f
        );

        for (GameObject* go : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!go) continue;

            float ox = go->getPositionX();
            float oy = go->getPositionY();
            if (ox < px - 40.0f || ox > px + 240.0f) continue;
            if (std::fabs(oy - py) > 180.0f) continue;

            int id = go->m_objectID;
            cocos2d::CCRect r = go->getObjectRect();
            if (r.size.width <= 0.1f || r.size.height <= 0.1f) continue;

            // D-Block (ID 1755)
            if (id == 1755) {
                out.push_back({ 3, r, 0.f, false, false });
                continue;
            }

            if (isInteractiveOrbOrPad(id)) {
                out.push_back({ 4, r, 0.f, false, false });
                continue;
            }

            auto type = go->m_objectType;
            if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
                if (safeNowRect.intersectsRect(r)) continue;
                out.push_back({ 1, r, 0.f, false, false });
            } else if (type == GameObjectType::Solid) {
                if (safeNowRect.intersectsRect(r)) continue;
                out.push_back({ 0, r, 0.f, false, false });
            } else if (type == GameObjectType::Slope) {
                out.push_back({ 2, r, go->getRotation(), go->isFlipX(), go->isFlipY() });
            }
        }
    }

    bool hitsSlopeTriangle(LocalObject const& s, float x, float y, float radius) {
        if (x + radius < s.rect.getMinX() || x - radius > s.rect.getMaxX() ||
            y + radius < s.rect.getMinY() || y - radius > s.rect.getMaxY()) {
            return false;
        }

        float u = (x - s.rect.getMinX()) / s.rect.size.width;
        float v = (y - s.rect.getMinY()) / s.rect.size.height;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        int rot = ((static_cast<int>(std::round(s.rot)) % 360) + 360) % 360;
        float ru = u;
        float rv = v;
        if (rot == 90) {
            ru = 1.0f - v;
            rv = u;
        } else if (rot == 180) {
            ru = 1.0f - u;
            rv = 1.0f - v;
        } else if (rot == 270) {
            ru = v;
            rv = 1.0f - u;
        }

        if (s.flipX) ru = 1.0f - ru;
        if (s.flipY) rv = 1.0f - rv;

        float pad = radius / std::max(s.rect.size.width, 15.0f);
        return (rv <= ru + pad);
    }

    bool overlapsDBlock(std::vector<LocalObject> const& objs, cocos2d::CCRect const& r) {
        for (auto const& o : objs) {
            if (o.kind == 3 && r.intersectsRect(o.rect)) return true;
        }
        return false;
    }

    bool overlapsOrbOrPad(std::vector<LocalObject> const& objs, float x, float y, float hw, float hh) {
        cocos2d::CCRect r(x - hw, y - hh, hw * 2.0f, hh * 2.0f);
        for (auto const& o : objs) {
            if (o.kind == 4 && r.intersectsRect(o.rect)) return true;
        }
        return false;
    }

    // Simula cuantos ticks sobrevive el Wave si espera `delayTicks` antes de cambiar a `targetDown`
    int evalWavePath(
        PlayerObject* p,
        std::vector<LocalObject> const& objs,
        bool currentDown,
        bool targetDown,
        int delayTicks
    ) {
        float x = p->getPositionX();
        float y = p->getPositionY();
        float gravSign = p->m_isUpsideDown ? -1.0f : 1.0f;
        float waveSlope = (p->m_vehicleSize < 1.0f) ? 2.0f : 1.0f;

        cocos2d::CCRect pRect = p->getObjectRect();
        float hw = std::max(pRect.size.width * 0.35f, 2.5f);
        float hh = std::max(pRect.size.height * 0.35f, 2.5f);

        int survivedAfterSwitch = 0;
        int totalTicks = delayTicks + kWaveLookahead;

        for (int step = 1; step <= totalTicks; ++step) {
            bool hold = (step <= delayTicks) ? currentDown : targetDown;
            float dir = (hold ? 1.0f : -1.0f) * gravSign;

            x += g_stepX;
            y += g_stepX * waveSlope * dir;

            if (y - hh < 0.0f) {
                return (step <= delayTicks) ? -1 : survivedAfterSwitch;
            }

            cocos2d::CCRect simRect(x - hw, y - hh, hw * 2.0f, hh * 2.0f);
            bool inDBlock = overlapsDBlock(objs, simRect);

            for (auto const& o : objs) {
                if (o.kind == 1) {
                    if (simRect.intersectsRect(o.rect)) {
                        return (step <= delayTicks) ? -1 : survivedAfterSwitch;
                    }
                } else if (o.kind == 0) {
                    if (simRect.intersectsRect(o.rect)) {
                        if (inDBlock) {
                            if (dir > 0.0f) y = o.rect.getMinY() - hh - 0.5f;
                            else y = o.rect.getMaxY() + hh + 0.5f;
                        } else {
                            return (step <= delayTicks) ? -1 : survivedAfterSwitch;
                        }
                    }
                } else if (o.kind == 2) {
                    if (hitsSlopeTriangle(o, x, y, hh * 0.8f)) {
                        return (step <= delayTicks) ? -1 : survivedAfterSwitch;
                    }
                }
            }

            if (step > delayTicks) {
                ++survivedAfterSwitch;
            }
        }
        return survivedAfterSwitch;
    }

    // Simula el arco completo de salto de Cubo / Bola / Robot / Spider (56 ticks = salto entero hasta aterrizar)
    int evalGroundJumpPath(
        PlayerObject* p,
        std::vector<LocalObject> const& objs,
        int delayTicks
    ) {
        float x = p->getPositionX();
        float y = p->getPositionY();
        float gravSign = p->m_isUpsideDown ? -1.0f : 1.0f;
        float miniScale = (p->m_vehicleSize < 1.0f) ? 0.8f : 1.0f;

        cocos2d::CCRect pRect = p->getObjectRect();
        float hw = pRect.size.width * 0.44f;
        float hh = pRect.size.height * 0.44f;

        bool onGround = p->m_isOnGround;
        // Convertir velocidad vertical de Cocos/GD a unidades por tick a 240Hz
        float vy = onGround ? 0.0f : static_cast<float>(p->m_yVelocity) * 0.48f;
        float gravityPerTick = 0.238f * gravSign;
        float jumpVelPerTick = 5.48f * miniScale * gravSign;

        int survivedAfterJump = 0;
        int totalTicks = delayTicks + kCubeLookahead;

        for (int step = 1; step <= totalTicks; ++step) {
            if (step == delayTicks + 1) {
                bool touchingOrb = overlapsOrbOrPad(objs, x, y, hw * 1.25f, hh * 1.25f);
                if (!onGround && !touchingOrb) {
                    return -1; // Sigue en el aire sin tocar suelo ni orbe
                }
                if (p->m_isBall || p->m_isSpider) {
                    gravSign = -gravSign;
                    gravityPerTick = -gravityPerTick;
                    vy = -1.5f * gravSign;
                } else {
                    vy = jumpVelPerTick;
                }
                onGround = false;
            }

            x += g_stepX;
            if (!onGround) {
                y += vy;
                vy -= gravityPerTick;
                if (!p->m_isUpsideDown && y - hh <= 0.0f) {
                    y = hh;
                    vy = 0.0f;
                    onGround = true;
                }
            }

            cocos2d::CCRect simRect(x - hw, y - hh, hw * 2.0f, hh * 2.0f);
            bool hasFloorContact = (!p->m_isUpsideDown && y - hh <= 0.5f);

            for (auto const& o : objs) {
                if (o.kind == 1) {
                    if (simRect.intersectsRect(o.rect)) {
                        return (step <= delayTicks) ? -1 : survivedAfterJump;
                    }
                } else if (o.kind == 0) {
                    if (!simRect.intersectsRect(o.rect)) continue;

                    if (gravSign > 0.0f) {
                        float playerBottom = y - hh;
                        float blockTop = o.rect.getMaxY();
                        if (vy <= 0.5f && playerBottom >= blockTop - 8.0f) {
                            y = blockTop + hh;
                            vy = 0.0f;
                            onGround = true;
                            hasFloorContact = true;
                        } else {
                            return (step <= delayTicks) ? -1 : survivedAfterJump;
                        }
                    } else {
                        float playerTop = y + hh;
                        float blockBottom = o.rect.getMinY();
                        if (vy >= -0.5f && playerTop <= blockBottom + 8.0f) {
                            y = blockBottom - hh;
                            vy = 0.0f;
                            onGround = true;
                            hasFloorContact = true;
                        } else {
                            return (step <= delayTicks) ? -1 : survivedAfterJump;
                        }
                    }
                } else if (o.kind == 2) {
                    if (hitsSlopeTriangle(o, x, y, hh * 0.7f)) {
                        if (step <= delayTicks) {
                            onGround = true;
                            hasFloorContact = true;
                        } else {
                            return survivedAfterJump;
                        }
                    }
                }
            }

            // Si durante la espera en el suelo se acaba la plataforma bajo los pies, empieza a caer
            if (step <= delayTicks && onGround && !hasFloorContact) {
                cocos2d::CCRect probeBelow(x - hw, y - hh - 4.0f * gravSign, hw * 2.0f, 6.0f);
                bool supported = false;
                for (auto const& o : objs) {
                    if ((o.kind == 0 || o.kind == 2) && probeBelow.intersectsRect(o.rect)) {
                        supported = true;
                        break;
                    }
                }
                if (!supported) {
                    onGround = false;
                }
            }

            if (step > delayTicks) {
                ++survivedAfterJump;
            }
        }
        return survivedAfterJump;
    }

    // Devuelve `true` si disparar el input en ESTE tick es igual o mejor que esperar dentro de `remainingTicks`
    bool isOptimalToFireNow(
        PlayLayer* pl,
        PlayerObject* p,
        bool currentDown,
        bool targetDown,
        int remainingTicks
    ) {
        if (!pl || !p || remainingTicks <= 0) return true;

        std::vector<LocalObject> objs;
        gatherNearbyObjects(pl, p, objs);

        if (p->m_isDart) {
            int scoreNow = evalWavePath(p, objs, currentDown, targetDown, 0);
            if (scoreNow >= kWaveLookahead) return true;

            int bestFuture = -1;
            for (int k = 1; k <= remainingTicks; ++k) {
                int s = evalWavePath(p, objs, currentDown, targetDown, k);
                if (s > bestFuture) bestFuture = s;
            }

            if (bestFuture > scoreNow) return false;
            return true;
        }

        if (!p->m_isShip && !p->m_isBird && !p->m_isSwing) {
            if (!targetDown) return true; // Soltar en modo cubo es seguro si no hay PRESS pendiente

            int scoreNow = evalGroundJumpPath(p, objs, 0);
            if (scoreNow >= kCubeLookahead) return true;

            int bestFuture = -1;
            for (int k = 1; k <= remainingTicks; ++k) {
                int s = evalGroundJumpPath(p, objs, k);
                if (s > bestFuture) bestFuture = s;
            }

            if (bestFuture > scoreNow) return false;
            if (scoreNow < 0) return false;
            return true;
        }

        return true;
    }

    void dispatchInputNow(GJBaseGameLayer* bgl, bool down, int button, bool isPlayer1) {
        if (isPlayer1) g_holdingP1 = down;
        else g_holdingP2 = down;

        g_replaying = true;
        bgl->handleButton(down, button, isPlayer1);
        g_replaying = false;
    }
}

class $modify(IBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        initConsole();
        g_pendingP1 = PendingInput{};
        g_pendingP2 = PendingInput{};
        g_holdingP1 = false;
        g_holdingP2 = false;
        g_currentTick = 0;
        g_lastX = this->m_player1 ? this->m_player1->getPositionX() : 0.0f;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_pendingP1 = PendingInput{};
        g_pendingP2 = PendingInput{};
        g_holdingP1 = false;
        g_holdingP2 = false;
        g_currentTick = 0;
        g_lastX = this->m_player1 ? this->m_player1->getPositionX() : 0.0f;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        ++g_currentTick;

        if (this->m_player1) {
            float curX = this->m_player1->getPositionX();
            float dx = curX - g_lastX;
            if (dx > 0.15f && dx < 12.0f) {
                g_stepX = dx;
            } else {
                g_stepX = this->m_player1->m_playerSpeed * 1.4425f;
            }
            g_lastX = curX;
        }

#ifdef GEODE_IS_WINDOWS
        bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6Down && !g_f6WasDown) {
            g_stage = (g_stage % 6) + 1;
            int allT = kStageAllTicks[g_stage];
            int waveT = kStageWaveTicks[g_stage];
            if (allT == 0 && waveT == 0) {
                printLog("[InputBuffer] F6 Cycle -> STAGE 6: OFF (100% Raw Input)");
            } else {
                printLog(fmt::format(
                    "[InputBuffer] F6 Cycle -> STAGE {}: {} Ticks All | {} Ticks Wave ({:.2f}f / {:.2f}f @ 60fps)",
                    g_stage, allT, waveT, allT * 0.25f, waveT * 0.25f
                ));
            }
        }
        g_f6WasDown = f6Down;
#endif

        auto updatePlayerBuffer = [&](PendingInput& pend, PlayerObject* player, bool& holdingState) {
            if (!pend.active || !player || player->m_isDead) return;

            int elapsed = static_cast<int>(g_currentTick - pend.pressedTick);
            int remaining = pend.maxTicks - elapsed;

            bool fireNow = (remaining <= 0) || isOptimalToFireNow(this, player, holdingState, pend.down, remaining);
            if (!fireNow) return;

            bool firedDown = pend.down;
            int firedBtn = pend.button;
            bool firedP1 = pend.isPlayer1;
            uint64_t origTick = pend.pressedTick;
            bool hasOpposite = pend.queuedOpposite;
            int holdLen = std::max(1, pend.oppositeHoldTicks);

            pend.active = false;
            pend.queuedOpposite = false;

            dispatchInputNow(this, firedDown, firedBtn, firedP1);

            if (elapsed > 0) {
                printLog(fmt::format(
                    "[InputBuffer] >> SAVED +{} TICKS ({:.2f} frames @ 60fps) {}! (Frame#: {} -> Delayed to {}) [Look: {}]",
                    elapsed,
                    elapsed * 0.25f,
                    firedDown ? "PRESS" : "RELEASE",
                    origTick,
                    g_currentTick,
                    kWaveLookahead
                ));
            }

            // Si el usuario ya habia soltado el raton mientras el PRESS esperaba en el buffer,
            // encolamos ahora el RELEASE para no cancelar el salto ni dejar el boton bloqueado
            if (hasOpposite) {
                pend.active = true;
                pend.down = !firedDown;
                pend.button = firedBtn;
                pend.isPlayer1 = firedP1;
                pend.pressedTick = g_currentTick;
                pend.maxTicks = player->m_isDart ? std::max(holdLen, getMaxBufferTicks(player)) : 1;
                pend.queuedOpposite = false;
            }
        };

        updatePlayerBuffer(g_pendingP1, this->m_player1, g_holdingP1);
        updatePlayerBuffer(g_pendingP2, this->m_player2, g_holdingP2);
    }
};

class $modify(IBBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* pl = PlayLayer::get();
        if (g_replaying || !pl || this != static_cast<GJBaseGameLayer*>(pl) || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        PlayerObject* player = isPlayer1 ? this->m_player1 : this->m_player2;
        int maxTicks = getMaxBufferTicks(player);
        bool& holdingState = isPlayer1 ? g_holdingP1 : g_holdingP2;
        PendingInput& pend = isPlayer1 ? g_pendingP1 : g_pendingP2;

        if (maxTicks <= 0 || !player || player->m_isDead) {
            pend.active = false;
            pend.queuedOpposite = false;
            holdingState = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Si llega el RELEASE mientras el PRESS temprano aun esta esperando en el buffer,
        // NO borramos el PRESS: guardamos el RELEASE en cola para despues de que salte
        if (pend.active && pend.down != down) {
            pend.queuedOpposite = true;
            pend.oppositeHoldTicks = std::max(1, static_cast<int>(g_currentTick - pend.pressedTick));
            return;
        }

        if (isOptimalToFireNow(pl, player, holdingState, down, maxTicks)) {
            pend.active = false;
            pend.queuedOpposite = false;
            holdingState = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        pend.active = true;
        pend.down = down;
        pend.button = button;
        pend.isPlayer1 = isPlayer1;
        pend.pressedTick = g_currentTick;
        pend.maxTicks = maxTicks;
        pend.queuedOpposite = false;
    }
};
