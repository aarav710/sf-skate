// Rendering: the sky, the city shader (lighting, sun shadows, street lamps, contact shadow, fog, aerial haze, surface
// detail, wet streets, water), sun shadow maps, the off-screen scene target and the post pass (ambient occlusion,
// bloom, motion blur, color grading, tone mapping).
#pragma once
#include "common.h"

// Everything the city shader needs that changes from frame to frame.
struct Look {
    Vector3 camPos, sunDir, sunCol, amb, fog, sky;
    float fogDen, night, time;
    float wet = 0;            // 0 dry .. 1 soaked streets (rain)
    float fogH = 300;         // haze scale height: low for Karl the Fog, which pools in the valleys
};

// The sky behind everything: gradient, haze at the horizon, sun or moon, and a drifting cloud layer.
struct SkyLook {
    Vector3 fwd, right, up;   // camera basis
    float tanHalf, aspect;    // tan(fovy / 2), width / height
    Vector3 sunDir, sunCol, top, horizon, fog;
    float night, time;
    float rain = 0, fogK = 0; // overcast 0..1, Karl the Fog 0..1
};

// The post pass: color grading per time of day, bloom, speed blur.
struct Grade {
    Vector3 gain{1, 1, 1}, lift{0, 0, 0};
    float sat = 1, contrast = 1, bloom = 0.5f, vignette = 0.25f, blur = 0;
};

struct Renderer {
    Shader city{}, cityFar{}, depth{}, post{}, blur{}, extract{}, sky{}, ssao{}, aoBlur{};
    // 2 high (two shadow cascades + a wide far one, ambient occlusion, HDR), 1 medium (one cascade), 0 low (no shadows
    // or bloom). 4x MSAA and bloom on all but low.
    int quality = 2;
    static const char* QUALITY_NAMES[3];

    // sun shadows
    static constexpr int MAXC = 3;   // 0, 1 near the camera; 2 the wide far one (High), redrawn only when it moves
    unsigned shFbo[MAXC]{}, shTex[MAXC]{};
    int shSize = 2048;
    int nCasc = 0;             // cascades rendered this frame
    Matrix lightView[MAXC]{}, lightVP[MAXC]{};
    float half[MAXC]{};
    bool farCasc = false;      // the far cascade is in use this frame
    bool farDirty = true;      // ... and needs drawing again (moved, or the sun turned)

    static constexpr int MAXLAMPS = 16;

    // off-screen scene: multisampled color + depth, resolved into `scene`; alpha = 1 - bloom. Half-float on High
    // (HDR: lights and the sun don't clip, the post pass tone maps them).
    unsigned msFbo = 0, msColor = 0, msDepth = 0;
    int msSamples = 0;
    bool hdr = false;
    RenderTexture2D scene{}, bloomA{}, bloomB{}, bloomC{};
    int w = 0, h = 0;
    // ambient occlusion (High): the near pass's depth resolved, AO at half size (r = AO, g = view depth) and blurred
    unsigned dsFbo = 0, dsTex = 0;
    RenderTexture2D aoA{}, aoB{};
    float viewFovy = 60, viewNear = 0.15f, viewFar = 270;

    void init();
    void setQuality(int q);
    void setLook(const Look& L);
    // static meshes carry real normals; immediate-mode models fall back to flat face normals
    void staticNormals(bool on);

    // shadow pass: plan the cascades around the camera, then for each one begin / draw casters / end
    int planShadows(Vector3 camPos, Vector3 camFwd, Vector3 sunDir, float strength);
    void beginCascade(int k);
    void endCascade();

    void beginScene(Color clear);   // binds the off-screen target (call inside BeginDrawing)
    void drawSky(const SkyLook& s); // right after beginScene: fills the background, bloom mask included
    float cloudAt(Vector3 dir) const;   // cloud cover (0..1) the last drawSky showed in this direction (for the stars)
    // street lamps near the camera: xyz and strength (0 = off); applied by the near pass
    void setLamps(const Vector4* l, int n);
    void setView(float fovyDeg, float nearZ, float farZ) { viewFovy = fovyDeg; viewNear = nearZ; viewFar = farZ; }
    // opaque city passes write a bloom mask into alpha; translucent bits must leave it alone
    void opaque(bool on);
    void endScene();                // resolve, bloom
    void present(const Grade& g);   // draw the graded scene to the screen

private:
    Matrix savedP{}, savedMV{};
    int vp[4]{};
    int locUseVN = -1, locShadow[MAXC]{}, locLVP[MAXC]{}, locShInfo = -1;
    float shStrength = 1;
    Matrix shProj[MAXC]{};
    Vector3 farAt{1e9f, 0, 0}, farSun{};   // where the far cascade was last drawn, and for which sun
    SkyLook lastSky{};
    void ambientOcclusion();
    void ensureTargets();
    void freeTargets();
};
