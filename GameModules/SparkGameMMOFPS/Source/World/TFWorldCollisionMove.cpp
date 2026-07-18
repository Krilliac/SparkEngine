/**
 * @file TFWorldCollisionMove.cpp
 * @brief The shared capsule-sweep move resolver (server integrator + client
 *        prediction): horizontal sweep-and-slide plus the collision-v2 vertical
 *        resolve. Scene parse/Build lives in TFWorldCollision.cpp and the
 *        W10/W11 decor-OBB registration in TFWorldCollisionDecor.cpp (same
 *        class, split per the repo file-size rules — mirrors the
 *        TFWorldSetup/-Net split).
 */
#include "World/TFWorldCollision.h"

#include "Game/TFMovementModel.h" // kTFPawnRadiusM / kTFPawnHeightM

#include "Physics/PhysicsSystem.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    // -----------------------------------------------------------------------
    // Shared move resolver (server integrator + client prediction)
    // -----------------------------------------------------------------------

    void TFWorldCollision::ResolveMove(const float prevPos[3], float pos[3], float vel[3], bool* grounded) const
    {
        if (!IsActive())
            return;

        bool anyHit = false;

        // -------------------------------------------------------------------
        // 1) Horizontal sweep + slide (unchanged from the 2026-07-10 wave).
        // -------------------------------------------------------------------
        float curX = pos[0]; // resolved column defaults to the integrated XZ
        float curZ = pos[2]; // (vertical resolve below runs even without XZ motion)

        float remX = pos[0] - prevPos[0];
        float remZ = pos[2] - prevPos[2];
        if (remX * remX + remZ * remZ >= 1.0e-10f)
        {
            // Swept capsule: bottom lifted kTFStepUpM above the feet so standing on
            // plateau-flush mesa tops (and stepping over low curbs) never registers
            // as a wall hit; top at the pawn head. CapsuleCastFiltered's `height` is
            // the cylindrical section (total = height + 2 * radius).
            const float capTotal = kTFPawnHeightM - kTFStepUpM;          // 1.45
            const float capCyl = capTotal - 2.0f * kTFPawnRadiusM;       // 0.65
            const float centerY = pos[1] + kTFStepUpM + capTotal * 0.5f; // feet + 1.075

            curX = prevPos[0];
            curZ = prevPos[2];

            for (int iter = 0; iter < kTFSlideIters; ++iter)
            {
                const float len = std::sqrt(remX * remX + remZ * remZ);
                if (len < 1.0e-5f)
                    break;

                const XMFLOAT3 from{curX, centerY, curZ};
                const XMFLOAT3 to{curX + remX, centerY, curZ + remZ};
                const RaycastHit hit =
                    m_physics->CapsuleCastFiltered(kTFPawnRadiusM, capCyl, from, to, CollisionLayers::MovementMask);
                if (!hit.hasHit)
                {
                    curX += remX;
                    curZ += remZ;
                    break;
                }
                anyHit = true;

                const float dirX = remX / len;
                const float dirZ = remZ / len;
                const float travel = std::clamp(hit.distance - kTFMoveSkinM, 0.0f, len);
                curX += dirX * travel;
                curZ += dirZ * travel;

                // Slide the remainder along the surface (horizontal normal only;
                // shape-cast normals are true outward unit normals as of today).
                float nX = hit.normal.x;
                float nZ = hit.normal.z;
                const float nLen = std::sqrt(nX * nX + nZ * nZ);
                if (nLen < 1.0e-4f)
                    break; // floor/ceiling contact: nothing to slide along
                nX /= nLen;
                nZ /= nLen;

                float leftX = dirX * (len - travel);
                float leftZ = dirZ * (len - travel);
                const float into = leftX * nX + leftZ * nZ;
                if (into < 0.0f)
                {
                    leftX -= into * nX;
                    leftZ -= into * nZ;
                }
                remX = leftX;
                remZ = leftZ;

                // Kill the velocity component pushing into the surface so the next
                // tick does not re-accelerate into the wall.
                const float vInto = vel[0] * nX + vel[2] * nZ;
                if (vInto < 0.0f)
                {
                    vel[0] -= vInto * nX;
                    vel[2] -= vInto * nZ;
                }
            }
        }

        pos[0] = curX;
        pos[2] = curZ;

        // -------------------------------------------------------------------
        // 2) Vertical resolve at the resolved column (collision-v2). Uses the
        //    FULL-height capsule (bottom at the feet — no step-up lift, or the
        //    pawn would sink kTFStepUpM into every roof before contact).
        //    TFMoveStep's terrain clamp already ran, so a falling pawn may
        //    arrive here already snapped to terrain BELOW a roof — sweeping
        //    from prevPos.y catches the roof on the way down regardless. The
        //    caller's terrain re-clamp stays the floor-of-last-resort.
        // -------------------------------------------------------------------
        const float fullCyl = kTFPawnHeightM - 2.0f * kTFPawnRadiusM; // 1.0
        const float halfTotal = kTFPawnHeightM * 0.5f;                // 0.9
        const float dy = pos[1] - prevPos[1];

        if (dy > 1.0e-6f && vel[1] > 0.0f)
        {
            // Rising (jump): stop under static-body undersides. One cheap cast;
            // any hit blocks — the horizontal pass keeps the column skin-clear
            // of walls, so a straight-up sweep only meets real overheads.
            const XMFLOAT3 from{curX, prevPos[1] + halfTotal, curZ};
            const XMFLOAT3 to{curX, prevPos[1] + halfTotal + dy, curZ};
            const RaycastHit hit =
                m_physics->CapsuleCastFiltered(kTFPawnRadiusM, fullCyl, from, to, CollisionLayers::MovementMask);
            if (hit.hasHit)
            {
                pos[1] = prevPos[1] + std::clamp(hit.distance - kTFMoveSkinM, 0.0f, dy);
                if (vel[1] > 0.0f)
                    vel[1] = 0.0f; // head bonk: kill the ascent
                anyHit = true;
            }
        }
        else if (vel[1] <= 0.0f && dy <= 1.0e-6f)
        {
            // Descending (or standing on a body): land on static-body tops.
            // kTFLandSnapM extends the sweep past the integrated position so
            // the per-tick gravity droop (~5 mm at 60 Hz) keeps re-finding the
            // roof through the kTFMoveSkinM hover gap — grounded stays stable
            // instead of strobing (friction/jump would flicker otherwise).
            const float sweepLen = -dy + kTFLandSnapM;
            const XMFLOAT3 from{curX, prevPos[1] + halfTotal, curZ};
            const XMFLOAT3 to{curX, prevPos[1] + halfTotal - sweepLen, curZ};
            const RaycastHit hit =
                m_physics->CapsuleCastFiltered(kTFPawnRadiusM, fullCyl, from, to, CollisionLayers::MovementMask);
            if (hit.hasHit && hit.normal.y > kTFWalkableNormalY)
            {
                // Walkable top: stand skin-height above the contact. Side/edge
                // grazes (normal mostly horizontal) are NOT landings — the pawn
                // keeps falling and the terrain clamp remains the backstop.
                pos[1] = prevPos[1] - std::clamp(hit.distance - kTFMoveSkinM, 0.0f, sweepLen);
                if (vel[1] < 0.0f)
                    vel[1] = 0.0f;
                if (grounded)
                    *grounded = true; // only ever SET here; TFMoveStep owns clearing
                anyHit = true;
            }
        }

        if (anyHit)
            ++m_blockedMoves; // diagnostic only (tf_validate); see header note
    }

} // namespace Terrafront
