#pragma once
#include "raymath.h"
#include <algorithm>

// The sky's view basis from the camera's own up (cam.up), orthogonalised against the view direction, so a rolled or
// inverted camera (the seaplane) rotates the sky with the world. Falls back to world up, then to a fixed axis, when
// cam.up is degenerate (zero, or parallel to the view direction).
inline void skyBasis(Vector3 fwd, Vector3 camUp, Vector3& right, Vector3& up) {
    const Vector3 tries[3] = {camUp, {0, 1, 0}, {1, 0, 0}};
    for (const Vector3& u : tries) {
        Vector3 r = Vector3CrossProduct(fwd, u);
        if (Vector3Length(r) > 1e-3f * std::max(Vector3Length(u), 1e-6f)) {
            right = Vector3Normalize(r);
            up = Vector3CrossProduct(right, fwd);
            return;
        }
    }
    right = {1, 0, 0};
    up = {0, 1, 0};
}
