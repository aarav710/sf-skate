// Rendering: the city shader (lighting, sun shadows, contact shadow, fog, surface detail, wet streets, water),
// sun shadow maps, the off-screen scene target and the post pass (bloom, motion blur, color grading).
#pragma once
#include "common.h"

// Everything the city shader needs that changes from frame to frame.
struct Look {
    Vector3 camPos, sunDir, sunCol, amb, fog, sky;
    float fogDen, night, time;
    float wet = 0;            // 0 dry .. 1 soaked streets (rain)
    float fogH = 300;         // haze scale height: low for Karl the Fog, which pools in the valleys
};

// The post pass: color grading per time of day, bloom, speed blur.
struct Grade {
    Vector3 gain{1, 1, 1}, lift{0, 0, 0};
    float sat = 1, contrast = 1, bloom = 0.5f, vignette = 0.25f, blur = 0;
};

struct Renderer {
    Shader city{}, cityFar{}, depth{}, post{}, blur{}, extract{};
    int quality = 2;           // 2 high (two shadow cascades, bloom, 4x MSAA), 1 medium (one cascade), 0 low (no shadows / bloom)
    static const char* QUALITY_NAMES[3];

    // sun shadows
    static constexpr int MAXC = 2;
    unsigned shFbo[MAXC]{}, shTex[MAXC]{};
    int shSize = 2048;
    int nCasc = 0;             // cascades rendered this frame
    Matrix lightView[MAXC]{}, lightVP[MAXC]{};
    float half[MAXC]{};

    // off-screen scene: multisampled color + depth, resolved into `scene`; alpha = 1 - bloom
    unsigned msFbo = 0, msColor = 0, msDepth = 0;
    int msSamples = 0;
    RenderTexture2D scene{}, bloomA{}, bloomB{}, bloomC{};
    int w = 0, h = 0;

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
    // opaque city passes write a bloom mask into alpha; translucent bits must leave it alone
    void opaque(bool on);
    void endScene();                // resolve, bloom
    void present(const Grade& g);   // draw the graded scene to the screen

private:
    Matrix savedP{}, savedMV{};
    int vp[4]{};
    int locUseVN = -1, locShadow[MAXC]{}, locLVP[MAXC]{}, locShInfo = -1;
    float shStrength = 1;
    void ensureTargets();
    void freeTargets();
};
