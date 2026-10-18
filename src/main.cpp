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
        int kind; // 0 = Solid, 1 = Spike, 2 = Saw, 3 = Slope, 4 = DBlock, 5 = Orb/Pad
        cocos2d::CCRect rect;
        float cx;
        float cy;
        float rot;
        bool flipX;
        bool flipY;
    };

    // Estado real del boton dentro del juego vs buffer pendiente
    bool g_actualHoldingP1 = false;
    bool g_actualHoldingP2 = false;
    bool g_replayingInput = false;

    struct PendingAction {
        bool active = false;
        bool down = false;
        int button = 1;
        bool isPlayer1 = true;
        uint64_t pressedTick = 0;
        int maxTicks = 0;

        // Si el jugador suelta el raton antes de que se dispare el PRESS guardado,
        // guardamos la duracion del micro-click para soltarlo despues de disparar el PRESS
        bool hasQueuedOpposite = false;
        int oppositeDelayTicks = 1;
    };

    PendingAction g_pendingP1;
    PendingAction g_pendingP2;

    uint64_t g_currentTick = 0;
    float g_lastX = 0.0f;
    float g_measuredStepX = 1.29825f;
    bool g_f6WasDown = false;

    // Etapas con F6 (igual que en el video + modo de 10 ticks):
    // Stage 1: 2 All / 2 Wave
    // Stage 2: 4 All / 4 Wave
    // Stage 3: 8 All / 8 Wave
    // Stage 4: 10 All / 10 Wave (convierte 1 tick en 10 ticks)
    // Stage 5: 16 All / 8 Wave
    // Stage 6: 16 All / 16 Wave
    // Stage 7: OFF (100% Raw Input)
    int g_stage = 2; // Inicia en Stage 2 (4 ticks)
    const int kStageAllTicks[]  = { 0, 2, 4, 8, 9, 16, 16, 0 };
    const int kStageWaveTicks[] = { 0, 2, 4, 8, 9,  8, 16, 0 };
    const int kWaveLookahead = 22;
    const int kCubeLookahead = 88;

    void initConsole() {
#ifdef GEODE_IS_WINDOWS
        static bool opened = false;
        if (!opened) {
            if (AllocConsole()) {
                FILE* fDummy = nullptr;
                freopen_s(&fDummy, "CONOUT$", "w", stdout);
                SetConsoleTitleA("Geode InputBuffer v1.0");
            }
            opened = true;
            std::printf("[InputBuffer] v1.0 LOADED (Wave Spikes/Slopes Trajectory Fix)\n");
            std::printf("[InputBuffer] Starting Mode: STAGE 2 (4 All / 4 Wave)\n");
            std::printf("[InputBuffer] Press F6 in level to cycle: 2/2 -> 4/4 -> 8/8 -> 9/9 (1->10t) -> 16/8 -> 16/16 -> OFF\n");
        }
#endif
    }

    void printLog(std::string const& msg) {
        log::info("{}", msg);
#ifdef GEODE_IS_WINDOWS
        std::printf("%s\n", msg.c_str());
#endif
    }

    int getMaxTicksForPlayer(PlayerObject* p) {
        if (!p) return 0;
        return p->m_isDart ? kStageWaveTicks[g_stage] : kStageAllTicks[g_stage];
    }

    bool isOrbOrPadID(int id, int type) {
        // Orbes y pads interactivos en GD
        if (type == 10 || type == 11 || type == 12 || type == 19 ||
            type == 26 || type == 27 || type == 36 || type == 40) {
            return true;
        }
        return (id == 36 || id == 84 || id == 141 || id == 1022 ||
                id == 1330 || id == 1333 || id == 1704 || id == 1751 ||
                id == 3004 || id == 3027 || id == 35 || id == 67 ||
                id == 140 || id == 1332);
    }

    // Recopila solo los objetos cercanos una vez por tick y filtra decoracion pasable
    void collectLocalObjects(PlayLayer* pl, PlayerObject* p, std::vector<LocalObject>& out) {
        out.clear();
        if (!pl || !pl->m_objects || !p) return;

        float px = p->getPositionX();
        float py = p->getPositionY();
        cocos2d::CCRect currentPRect = p->getObjectRect();
        // Hitbox interna actual del jugador (para ignorar decoracion NoTouch que ya nos solapa)
        cocos2d::CCRect aliveCore(
            px - currentPRect.size.width * 0.25f,
            py - currentPRect.size.height * 0.25f,
            currentPRect.size.width * 0.50f,
            currentPRect.size.height * 0.50f
        );

        for (GameObject* go : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (!go) continue;

            float ox = go->getPositionX();
            float oy = go->getPositionY();
            if (ox < px - 45.0f || ox > px + 260.0f) continue;
            if ( std::fabs(oy - py) > 190.0f) continue;

            int id = go->m_objectID;
            int type = static_cast<int>(go->m_objectType);
            cocos2d::CCRect r = go->getObjectRect();
            if (r.size.width <= 0.1f || r.size.height <= 0.1f) continue;

            // D-Block (ID 1755): permite que el Wave deslice sobre bloques solidos
            if (id == 1755) {
                out.push_back({ 4, r, ox, oy, 0.f, false, false });
                continue;
            }

            if (isOrbOrPadID(id, type)) {
                out.push_back({ 5, r, ox, oy, 0.f, false, false });
                continue;
            }

            // 0 = Solid, 2 = Hazard (Spike), 39 = AnimatedHazard (Saw), 23 = Slope
            if (type == 0) {
                // Si el centro vivo del jugador ya esta dentro del bloque, es decoracion NoTouch
                if (aliveCore.intersectsRect(r)) continue;
                out.push_back({ 0, r, ox, oy, 0.f, false, false });
            } else if (type == 2) {
                // Hitbox real de pincho en GD (~24% ancho x 42% alto centrado)
                float sw = r.size.width * 0.24f;
                float sh = r.size.height * 0.42f;
                cocos2d::CCRect spikeRect(ox - sw * 0.5f, oy - sh * 0.5f, sw, sh);
                if (aliveCore.intersectsRect(spikeRect)) continue;
                out.push_back({ 1, spikeRect, ox, oy, 0.f, false, false });
            } else if (type == 39) {
                // Sierra / peligro circular
                float sw = r.size.width * 0.62f;
                float sh = r.size.height * 0.62f;
                cocos2d::CCRect sawRect(ox - sw * 0.5f, oy - sh * 0.5f, sw, sh);
                if (aliveCore.intersectsRect(sawRect)) continue;
                out.push_back({ 2, sawRect, ox, oy, 0.f, false, false });
            } else if (type == 23) {
                out.push_back({
                    3,
                    r,
                    ox,
                    oy,
                    go->getRotation(),
                    go->isFlipX(),
                    go->isFlipY()
                });
            }
        }
    }

    bool pointHitsSlope(LocalObject const& s, float x, float y, float radius) {
        if (x + radius < s.rect.getMinX() || x - radius > s.rect.getMaxX() ||
            y + radius < s.rect.getMinY() || y - radius > s.rect.getMaxY()) {
            return false;
        }

        float u = (x - s.rect.getMinX()) / s.rect.size.width;
        float v = (y - s.rect.getMinY()) / s.rect.size.height;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        // Transformar a coordenadas locales de la rampa segun rotacion y flip
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

        float margin = radius / std::max(s.rect.size.width, 15.0f);
        // En espacio local estandar de GD, el triangulo solido es rv <= ru
        return (rv <= ru + margin);
    }

    bool touchesOrbAt(std::vector<LocalObject> const& objs, float x, float y, float halfW, float halfH) {
        cocos2d::CCRect pRect(x - halfW, y - halfH, halfW * 2.0f, halfH * 2.0f);
        for (auto const& o : objs) {
            if (o.kind == 5 && pRect.intersectsRect(o.rect)) {
                return true;
            }
        }
        return false;
    }

    bool isInsideDBlock(std::vector<LocalObject> const& objs, cocos2d::CCRect const& pRect) {
        for (auto const& o : objs) {
            if (o.kind == 4 && pRect.intersectsRect(o.rect)) {
                return true;
            }
        }
        return false;
    }

    // Devuelve cuantos ticks sobrevive el Wave si mantiene el estado actual durante `delayTicks`
    // y cambia al estado `targetDown` en el tick `delayTicks` (evaluando `kWaveLookahead` ticks desde el cambio)
    int simulateWaveSurvival(
        PlayerObject* p,
        std::vector<LocalObject> const& objs,
        bool currentDown,
        bool targetDown,
        int delayTicks
    ) {
        float x = p->getPositionX();
        float y = p->getPositionY();
        float stepX = g_measuredStepX;
        float gravSign = p->m_isUpsideDown ? -1.0f : 1.0f;
        float slope = (p->m_vehicleSize < 1.0f) ? 2.0f : 1.0f;

        cocos2d::CCRect baseRect = p->getObjectRect();
        float halfW = std::max(baseRect.size.width * 0.32f, 2.5f);
        float halfH = std::max(baseRect.size.height * 0.32f, 2.5f);

        int totalSteps = delayTicks + kWaveLookahead;
        int survivedAfterSwitch = 0;

        for (int step = 1; step <= totalSteps; ++step) {
            bool activeDown = (step <= delayTicks) ? currentDown : targetDown;
            float dir = (activeDown ? 1.0f : -1.0f) * gravSign;
            x += stepX;
            y += stepX * slope * dir;

            // Suelo base del nivel
            if (y - halfH < 0.0f) {
                return (step <= delayTicks) ? -1 : survivedAfterSwitch;
            }

            cocos2d::CCRect simRect(x - halfW, y - halfH, halfW * 2.0f, halfH * 2.0f);
            bool inDBlock = isInsideDBlock(objs, simRect);

            for (auto const& o : objs) {
                if (o.kind == 1 || o.kind == 2) {
                    if (simRect.intersectsRect(o.rect)) {
                        return (step <= delayTicks) ? -1 : survivedAfterSwitch;
                    }
                } else if (o.kind == 0) {
                    if (simRect.intersectsRect(o.rect)) {
                        if (inDBlock) {
                            // En un D-Block el wave desliza sin morir
                            if (dir > 0.0f) {
                                y = o.rect.getMinY() - halfH - 0.5f;
                            } else {
                                y = o.rect.getMaxY() + halfH + 0.5f;
                            }
                        } else {
                            return (step <= delayTicks) ? -1 : survivedAfterSwitch;
                        }
                    }
                } else if (o.kind == 3) {
                    if (pointHitsSlope(o, x, y, halfH * 0.85f)) {
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

    // Simula cuantos ticks sobrevive el Cubo/Bola/Robot/Spider si espera `delayTicks` ticks
    // antes de ejecutar el salto/orbe `targetDown == true`
    int simulateGroundModeSurvival(
        PlayerObject* p,
        std::vector<LocalObject> const& objs,
        int delayTicks
    ) {
        float x = p->getPositionX();
        float y = p->getPositionY();
        float stepX = g_measuredStepX;
        float gravSign = p->m_isUpsideDown ? -1.0f : 1.0f;
        float miniScale = (p->m_vehicleSize < 1.0f) ? 0.8f : 1.0f;

        cocos2d::CCRect baseRect = p->getObjectRect();
        float halfW = baseRect.size.width * 0.42f;
        float halfH = baseRect.size.height * 0.42f;

        bool onGround = p->m_isOnGround;
        float vy = static_cast<float>(p->m_yVelocity);
        bool jumped = false;
        int survivedAfterJump = 0;

        int totalSteps = delayTicks + kCubeLookahead;
        for (int step = 1; step <= totalSteps; ++step) {
            // En el tick exacto `delayTicks + 1`, intentamos ejecutar el salto/orbe
            if (step == delayTicks + 1) {
                bool canOrb = touchesOrbAt(objs, x, y, halfW * 1.15f, halfH * 1.15f);
                if (!onGround && !canOrb) {
                    // Todavia esta en el aire sin tocar suelo ni orbe: no puede saltar en este tick
                    return -1;
                }
                vy = 11.18f * miniScale * gravSign;
                onGround = false;
                jumped = true;
            }

            x += stepX;
            if (!onGround) {
                y += vy * 0.9f * 0.25f;
                vy -= 0.958f * gravSign * 0.25f;
                if (!p->m_isUpsideDown && y - halfH <= 0.0f) {
                    y = halfH;
                    vy = 0.0f;
                    onGround = true;
                }
            }

            cocos2d::CCRect simRect(x - halfW, y - halfH, halfW * 2.0f, halfH * 2.0f);
            bool supportedThisTick = (!p->m_isUpsideDown && y - halfH <= 0.5f);

            for (auto const& o : objs) {
                if (o.kind == 1 || o.kind == 2) {
                    if (simRect.intersectsRect(o.rect)) {
                        return (step <= delayTicks) ? -1 : survivedAfterJump;
                    }
                } else if (o.kind == 0) {
                    if (!simRect.intersectsRect(o.rect)) continue;

                    if (!p->m_isUpsideDown) {
                        float playerBottom = y - halfH;
                        float blockTop = o.rect.getMaxY();
                        // Si cae o corre por encima de la superficie superior del bloque, aterriza
                        if (vy <= 0.5f && playerBottom >= blockTop - 7.5f) {
                            y = blockTop + halfH;
                            vy = 0.0f;
                            onGround = true;
                            supportedThisTick = true;
                        } else {
                            // Choque frontal o contra el techo del bloque
                            return (step <= delayTicks) ? -1 : survivedAfterJump;
                        }
                    } else {
                        float playerTop = y + halfH;
                        float blockBottom = o.rect.getMinY();
                        if (vy >= -0.5f && playerTop <= blockBottom + 7.5f) {
                            y = blockBottom - halfH;
                            vy = 0.0f;
                            onGround = true;
                            supportedThisTick = true;
                        } else {
                            return (step <= delayTicks) ? -1 : survivedAfterJump;
                        }
                    }
                } else if (o.kind == 3) {
                    if (pointHitsSlope(o, x, y, halfH * 0.7f)) {
                        if (!jumped) {
                            onGround = true;
                            supportedThisTick = true;
                        } else {
                            return survivedAfterJump;
                        }
                    }
                }
            }

            // Si mientras esperamos en el suelo se acaba el bloque bajo nuestros pies, empieza a caer
            if (step <= delayTicks && onGround && !supportedThisTick) {
                // Comprobar si hay algun bloque justo debajo a 3 unidades
                cocos2d::CCRectProbe;
                cocos2d::CCRect feetRect(x - halfW, y - halfH - 4.0f * gravSign, halfW * 2.0f, 6.0f);
                bool hasFloorBelow = (!p->m_isUpsideDown && (y - halfH <= 2.0f));
                if (!hasFloorBelow) {
                    for (auto const& o : objs) {
                        if ((o.kind == 0 || o.kind == 3) && feetRect.intersectsRect(o.rect)) {
                            hasFloorBelow = true;
                            break;
                        }
                    }
                }
                if (!hasFloorBelow) {
                    onGround = false;
                }
            }

            if (step > delayTicks) {
                ++survivedAfterJump;
            }
        }
        return survivedAfterJump;
    }

    // Decide si ejecutar el input AHORA MISMO (delay = 0) es optimo o si conviene esperar
    // algun tick `k` dentro de `remainingTicks`
    bool shouldFireNow(
        PlayLayer* pl,
        PlayerObject* p,
        bool currentDown,
        bool targetDown,
        int remainingTicks
    ) {
        if (!pl || !p || remainingTicks <= 0) return true;

        std::vector<LocalObject> objs;
        collectLocalObjects(pl, p, objs);

        // 1. WAVE MODE (tanto PRESS como RELEASE)
        if (p->m_isDart) {
            int scoreNow = simulateWaveSurvival(p, objs, currentDown, targetDown, 0);
            if (scoreNow >= kWaveLookahead) {
                return true; // Ya sobrevive los 22 ticks completos
            }

            int bestFutureScore = -1;
            for (int k = 1; k <= remainingTicks; ++k) {
                int s = simulateWaveSurvival(p, objs, currentDown, targetDown, k);
                if (s > bestFutureScore) {
                    bestFutureScore = s;
                }
            }

            // Si esperar al menos 1 tick mejora claramente la trayectoria, seguimos esperando
            if (bestFutureScore > scoreNow) {
                return false;
            }
            return true;
        }

        // 2. CUBE / BALL / ROBOT / SPIDER
        if (!p->m_isShip && !p->m_isBird && !p->m_isSwing) {
            if (!targetDown) {
                return true; // Soltar en modo cubo no mata
            }

            int scoreNow = simulateGroundModeSurvival(p, objs, 0);
            if (scoreNow >= kCubeLookahead) {
                return true; // Saltar ahora mismo limpia todos los pinchos
            }

            int bestFutureScore = -1;
            for (int k = 1; k <= remainingTicks; ++k) {
                int s = simulateGroundModeSurvival(p, objs, k);
                if (s > bestFutureScore) {
                    bestFutureScore = s;
                }
            }

            if (bestFutureScore > scoreNow) {
                return false;
            }
            // Si aun esta en el aire sin tocar suelo ni orbe (`scoreNow == -1`), seguir guardando el clic
            if (scoreNow < 0) {
                return false;
            }
            return true;
        }

        // 3. SHIP / UFO / SWING
        return true;
    }

    void fireRawInput(GJBaseGameLayer* bgl, bool down, int button, bool isPlayer1) {
        if (isPlayer1) g_actualHoldingP1 = down;
        else g_actualHoldingP2 = down;

        g_replayingInput = true;
        bgl->handleButton(down, button, isPlayer1);
        g_replayingInput = false;
    }
}

class $modify(IBPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        initConsole();
        g_pendingP1 = PendingAction{};
        g_pendingP2 = PendingAction{};
        g_actualHoldingP1 = false;
        g_actualHoldingP2 = false;
        g_currentTick = 0;
        g_lastX = this->m_player1 ? this->m_player1->getPositionX() : 0.0f;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_pendingP1 = PendingAction{};
        g_pendingP2 = PendingAction{};
        g_actualHoldingP1 = false;
        g_actualHoldingP2 = false;
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
                g_measuredStepX = dx;
            } else {
                g_measuredStepX = this->m_player1->m_playerSpeed * 1.4425f;
            }
            g_lastX = curX;
        }

#ifdef GEODE_IS_WINDOWS
        bool f6Down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6Down && !g_f6WasDown) {
            g_stage = (g_stage % 7) + 1;
            int allT = kStageAllTicks[g_stage];
            int waveT = kStageWaveTicks[g_stage];
            if (allT == 0 && waveT == 0) {
                printLog("[InputBuffer] F6 Cycle -> STAGE 7: OFF (100% Raw Input)");
            } else {
                printLog(fmt::format(
                    "[InputBuffer] F6 Cycle -> STAGE {}: {} Ticks All | {} Ticks Wave ({:.2f}f / {:.2f}f @ 60fps)",
                    g_stage, allT, waveT, allT * 0.25f, waveT * 0.25f
                ));
            }
        }
        g_f6WasDown = f6Down;
#endif

        auto processPending = [&](PendingAction& pend, PlayerObject* player, bool& actualHold) {
            if (!pend.active || !player || player->m_isDead) return;

            int elapsed = static_cast<int>(g_currentTick - pend.pressedTick);
            int remaining = pend.maxTicks - elapsed;

            bool fire = (remaining <= 0) || shouldFireNow(this, player, actualHold, pend.down, remaining);
            if (!fire) return;

            bool firedDown = pend.down;
            int firedButton = pend.button;
            bool firedP1 = pend.isPlayer1;
            uint64_t startTick = pend.pressedTick;
            bool chainOpposite = pend.hasQueuedOpposite;
            int oppositeHoldTicks = std::max(1, pend.oppositeDelayTicks);

            pend.active = false;
            pend.hasQueuedOpposite = false;

            fireRawInput(this, firedDown, firedButton, firedP1);

            if (elapsed > 0) {
                printLog(fmt::format(
                    "[InputBuffer] >> SAVED +{} TICKS ({:.2f} frames @ 60fps) {}! (Frame#: {} -> Delayed to {}) [Look: {}]",
                    elapsed,
                    elapsed * 0.25f,
                    firedDown ? "PRESS" : "RELEASE",
                    startTick,
                    g_currentTick,
                    kWaveLookahead
                ));
            }

            // Si el usuario ya habia soltado el raton durante la espera (micro-click),
            // programamos ahora el RELEASE para que no se cancele ni se quede pulsado
            if (chainOpposite) {
                pend.active = true;
                pend.down = !firedDown;
                pend.button = firedButton;
                pend.isPlayer1 = firedP1;
                pend.pressedTick = g_currentTick;
                pend.maxTicks = std::max(oppositeHoldTicks, getMaxTicksForPlayer(player));
                pend.hasQueuedOpposite = false;
            }
        };

        processPending(g_pendingP1, this->m_player1, g_actualHoldingP1);
        processPending(g_pendingP2, this->m_player2, g_actualHoldingP2);
    }
};

class $modify(IBBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto* pl = PlayLayer::get();
        if (g_replayingInput || !pl || this != static_cast<GJBaseGameLayer*>(pl) || button != 1) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        PlayerObject* player = isPlayer1 ? this->m_player1 : this->m_player2;
        int maxTicks = getMaxTicksForPlayer(player);
        bool& actualHold = isPlayer1 ? g_actualHoldingP1 : g_actualHoldingP2;
        PendingAction& pend = isPlayer1 ? g_pendingP1 : g_pendingP2;

        if (maxTicks <= 0 || !player || player->m_isDead) {
            pend.active = false;
            actualHold = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // Si ya hay una accion guardada esperando (ej. un PRESS temprano) y el usuario
        // suelta el raton antes de que ese PRESS se haya disparado, NO cancelamos el PRESS:
        // guardamos el RELEASE en cola para despues de que salte/suba
        if (pend.active && pend.down != down) {
            pend.hasQueuedOpposite = true;
            pend.oppositeDelayTicks = std::max(1, static_cast<int>(g_currentTick - pend.pressedTick));
            return;
        }

        // Si ya podemos dispararlo en este mismo tick de forma segura, pasa directo sin retraso
        if (shouldFireNow(pl, player, actualHold, down, maxTicks)) {
            pend.active = false;
            pend.hasQueuedOpposite = false;
            actualHold = down;
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        // De lo contrario, retenemos el input hasta encontrar el tick optimo dentro de `maxTicks`
        pend.active = true;
        pend.down = down;
        pend.button = button;
        pend.isPlayer1 = isPlayer1;
        pend.pressedTick = g_currentTick;
        pend.maxTicks = maxTicks;
        pend.hasQueuedOpposite = false;
    }
};
