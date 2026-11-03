// XCAM VR Videos – nativer OpenXR-Betrachter für VR180/360/Flach-Fotos auf Meta Quest.
#include <android/log.h>
#include <android/bitmap.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

#define LOG_TAG "XcamVideo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

#define XR_CHECK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { LOGE("%s fehlgeschlagen: %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); } } while (0)

// ------------------------------------------------------------------ Mathe (spaltenweise 4x4)
struct Mat4 { float m[16]; };

static Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

static Mat4 fromPose(const XrPosef& p, bool withTranslation = true) {
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    Mat4 r{};
    r.m[0] = 1 - 2 * (y * y + z * z); r.m[1] = 2 * (x * y + z * w);     r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);     r.m[5] = 1 - 2 * (x * x + z * z); r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);     r.m[9] = 2 * (y * z - x * w);     r.m[10] = 1 - 2 * (x * x + y * y);
    if (withTranslation) { r.m[12] = p.position.x; r.m[13] = p.position.y; r.m[14] = p.position.z; }
    r.m[15] = 1;
    return r;
}

// Inverse einer starren Transformation (Rotation + Translation)
static Mat4 rigidInverse(const Mat4& a) {
    Mat4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; ++i)
        r.m[12 + i] = -(r.m[0 + i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}

static Mat4 projection(const XrFovf& fov, float n, float f) {
    const float l = tanf(fov.angleLeft), rr = tanf(fov.angleRight);
    const float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    const float w = rr - l, h = u - d;
    Mat4 m{};
    m.m[0] = 2 / w; m.m[8] = (rr + l) / w;
    m.m[5] = 2 / h; m.m[9] = (u + d) / h;
    m.m[10] = -(f + n) / (f - n); m.m[14] = -2 * f * n / (f - n);
    m.m[11] = -1;
    return m;
}

static Mat4 translateScale(float tx, float ty, float tz, float sx, float sy) {
    Mat4 m{};
    m.m[0] = sx; m.m[5] = sy; m.m[10] = 1; m.m[15] = 1;
    m.m[12] = tx; m.m[13] = ty; m.m[14] = tz;
    return m;
}

static double nowSec() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ------------------------------------------------------------------ Shader
static const char* kBgVS = R"(#version 300 es
layout(location=0) in vec3 aPos;
uniform mat4 uViewProj;
out vec3 vDir;
void main(){ vDir = aPos; gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

// Modi: 1 VR180 3D nebeneinander, 2 VR180 2D, 3 360 2D, 4 360 3D übereinander, 5 Flach 2D, 6 Flach 3D nebeneinander
static const char* kBgFS = R"(#version 300 es
precision highp float;
in vec3 vDir;
uniform sampler2D uTex;
uniform int uMode;
uniform int uEye;
uniform float uEyeAspect;
uniform float uHasImage;
uniform float uBright;
uniform float uSat;
uniform vec3 uWb;
out vec4 oColor;
const float PI = 3.14159265359;
void main(){
    vec3 d = normalize(vDir);
    float lon = atan(d.x, -d.z);
    float lat = asin(clamp(d.y, -1.0, 1.0));
    vec2 uv = vec2(0.0);
    bool ok = uHasImage > 0.5;
    float eye = float(uEye);
    if (uMode == 1 || uMode == 2) {
        if (abs(lon) > PI * 0.5) ok = false;
        uv = vec2(lon / PI + 0.5, 0.5 - lat / PI);
        uv.x = clamp(uv.x, 0.0005, 0.9995);
        if (uMode == 1) uv.x = uv.x * 0.5 + eye * 0.5;
    } else if (uMode == 3 || uMode == 4) {
        uv = vec2(lon / (2.0 * PI) + 0.5, 0.5 - lat / PI);
        if (uMode == 4) uv.y = clamp(uv.y, 0.001, 0.999) * 0.5 + eye * 0.5;
    } else {
        if (d.z > -0.05) { ok = false; }
        else {
            float t = -3.0 / d.z;
            vec3 p = d * t;
            float W = 4.8; float H = W / uEyeAspect;
            if (H > 3.2) { H = 3.2; W = H * uEyeAspect; }
            uv = vec2(p.x / W + 0.5, 0.5 - p.y / H);
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) ok = false;
            if (uMode == 6) uv.x = uv.x * 0.5 + eye * 0.5;
        }
    }
    // Saum bei 360° vermeiden: Ableitungen über den Sprung von atan() hinweg korrigieren
    vec2 gx = dFdx(uv), gy = dFdy(uv);
    if (abs(gx.x) > 0.25) gx.x -= sign(gx.x) * (uMode == 1 ? 0.5 : 1.0);
    if (abs(gy.x) > 0.25) gy.x -= sign(gy.x) * (uMode == 1 ? 0.5 : 1.0);
    if (ok) {
        vec3 c = textureGrad(uTex, uv, gx, gy).rgb;
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = max(mix(vec3(l), c, uSat), 0.0) * uWb * uBright;
        oColor = vec4(min(c, vec3(1.0)), 1.0);
    } else {
        float v = 0.03 + 0.02 * max(0.0, d.y);
        oColor = vec4(v, v, v * 1.2, 1.0);
    }
}
)";

// Farbkorrektur-Durchgang: Foto 1:1 in die Ebene kopieren, dabei Sättigung anwenden
static const char* kGradeVS = R"(#version 300 es
void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
)";
static const char* kGradeFS = R"(#version 300 es
precision highp float;
uniform highp sampler2D uTex;
uniform float uSat;
uniform int uH;
out vec4 oColor;
void main(){
    ivec2 p = ivec2(gl_FragCoord.xy);
    p.y = uH - 1 - p.y;   // OpenGL zählt von unten
    vec3 c = texelFetch(uTex, p, 0).rgb;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(l), c, uSat), 0.0, 1.0);
    oColor = vec4(c, 1.0);
}
)";

// Videobild (Android-SurfaceTexture) 1:1 in die Ebene kopieren
static const char* kOesVS = R"(#version 300 es
out vec2 vUV;
void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); vUV = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
)";
static const char* kOesFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES uTex;
uniform mat4 uM;
uniform float uSat;
in vec2 vUV;
out vec4 oColor;
void main(){
    vec2 uv = (uM * vec4(vUV, 0.0, 1.0)).xy;
    vec3 c = texture(uTex, uv).rgb;
    c = pow(max(c, vec3(0.0)), vec3(2.2));   // Video ist sRGB-kodiert, die Ebene erwartet linear
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(l), c, uSat), 0.0, 1.0);
    oColor = vec4(c, 1.0);
}
)";

static const char* kQuadVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
uniform mat4 uMVP;
out vec2 vUV;
void main(){ vUV = vec2(aPos.x + 0.5, 0.5 - aPos.y); gl_Position = uMVP * vec4(aPos, 0.0, 1.0); }
)";

static const char* kQuadFS = R"(#version 300 es
precision mediump float;
in vec2 vUV;
uniform sampler2D uTex;
uniform float uAlpha;
out vec4 oColor;
void main(){ oColor = texture(uTex, vUV) * uAlpha; }
)";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); LOGE("Shader: %s", log); }
    return s;
}

static GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); LOGE("Programm: %s", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ------------------------------------------------------------------ Java-Brücke
struct Java {
    JNIEnv* env = nullptr;
    jobject act = nullptr;
    jmethodID getImageCount, getStartIndex, getImageName, requestImage, pollImage, makeLabel, consumeJump, getBrightness, setBrightness, getSetting, putSetting, openGallery, getPhotoSettings, savePhotoSettings, getVideoSize, startVideoM, stopVideoM, togglePause, seekBy, positionMs, durationMs, isPlayingM, getVolume, setVolume, createVideoSurface, updateVideoTexture, releaseVideoSurface;
    jmethodID recycle;

    void init(JNIEnv* e, jobject a) {
        env = e; act = a;
        jclass c = env->GetObjectClass(act);
        getImageCount = env->GetMethodID(c, "getImageCount", "()I");
        getStartIndex = env->GetMethodID(c, "getStartIndex", "()I");
        getImageName = env->GetMethodID(c, "getImageName", "(I)Ljava/lang/String;");
        requestImage = env->GetMethodID(c, "requestImage", "(II)V");
        pollImage = env->GetMethodID(c, "pollImage", "()Landroid/graphics/Bitmap;");
        makeLabel = env->GetMethodID(c, "makeLabel", "(Ljava/lang/String;Ljava/lang/String;)Landroid/graphics/Bitmap;");
        consumeJump = env->GetMethodID(c, "consumeJump", "()I");
        getBrightness = env->GetMethodID(c, "getBrightness", "()F");
        setBrightness = env->GetMethodID(c, "setBrightness", "(F)V");
        getSetting = env->GetMethodID(c, "getSetting", "(Ljava/lang/String;I)I");
        putSetting = env->GetMethodID(c, "putSetting", "(Ljava/lang/String;I)V");
        openGallery = env->GetMethodID(c, "openGallery", "()V");
        getPhotoSettings = env->GetMethodID(c, "getPhotoSettings", "(I)Ljava/lang/String;");
        savePhotoSettings = env->GetMethodID(c, "savePhotoSettings", "(ILjava/lang/String;)V");
        getVideoSize = env->GetMethodID(c, "getVideoSize", "(I)[I");
        startVideoM = env->GetMethodID(c, "startVideo", "(ILandroid/view/Surface;)V");
        stopVideoM = env->GetMethodID(c, "stopVideo", "()V");
        togglePause = env->GetMethodID(c, "togglePause", "()Z");
        seekBy = env->GetMethodID(c, "seekBy", "(I)V");
        positionMs = env->GetMethodID(c, "positionMs", "()I");
        durationMs = env->GetMethodID(c, "durationMs", "()I");
        isPlayingM = env->GetMethodID(c, "isPlaying", "()Z");
        getVolume = env->GetMethodID(c, "getVolume", "()F");
        setVolume = env->GetMethodID(c, "setVolume", "(F)V");
        createVideoSurface = env->GetMethodID(c, "createVideoSurface", "(III)Landroid/view/Surface;");
        updateVideoTexture = env->GetMethodID(c, "updateVideoTexture", "([F)Z");
        releaseVideoSurface = env->GetMethodID(c, "releaseVideoSurface", "()V");
        jclass bc = env->FindClass("android/graphics/Bitmap");
        recycle = env->GetMethodID(bc, "recycle", "()V");
        env->DeleteLocalRef(bc);
        env->DeleteLocalRef(c);
        check();
    }
    void check() { if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); } }
    int count() { int r = env->CallIntMethod(act, getImageCount); check(); return r; }
    int start() { int r = env->CallIntMethod(act, getStartIndex); check(); return r; }
    int jump() { int r = env->CallIntMethod(act, consumeJump); check(); return r; }
    float brightness() { float r = env->CallFloatMethod(act, getBrightness); check(); return r > 0.f ? r : 1.f; }
    void saveBrightness(float v) { env->CallVoidMethod(act, setBrightness, v); check(); }
    void videoSize(int i, int& w, int& h) {
        w = h = 0;
        jintArray arr = (jintArray)env->CallObjectMethod(act, getVideoSize, i); check();
        if (!arr) return;
        jint v[2] = {0, 0};
        if (env->GetArrayLength(arr) >= 2) env->GetIntArrayRegion(arr, 0, 2, v);
        env->DeleteLocalRef(arr);
        w = v[0]; h = v[1];
    }
    void startVideo(int i, jobject surface) { env->CallVoidMethod(act, startVideoM, i, surface); check(); }
    void stopVideo() { env->CallVoidMethod(act, stopVideoM); check(); }
    bool toggle() { bool r = env->CallBooleanMethod(act, togglePause); check(); return r; }
    void seek(int ms) { env->CallVoidMethod(act, seekBy, ms); check(); }
    int pos() { int r = env->CallIntMethod(act, positionMs); check(); return r; }
    int dur() { int r = env->CallIntMethod(act, durationMs); check(); return r; }
    bool playing() { bool r = env->CallBooleanMethod(act, isPlayingM); check(); return r; }
    float volume() { float r = env->CallFloatMethod(act, getVolume); check(); return r; }
    void setVol(float v) { env->CallVoidMethod(act, setVolume, v); check(); }
    std::string photoSettings(int i) {
        jstring s = (jstring)env->CallObjectMethod(act, getPhotoSettings, i); check();
        if (!s) return "";
        const char* c = env->GetStringUTFChars(s, nullptr);
        std::string r = c ? c : "";
        env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        return r;
    }
    void savePhoto(int i, const std::string& v) {
        jstring s = env->NewStringUTF(v.c_str());
        env->CallVoidMethod(act, savePhotoSettings, i, s); check();
        env->DeleteLocalRef(s);
    }
    int setting(const char* key, int def) {
        jstring k = env->NewStringUTF(key);
        int r = env->CallIntMethod(act, getSetting, k, def); check();
        env->DeleteLocalRef(k); return r;
    }
    void saveSetting(const char* key, int v) {
        jstring k = env->NewStringUTF(key);
        env->CallVoidMethod(act, putSetting, k, v); check();
        env->DeleteLocalRef(k);
    }
    void request(int i, int maxSize) { env->CallVoidMethod(act, requestImage, i, maxSize); check(); }
    std::string name(int i) {
        jstring s = (jstring)env->CallObjectMethod(act, getImageName, i); check();
        if (!s) return "";
        const char* c = env->GetStringUTFChars(s, nullptr);
        std::string r = c ? c : "";
        env->ReleaseStringUTFChars(s, c);
        env->DeleteLocalRef(s);
        return r;
    }
};

// Bitmap → GL-Textur (sRGB, optional mit Mipmaps). Gibt false zurück, wenn nichts hochgeladen wurde.
static bool uploadBitmap(Java& j, jobject bmp, GLuint& tex, int& outW, int& outH, bool mips) {
    if (!bmp) return false;
    AndroidBitmapInfo info;
    bool ok = false;
    void* px = nullptr;
    if (AndroidBitmap_getInfo(j.env, bmp, &info) == ANDROID_BITMAP_RESULT_SUCCESS &&
        info.format == ANDROID_BITMAP_FORMAT_RGBA_8888 &&
        AndroidBitmap_lockPixels(j.env, bmp, &px) == ANDROID_BITMAP_RESULT_SUCCESS) {
        if (tex) glDeleteTextures(1, &tex);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        int levels = 1;
        if (mips) { int m = (int)std::max(info.width, info.height); while (m > 1) { m >>= 1; ++levels; } }
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, info.width, info.height);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(info.stride / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info.width, info.height, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (mips) glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        AndroidBitmap_unlockPixels(j.env, bmp);
        outW = (int)info.width; outH = (int)info.height;
        ok = true;
    } else {
        LOGE("Bitmap konnte nicht gelesen werden");
    }
    j.env->CallVoidMethod(bmp, j.recycle); j.check();
    j.env->DeleteLocalRef(bmp);
    return ok;
}

struct App;
static bool uploadPhoto(App& a, jobject bmp);

// ------------------------------------------------------------------ App-Zustand
struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
};

struct App {
    android_app* android = nullptr;
    bool resumed = false;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, focused = false, exitRequested = false;
    Swapchain swapchains[2];
    std::vector<XrViewConfigurationView> viewConfigs;

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction actNext, actPrev, actMode, actSwap, actExit, actStick, actStickY, actFilter, actLeftX, actLeftY, actReset, actResetBright;
    bool stickLatched = false;
    float brightness = 1.f;
    int shownPct = 100;
    double lastInput = 0;
    double lastSeek = 0;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    GLuint bgProg = 0, quadProg = 0, cubeVao = 0, cubeVbo = 0, cubeIbo = 0, quadVao = 0, quadVbo = 0;
    GLint bgViewProj, bgTex, bgMode, bgEye, bgAspect, bgHas, bgBright, bgSat, bgWb, qMVP, qTex, qAlpha;

    Java java;
    int count = 0, index = 0;
    int maxTex = 8192;
    GLuint imageTex = 0; int imgW = 0, imgH = 0; bool loading = false;
    int userMode = 0;   // 0 = Auto
    bool swapEyes = false;
    GLuint labelTex = 0; int labelW = 0, labelH = 0; double labelUntil = 0;

    // Darstellung als Compositor-Ebene (schärfer: nur einmal umgerechnet)
    bool extEquirect = false, extSettings = false, extColorScale = false;
    int64_t colorFormat = GL_SRGB8_ALPHA8;
    XrSwapchain imgSwapchain = XR_NULL_HANDLE;
    int filterMode = 0;   // 0 Supersampling, 1 Schärfen, 2 aus
    bool galleryRequested = false;
    bool helpShown = false;

    // Video
    bool extSurface = false;
    jobject surfaceRef = nullptr;
    GLuint oesTex = 0, oesProg = 0, oesVao = 0; GLint oTex = -1, oM = -1, oSat = -1;
    std::vector<XrSwapchainImageOpenGLESKHR> vImgs;
    std::vector<GLuint> vFbos;
    bool videoHasFrame = false;
    jfloatArray matArr = nullptr;
    XrAction actPlay = XR_NULL_HANDLE;
    float volume = 1.f; int shownVol = 100;
    double osdUntil = 0, lastOsd = 0;

    // Farbe: Sättigung (0..2, 1 = normal) und Weißpunkt (-1 kühler … +1 wärmer)
    float saturation = 1.f, whitePoint = 0.f;
    int shownSat = 100, shownWb = 0;
    bool gradeDirty = false; double lastGrade = 0;
    GLuint gradeProg = 0, gradeVao = 0; GLint gTex = -1, gSat = -1, gH = -1;
};

// Weißpunkt als Farbfaktoren (wärmer = mehr Rot, weniger Blau)
static void whiteScale(float wp, float out[3]) {
    out[0] = 1.f + 0.3f * wp;
    out[1] = 1.f + 0.03f * wp;
    out[2] = 1.f - 0.3f * wp;
}

// Foto (eigene Textur) mit Farbkorrektur in eine neue statische OpenXR-Swapchain rendern.
// Die Swapchain wird danach direkt von der Quest als Ebene angezeigt.
static bool regrade(App& a) {
    if (!a.imageTex || a.imgW <= 0 || a.imgH <= 0) return false;
    const int W = a.imgW, H = a.imgH;
    int levels = 1;
    { int m = std::max(W, H); while (m > 1) { m >>= 1; ++levels; } }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = a.colorFormat;
    ci.sampleCount = 1;
    ci.width = (uint32_t)W; ci.height = (uint32_t)H;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = (uint32_t)levels;
    XrSwapchain sc = XR_NULL_HANDLE;
    XrResult r = xrCreateSwapchain(a.session, &ci, &sc);
    if (XR_FAILED(r)) {
        LOGI("Swapchain mit Mipmaps nicht möglich (%d), ohne", (int)r);
        levels = 1; ci.mipCount = 1;
        r = xrCreateSwapchain(a.session, &ci, &sc);
        if (XR_FAILED(r)) { LOGE("Foto-Swapchain: %d", (int)r); return false; }
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc, &acq, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc, &wi);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imgs[idx].image, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glViewport(0, 0, W, H);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        glUseProgram(a.gradeProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, a.imageTex);
        glUniform1i(a.gTex, 0);
        glUniform1f(a.gSat, a.saturation);
        glUniform1i(a.gH, H);
        glBindVertexArray(a.gradeVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        if (levels > 1) {
            glBindTexture(GL_TEXTURE_2D, imgs[idx].image);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        LOGE("Framebuffer für Foto-Ebene unvollständig");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glFlush();
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc, &rel);
    if (!ok) { xrDestroySwapchain(sc); return false; }
    if (a.imgSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(a.imgSwapchain);
    a.imgSwapchain = sc;
    a.gradeDirty = false;
    a.lastGrade = nowSec();
    return true;
}

static const char* kModeNames[] = {
    "Auto", "VR180 3D (nebeneinander)", "VR180 2D", "360° 2D", "360° 3D (übereinander)", "Flach 2D", "Flach 3D (nebeneinander)"
};

static int autoMode(int w, int h) {
    if (w <= 0 || h <= 0) return 1;
    float a = (float)w / (float)h;
    if (a > 1.9f && a < 2.1f) return 1;      // XCAM: zwei 180°-Bilder nebeneinander (VR180 3D)
    if (a > 0.95f && a < 1.05f) return 2;    // einzelnes 180°-Bild
    if (a > 3.3f && a < 3.8f) return 6;      // zwei 16:9 nebeneinander
    return 5;
}

static int effectiveMode(App& a) { return a.userMode != 0 ? a.userMode : autoMode(a.imgW, a.imgH); }

static bool useLayers(App& a) { return a.imgSwapchain != XR_NULL_HANDLE && a.videoHasFrame; }

static const char* kFilterNames[] = { "Supersampling", "Schärfen", "Filter aus" };

static bool uploadPhoto(App& a, jobject bmp) {
    // Foto als eigene Textur behalten (Quelle für die Farbkorrektur); für die Ebene ohne Mipmaps
    const bool layers = a.extEquirect;
    if (!uploadBitmap(a.java, bmp, a.imageTex, a.imgW, a.imgH, !layers)) return false;
    if (layers && a.imgW > 16) {
        if (regrade(a)) return true;
        // Rückfall auf die einfache Darstellung (Shader) mit derselben Textur
        glBindTexture(GL_TEXTURE_2D, a.imageTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (a.imgSwapchain != XR_NULL_HANDLE) { xrDestroySwapchain(a.imgSwapchain); a.imgSwapchain = XR_NULL_HANDLE; }
    return true;
}

static void showLabel(App& a, const std::string& l1, const std::string& l2, double seconds = 3.0) {
    JNIEnv* env = a.java.env;
    jstring s1 = env->NewStringUTF(l1.c_str()), s2 = env->NewStringUTF(l2.c_str());
    jobject bmp = env->CallObjectMethod(a.java.act, a.java.makeLabel, s1, s2);
    a.java.check();
    env->DeleteLocalRef(s1); env->DeleteLocalRef(s2);
    if (uploadBitmap(a.java, bmp, a.labelTex, a.labelW, a.labelH, false)) a.labelUntil = nowSec() + seconds;
}

static std::string modeLine(App& a) {
    std::string m = a.userMode == 0 ? std::string("Auto: ") + kModeNames[autoMode(a.imgW, a.imgH)] : kModeNames[a.userMode];
    if (a.swapEyes) m += " · Augen getauscht";
    if (useLayers(a)) { if (a.extSettings) { m += " · "; m += kFilterNames[a.filterMode]; } }
    else m += " · einfache Darstellung";
    return m;
}

static std::string titleLine(App& a) {
    return std::to_string(a.index + 1) + "/" + std::to_string(a.count) + "  " + a.java.name(a.index);
}

// Einstellungen pro Foto: Helligkeit; Sättigung; Weißpunkt; Modus; Augen getauscht
static void savePhotoSettings(App& a) {
    if (a.count <= 0) return;
    char buf[96];
    snprintf(buf, sizeof buf, "%.3f;%.3f;%.3f;%d;%d", a.brightness, a.saturation, a.whitePoint, a.userMode, a.swapEyes ? 1 : 0);
    a.java.savePhoto(a.index, buf);
}

static void applyPhotoSettings(App& a) {
    float b = 1.f, sat = 1.f, wp = 0.f; int mode = 0, swap = 0;
    std::string v = a.java.photoSettings(a.index);
    if (!v.empty()) sscanf(v.c_str(), "%f;%f;%f;%d;%d", &b, &sat, &wp, &mode, &swap);
    a.brightness = std::min(3.f, std::max(0.25f, b));
    a.saturation = std::min(2.f, std::max(0.f, sat));
    a.whitePoint = std::min(1.f, std::max(-1.f, wp));
    a.userMode = (mode >= 0 && mode <= 6) ? mode : 0;
    a.swapEyes = swap != 0;
    a.shownPct = (int)lroundf(a.brightness * 20.f) * 5;
    a.shownSat = (int)lroundf(a.saturation * 20.f) * 5;
    a.shownWb = (int)lroundf(a.whitePoint * 20.f) * 5;
}

static std::string fmtClock(int ms) {
    int s = std::max(0, ms / 1000);
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

// Einblendung mit Titel und Zeit (wird regelmäßig aufgefrischt, solange sie sichtbar ist)
static void showOsd(App& a, double seconds = 3.0) {
    const int d = a.java.dur(), p = a.java.pos();
    std::string line2 = d > 0 ? std::string(a.java.playing() ? "▶  " : "❚❚  ") + fmtClock(p) + " / " + fmtClock(d) : "Lädt …";
    line2 += "   ·   Lautstärke " + std::to_string(a.shownVol) + " %";
    showLabel(a, titleLine(a), line2, seconds + 0.6);
    a.osdUntil = nowSec() + seconds;
    a.lastOsd = nowSec();
}

static void stopVideo(App& a) {
    a.java.stopVideo();
    a.java.env->CallVoidMethod(a.java.act, a.java.releaseVideoSurface); a.java.check();
    if (!a.vFbos.empty()) { glDeleteFramebuffers((GLsizei)a.vFbos.size(), a.vFbos.data()); a.vFbos.clear(); }
    a.vImgs.clear();
    if (a.imgSwapchain != XR_NULL_HANDLE) { xrDestroySwapchain(a.imgSwapchain); a.imgSwapchain = XR_NULL_HANDLE; }
    if (a.surfaceRef) { a.java.env->DeleteGlobalRef(a.surfaceRef); a.surfaceRef = nullptr; }
    if (a.oesTex) { glDeleteTextures(1, &a.oesTex); a.oesTex = 0; }
    a.videoHasFrame = false;
}

// Video über eine SurfaceTexture dekodieren und jedes Bild 1:1 in eine OpenXR-Swapchain kopieren,
// die die Quest als Ebene (360°/180°) zeichnet.
static bool startVideoAt(App& a) {
    stopVideo(a);
    int w = 0, h = 0;
    a.java.videoSize(a.index, w, h);
    if (w <= 0 || h <= 0) { showLabel(a, titleLine(a), "Video kann nicht gelesen werden", 4.0); return false; }
    if (!a.extEquirect) { showLabel(a, "Video-Ebenen nicht verfügbar", "Diese Quest-Software unterstützt das nicht", 6.0); return false; }

    // Swapchain in Videogröße
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = a.colorFormat;
    ci.sampleCount = 1;
    ci.width = (uint32_t)w; ci.height = (uint32_t)h;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrSwapchain sc = XR_NULL_HANDLE;
    XrResult r = xrCreateSwapchain(a.session, &ci, &sc);
    if (XR_FAILED(r)) {
        LOGE("Video-Swapchain %dx%d: %d", w, h, (int)r);
        showLabel(a, titleLine(a), "Video-Ebene konnte nicht erzeugt werden (Fehler " + std::to_string((int)r) + ")", 6.0);
        return false;
    }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(sc, 0, &n, nullptr);
    a.vImgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader*)a.vImgs.data());
    a.vFbos.resize(n);
    glGenFramebuffers((GLsizei)n, a.vFbos.data());
    for (uint32_t k = 0; k < n; ++k) {
        glBindFramebuffer(GL_FRAMEBUFFER, a.vFbos[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.vImgs[k].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    a.imgSwapchain = sc;

    // Externe Textur + Surface für den MediaPlayer
    glGenTextures(1, &a.oesTex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, a.oesTex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    jobject surface = a.java.env->CallObjectMethod(a.java.act, a.java.createVideoSurface, (jint)a.oesTex, (jint)w, (jint)h);
    a.java.check();
    if (!surface) { showLabel(a, titleLine(a), "Video-Surface konnte nicht erzeugt werden", 6.0); stopVideo(a); return false; }
    a.surfaceRef = a.java.env->NewGlobalRef(surface);
    a.java.env->DeleteLocalRef(surface);
    if (!a.matArr) {
        jfloatArray m = a.java.env->NewFloatArray(16);
        a.matArr = (jfloatArray)a.java.env->NewGlobalRef(m);
        a.java.env->DeleteLocalRef(m);
    }
    a.imgW = w; a.imgH = h;
    a.videoHasFrame = false;
    a.java.startVideo(a.index, a.surfaceRef);
    LOGI("Video %d: %dx%d", a.index, w, h);
    return true;
}

// Neues Videobild abholen und in die Swapchain kopieren
static void pumpVideo(App& a) {
    if (a.imgSwapchain == XR_NULL_HANDLE || !a.oesTex || a.vImgs.empty()) return;
    JNIEnv* env = a.java.env;
    jboolean fresh = env->CallBooleanMethod(a.java.act, a.java.updateVideoTexture, a.matArr);
    a.java.check();
    if (!fresh) return;
    float M[16];
    env->GetFloatArrayRegion(a.matArr, 0, 16, M);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(a.imgSwapchain, &acq, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(a.imgSwapchain, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, a.vFbos[idx]);
    glViewport(0, 0, a.imgW, a.imgH);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glUseProgram(a.oesProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, a.oesTex);
    glUniform1i(a.oTex, 0);
    glUniformMatrix4fv(a.oM, 1, GL_FALSE, M);
    glUniform1f(a.oSat, a.saturation);
    glBindVertexArray(a.oesVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(a.imgSwapchain, &rel);
    a.videoHasFrame = true;
}

static void loadIndex(App& a, int i) {
    if (a.count <= 0) return;
    a.index = ((i % a.count) + a.count) % a.count;
    applyPhotoSettings(a);
    a.loading = false;
    if (startVideoAt(a)) {
        if (!a.helpShown) {
            a.helpShown = true;
            showLabel(a, "Rechts: A Play/Pause · ←→ spulen · ↑↓ Helligkeit · B nächstes · Trigger = Übersicht",
                         "Links: ↑↓ Lautstärke · ←→ Weißpunkt · X Modus · Y Augen · Trigger vorheriges Video", 6.0);
            a.osdUntil = 0;
        } else {
            showOsd(a);
        }
    }
}

// ------------------------------------------------------------------ Android-Lebenszyklus
static void onCmd(android_app* app, int32_t cmd) {
    App& a = *(App*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME: a.resumed = true; break;
        case APP_CMD_PAUSE: a.resumed = false; break;
        case APP_CMD_DESTROY: a.exitRequested = true; break;
        default: break;
    }
}

// ------------------------------------------------------------------ Initialisierung
static bool initEgl(App& a) {
    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(a.display, nullptr, nullptr);
    const EGLint attribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint n = 0;
    if (!eglChooseConfig(a.display, attribs, &a.config, 1, &n) || n < 1) { LOGE("Keine EGL-Konfiguration"); return false; }
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, ctxAttr);
    const EGLint sAttr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    a.surface = eglCreatePbufferSurface(a.display, a.config, sAttr);
    if (!eglMakeCurrent(a.display, a.surface, a.surface, a.context)) { LOGE("eglMakeCurrent fehlgeschlagen"); return false; }
    return true;
}

static void initGl(App& a) {
    a.bgProg = program(kBgVS, kBgFS);
    a.bgViewProj = glGetUniformLocation(a.bgProg, "uViewProj");
    a.bgTex = glGetUniformLocation(a.bgProg, "uTex");
    a.bgMode = glGetUniformLocation(a.bgProg, "uMode");
    a.bgEye = glGetUniformLocation(a.bgProg, "uEye");
    a.bgAspect = glGetUniformLocation(a.bgProg, "uEyeAspect");
    a.bgHas = glGetUniformLocation(a.bgProg, "uHasImage");
    a.bgBright = glGetUniformLocation(a.bgProg, "uBright");
    a.bgSat = glGetUniformLocation(a.bgProg, "uSat");
    a.bgWb = glGetUniformLocation(a.bgProg, "uWb");
    a.gradeProg = program(kGradeVS, kGradeFS);
    a.gTex = glGetUniformLocation(a.gradeProg, "uTex");
    a.gSat = glGetUniformLocation(a.gradeProg, "uSat");
    a.gH = glGetUniformLocation(a.gradeProg, "uH");
    glGenVertexArrays(1, &a.gradeVao);
    a.oesProg = program(kOesVS, kOesFS);
    a.oTex = glGetUniformLocation(a.oesProg, "uTex");
    a.oM = glGetUniformLocation(a.oesProg, "uM");
    a.oSat = glGetUniformLocation(a.oesProg, "uSat");
    glGenVertexArrays(1, &a.oesVao);
    a.quadProg = program(kQuadVS, kQuadFS);
    a.qMVP = glGetUniformLocation(a.quadProg, "uMVP");
    a.qTex = glGetUniformLocation(a.quadProg, "uTex");
    a.qAlpha = glGetUniformLocation(a.quadProg, "uAlpha");

    const float s = 10.f;
    const float cube[] = { -s,-s,-s,  s,-s,-s,  s, s,-s, -s, s,-s,  -s,-s, s,  s,-s, s,  s, s, s, -s, s, s };
    const unsigned short idx[] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,  3,2,6, 3,6,7,  0,3,7, 0,7,4,  1,5,6, 1,6,2 };
    glGenVertexArrays(1, &a.cubeVao);
    glBindVertexArray(a.cubeVao);
    glGenBuffers(1, &a.cubeVbo); glBindBuffer(GL_ARRAY_BUFFER, a.cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    glGenBuffers(1, &a.cubeIbo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, a.cubeIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    const float quad[] = { -0.5f,-0.5f, 0.5f,-0.5f, 0.5f,0.5f, -0.5f,-0.5f, 0.5f,0.5f, -0.5f,0.5f };
    glGenVertexArrays(1, &a.quadVao);
    glBindVertexArray(a.quadVao);
    glGenBuffers(1, &a.quadVbo); glBindBuffer(GL_ARRAY_BUFFER, a.quadVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    GLint mt = 4096; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
    a.maxTex = std::min(mt, 8192);
    LOGI("Maximale Texturgröße: %d", a.maxTex);
}

static XrPath path(App& a, const char* s) { XrPath p; xrStringToPath(a.instance, s, &p); return p; }

static XrAction makeAction(App& a, const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction act = XR_NULL_HANDLE;
    XR_CHECK(xrCreateAction(a.actionSet, &ci, &act));
    return act;
}

static void initActions(App& a) {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "viewer");
    strcpy(si.localizedActionSetName, "Betrachter");
    XR_CHECK(xrCreateActionSet(a.instance, &si, &a.actionSet));
    a.actNext = makeAction(a, "next", "Weiter", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actPrev = makeAction(a, "prev", "Zurueck", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actMode = makeAction(a, "mode", "Modus", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actSwap = makeAction(a, "swap", "Augen tauschen", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actExit = makeAction(a, "exit", "Beenden", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actStick = makeAction(a, "stick", "Stick", XR_ACTION_TYPE_FLOAT_INPUT);
    a.actStickY = makeAction(a, "sticky", "Helligkeit", XR_ACTION_TYPE_FLOAT_INPUT);
    a.actLeftX = makeAction(a, "whitepoint", "Weisspunkt", XR_ACTION_TYPE_FLOAT_INPUT);
    a.actLeftY = makeAction(a, "saturation", "Saettigung", XR_ACTION_TYPE_FLOAT_INPUT);
    a.actReset = makeAction(a, "reset", "Farben zuruecksetzen", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actResetBright = makeAction(a, "resetbright", "Helligkeit zuruecksetzen", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actFilter = makeAction(a, "filter", "Bildfilter", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a.actPlay = makeAction(a, "play", "Abspielen/Pause", XR_ACTION_TYPE_BOOLEAN_INPUT);

    std::vector<XrActionSuggestedBinding> touch = {
        {a.actPlay, path(a, "/user/hand/right/input/a/click")},
        {a.actNext, path(a, "/user/hand/right/input/b/click")},
        {a.actPrev, path(a, "/user/hand/left/input/trigger/value")},
        {a.actMode, path(a, "/user/hand/left/input/x/click")},
        {a.actSwap, path(a, "/user/hand/left/input/y/click")},
        {a.actExit, path(a, "/user/hand/left/input/menu/click")},
        {a.actExit, path(a, "/user/hand/right/input/trigger/value")},
        {a.actStick, path(a, "/user/hand/right/input/thumbstick/x")},
        {a.actStickY, path(a, "/user/hand/right/input/thumbstick/y")},
        {a.actLeftX, path(a, "/user/hand/left/input/thumbstick/x")},
        {a.actLeftY, path(a, "/user/hand/left/input/thumbstick/y")},
        {a.actReset, path(a, "/user/hand/left/input/thumbstick/click")},
        {a.actResetBright, path(a, "/user/hand/right/input/thumbstick/click")},
        {a.actFilter, path(a, "/user/hand/left/input/squeeze/value")},
        {a.actFilter, path(a, "/user/hand/right/input/squeeze/value")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(a, "/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = touch.data();
    sb.countSuggestedBindings = (uint32_t)touch.size();
    XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &sb));

    std::vector<XrActionSuggestedBinding> simple = {
        {a.actPlay, path(a, "/user/hand/right/input/select/click")},
        {a.actPrev, path(a, "/user/hand/left/input/select/click")},
        {a.actExit, path(a, "/user/hand/left/input/menu/click")},
    };
    sb.interactionProfile = path(a, "/interaction_profiles/khr/simple_controller");
    sb.suggestedBindings = simple.data();
    sb.countSuggestedBindings = (uint32_t)simple.size();
    XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &sb));

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1; ai.actionSets = &a.actionSet;
    XR_CHECK(xrAttachSessionActionSets(a.session, &ai));
}

static bool initXr(App& a) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = a.android->activity->vm;
        li.applicationContext = a.android->activity->clazz;
        initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&li);
    }

    // Verfügbare Erweiterungen abfragen
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data());
    auto has = [&](const char* n) {
        for (auto& p : props) if (strcmp(p.extensionName, n) == 0) return true;
        return false;
    };
    std::vector<const char*> exts = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    if (has(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME)) { exts.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME); a.extEquirect = true; }
    if (has(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) { exts.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME); a.extSettings = true; }
    if (has(XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME)) { exts.push_back(XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME); a.extSurface = true; }
    if (has(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME)) { exts.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME); a.extColorScale = true; }
    LOGI("Erweiterungen: equirect2=%d settings=%d colorscale=%d", a.extEquirect, a.extSettings, a.extColorScale);
    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = a.android->activity->vm;
    ai.applicationActivity = a.android->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.enabledExtensionNames = exts.data();
    strcpy(ci.applicationInfo.applicationName, "XcamVideo");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "none");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrResult r = xrCreateInstance(&ci, &a.instance);
    if (XR_FAILED(r)) { LOGE("xrCreateInstance: %d", (int)r); return false; }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(a.instance, &sgi, &a.systemId);
    if (XR_FAILED(r)) { LOGE("xrGetSystem: %d", (int)r); return false; }

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(a.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(a.instance, a.systemId, &req);

    if (!initEgl(a)) return false;
    initGl(a);

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = a.display; gb.config = a.config; gb.context = a.context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb; sci.systemId = a.systemId;
    r = xrCreateSession(a.instance, &sci, &a.session);
    if (XR_FAILED(r)) { LOGE("xrCreateSession: %d", (int)r); return false; }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.viewSpace));

    // Swapchains
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(a.instance, a.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    a.viewConfigs.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(a.instance, a.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, a.viewConfigs.data());
    if (n < 2) { LOGE("Keine Stereo-Ansicht"); return false; }

    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(a.session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(a.session, fc, &fc, formats.data());
    int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t f : formats) if (f == GL_SRGB8_ALPHA8) { format = f; break; }
    a.colorFormat = format;
    LOGI("Swapchain-Format: 0x%llx", (long long)format);

    for (int e = 0; e < 2; ++e) {
        Swapchain& sc = a.swapchains[e];
        const auto& vc = a.viewConfigs[e];
        // etwas Supersampling für mehr Bildschärfe
        sc.width = (int32_t)std::min<uint32_t>(vc.maxImageRectWidth, (uint32_t)(vc.recommendedImageRectWidth * 1.3f));
        sc.height = (int32_t)std::min<uint32_t>(vc.maxImageRectHeight, (uint32_t)(vc.recommendedImageRectHeight * 1.3f));
        XrSwapchainCreateInfo sci2{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci2.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci2.format = format;
        sci2.sampleCount = 1;
        sci2.width = sc.width; sci2.height = sc.height;
        sci2.faceCount = 1; sci2.arraySize = 1; sci2.mipCount = 1;
        r = xrCreateSwapchain(a.session, &sci2, &sc.handle);
        if (XR_FAILED(r)) { LOGE("xrCreateSwapchain: %d", (int)r); return false; }
        uint32_t ic = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &ic, nullptr);
        sc.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(sc.handle, ic, &ic, (XrSwapchainImageBaseHeader*)sc.images.data());
        sc.fbos.resize(ic);
        glGenFramebuffers((GLsizei)ic, sc.fbos.data());
        for (uint32_t i = 0; i < ic; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[i].image, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    initActions(a);
    return true;
}

// ------------------------------------------------------------------ Ereignisse & Eingabe
static void pollXrEvents(App& a) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                a.exitRequested = true; break;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = (XrEventDataSessionStateChanged*)&ev;
                a.state = e->state;
                LOGI("Session-Zustand: %d", (int)a.state);
                if (a.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(a.session, &bi));
                    a.running = true;
                } else if (a.state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xrEndSession(a.session));
                    a.running = false;
                } else if (a.state == XR_SESSION_STATE_EXITING || a.state == XR_SESSION_STATE_LOSS_PENDING) {
                    a.exitRequested = true;
                }
                a.focused = (a.state == XR_SESSION_STATE_FOCUSED);
                break;
            }
            default: break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static bool pressed(App& a, XrAction act) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = act;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(a.session, &gi, &st))) return false;
    return st.isActive && st.changedSinceLastSync && st.currentState;
}

static void handleInput(App& a) {
    if (!a.focused) return;
    XrActiveActionSet as{a.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &as;
    if (XR_FAILED(xrSyncActions(a.session, &si))) return;

    int step = 0;
    if (pressed(a, a.actNext)) step = 1;
    if (pressed(a, a.actPrev)) step = -1;

    const double now = nowSec();
    const float dt = (float)std::min(0.1, std::max(0.0, now - a.lastInput));
    a.lastInput = now;

    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    XrActionStateFloat fs{XR_TYPE_ACTION_STATE_FLOAT};
    float sx = 0.f, sy = 0.f;
    gi.action = a.actStick;
    if (XR_SUCCEEDED(xrGetActionStateFloat(a.session, &gi, &fs)) && fs.isActive) sx = fs.currentState;
    XrActionStateFloat fy{XR_TYPE_ACTION_STATE_FLOAT};
    gi.action = a.actStickY;
    if (XR_SUCCEEDED(xrGetActionStateFloat(a.session, &gi, &fy)) && fy.isActive) sy = fy.currentState;

    // Stick links/rechts: 10 Sekunden spulen (gedrückt halten = weiter spulen)
    if (fabsf(sx) > 0.7f && fabsf(sx) > fabsf(sy)) {
        if (!a.stickLatched || now - a.lastSeek > 0.35) {
            a.java.seek(sx > 0 ? 10000 : -10000);
            a.lastSeek = now;
            a.stickLatched = true;
            showOsd(a);
        }
    } else if (fabsf(sx) < 0.3f) a.stickLatched = false;

    if (pressed(a, a.actPlay)) { a.java.toggle(); showOsd(a); }

    // Stick oben/unten: Helligkeit (stufenlos, 25 % … 300 %)
    if (fabsf(sy) > 0.25f && fabsf(sy) > fabsf(sx)) {
        const float speed = (fabsf(sy) - 0.25f) / 0.75f;  // 0..1
        a.brightness *= expf((sy > 0 ? 1.f : -1.f) * speed * 0.9f * dt);
        a.brightness = std::min(3.f, std::max(0.25f, a.brightness));
        int pct = (int)lroundf(a.brightness * 20.f) * 5;
        if (pct != a.shownPct) {
            a.shownPct = pct;
            showLabel(a, "Helligkeit", std::to_string(pct) + " %", 1.5);
            savePhotoSettings(a);
        }
    }

    // Linker Stick: oben/unten Sättigung, links/rechts Weißpunkt
    float lx = 0.f, ly = 0.f;
    XrActionStateFloat flx{XR_TYPE_ACTION_STATE_FLOAT}, fly{XR_TYPE_ACTION_STATE_FLOAT};
    gi.action = a.actLeftX;
    if (XR_SUCCEEDED(xrGetActionStateFloat(a.session, &gi, &flx)) && flx.isActive) lx = flx.currentState;
    gi.action = a.actLeftY;
    if (XR_SUCCEEDED(xrGetActionStateFloat(a.session, &gi, &fly)) && fly.isActive) ly = fly.currentState;
    // Linker Stick oben/unten: Lautstärke
    if (fabsf(ly) > 0.25f && fabsf(ly) > fabsf(lx)) {
        const float speed = (fabsf(ly) - 0.25f) / 0.75f;
        a.volume = std::min(1.f, std::max(0.f, a.volume + (ly > 0 ? 1.f : -1.f) * speed * 0.6f * dt));
        int pct = (int)lroundf(a.volume * 20.f) * 5;
        if (pct != a.shownVol) {
            a.shownVol = pct;
            a.java.setVol(a.volume);
            showLabel(a, "Lautstärke", std::to_string(pct) + " %", 1.5);
            a.osdUntil = 0;
        }
    }
    if (fabsf(lx) > 0.25f && fabsf(lx) > fabsf(ly)) {
        const float speed = (fabsf(lx) - 0.25f) / 0.75f;
        a.whitePoint = std::min(1.f, std::max(-1.f, a.whitePoint + (lx > 0 ? 1.f : -1.f) * speed * 0.6f * dt));
        int v = (int)lroundf(a.whitePoint * 20.f) * 5;
        if (v != a.shownWb) {
            a.shownWb = v;
            std::string t = v == 0 ? "neutral" : (v > 0 ? "wärmer +" + std::to_string(v) : "kühler " + std::to_string(v));
            showLabel(a, "Weißpunkt", t, 1.5);
            savePhotoSettings(a);
        }
    }
    // Linken Stick drücken: Sättigung und Weißpunkt zurück auf normal
    if (pressed(a, a.actReset)) {
        a.whitePoint = 0.f; a.shownWb = 0;
        savePhotoSettings(a);
        showLabel(a, "Weißpunkt", "neutral", 1.5);
    }
    // Rechten Stick drücken: Helligkeit zurück auf 100 %
    if (pressed(a, a.actResetBright)) {
        a.brightness = 1.f; a.shownPct = 100;
        savePhotoSettings(a);
        showLabel(a, "Helligkeit", "100 %", 1.5);
    }
    a.gradeDirty = false;

    if (step != 0 && a.count > 1) loadIndex(a, a.index + step);

    if (pressed(a, a.actMode)) {
        a.userMode = (a.userMode + 1) % 7;
        savePhotoSettings(a);
        showLabel(a, "Modus", modeLine(a));
    }
    if (pressed(a, a.actSwap)) {
        a.swapEyes = !a.swapEyes;
        savePhotoSettings(a);
        showLabel(a, a.swapEyes ? "Augen getauscht" : "Augen normal", modeLine(a));
    }
    if (pressed(a, a.actFilter)) {
        if (useLayers(a) && a.extSettings) {
            a.filterMode = (a.filterMode + 1) % 3;
            a.java.saveSetting("filter", a.filterMode);
            showLabel(a, "Bildfilter", kFilterNames[a.filterMode]);
        } else {
            showLabel(a, "Bildfilter", "auf diesem Gerät nicht verfügbar");
        }
    }
    if (pressed(a, a.actExit)) { a.galleryRequested = true; a.exitRequested = true; }
}

static void updateImage(App& a) {
    pumpVideo(a);
    int j = a.java.jump();
    if (j >= 0) { a.count = a.java.count(); loadIndex(a, j); }
    const double now = nowSec();
    if (now < a.osdUntil && now - a.lastOsd > 0.5) showOsd(a, a.osdUntil - now);
}

// ------------------------------------------------------------------ Rendern
static void renderEye(App& a, int eye, const XrView& view, const XrPosef& head, double t, bool layersOnly) {
    Swapchain& sc = a.swapchains[eye];
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &acq, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);

    glBindFramebuffer(GL_FRAMEBUFFER, sc.fbos[idx]);
    glViewport(0, 0, sc.width, sc.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    if (layersOnly) glClearColor(0, 0, 0, 0);   // durchsichtig, das Foto liegt darunter als Ebene
    else glClearColor(0.03f, 0.03f, 0.04f, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    const Mat4 proj = projection(view.fov, 0.05f, 100.f);
    const Mat4 eyeView = rigidInverse(fromPose(view.pose));
    const Mat4 rotView = rigidInverse(fromPose(view.pose, false));

    // Hintergrund (Foto auf Kugel/Ebene, Richtung pro Pixel berechnet) – nur ohne Compositor-Ebene
    const int mode = effectiveMode(a);
    if (!layersOnly) {
    glUseProgram(a.bgProg);
    Mat4 vp = mul(proj, rotView);
    glUniformMatrix4fv(a.bgViewProj, 1, GL_FALSE, vp.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a.imageTex);
    glUniform1i(a.bgTex, 0);
    glUniform1i(a.bgMode, mode);
    glUniform1i(a.bgEye, a.swapEyes ? 1 - eye : eye);
    float eyeAspect = a.imgH > 0 ? (float)a.imgW / (float)a.imgH : 1.f;
    if (mode == 6) eyeAspect *= 0.5f;
    glUniform1f(a.bgAspect, eyeAspect);
    glUniform1f(a.bgHas, a.imageTex ? 1.f : 0.f);
    glUniform1f(a.bgBright, a.brightness);
    glUniform1f(a.bgSat, a.saturation);
    { float ws[3]; whiteScale(a.whitePoint, ws); glUniform3f(a.bgWb, ws[0], ws[1], ws[2]); }
    glBindVertexArray(a.cubeVao);
    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    }

    // Einblendung (kopffest, unten in der Mitte)
    if (a.labelTex && t < a.labelUntil) {
        float alpha = (float)std::min(1.0, (a.labelUntil - t) / 0.5);
        Mat4 model = mul(fromPose(head), translateScale(0, -0.28f, -1.1f, 0.62f, 0.62f * a.labelH / std::max(1, a.labelW)));
        Mat4 mvp = mul(proj, mul(eyeView, model));
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(a.quadProg);
        glUniformMatrix4fv(a.qMVP, 1, GL_FALSE, mvp.m);
        glBindTexture(GL_TEXTURE_2D, a.labelTex);
        glUniform1i(a.qTex, 0);
        glUniform1f(a.qAlpha, alpha);
        glBindVertexArray(a.quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &rel);
}

static void renderFrame(App& a) {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(a.session, &fwi, &fs))) return;
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(a.session, &fbi))) return;

    handleInput(a);
    updateImage(a);

    XrCompositionLayerProjectionView projViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[4];
    uint32_t layerCount = 0;
    XrCompositionLayerEquirect2KHR eq[2] = {{XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR}, {XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR}};
    XrCompositionLayerQuad qd[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    XrCompositionLayerColorScaleBiasKHR colorScale{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
    const bool layersOnly = useLayers(a);

    if (fs.shouldRender) {
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = a.localSpace;
        uint32_t vc = 0;
        XrResult r = xrLocateViews(a.session, &vli, &vs, 2, &vc, views);
        bool valid = XR_SUCCEEDED(r) && vc == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
        if (valid) {
            XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
            XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
            if (XR_SUCCEEDED(xrLocateSpace(a.viewSpace, a.localSpace, fs.predictedDisplayTime, &hl)) &&
                (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) head = hl.pose;
            const double t = nowSec();
            if (layersOnly) {
                // Verkettung der Zusatzinfos: Qualität (Supersampling/Schärfen) und Helligkeit
                const void* chain = nullptr;
                if (a.extColorScale) {
                    float ws[3]; whiteScale(a.whitePoint, ws);
                    colorScale.colorScale = {a.brightness * ws[0], a.brightness * ws[1], a.brightness * ws[2], 1.f};
                    colorScale.colorBias = {0, 0, 0, 0};
                    colorScale.next = chain; chain = &colorScale;
                }
                if (a.extSettings && a.filterMode != 2) {
                    settings.layerFlags = a.filterMode == 0 ? XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB
                                                            : XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
                    settings.next = chain; chain = &settings;
                }
                const int mode = effectiveMode(a);
                const int W = a.imgW, H = a.imgH;
                const bool stereoLR = (mode == 1 || mode == 6), stereoTB = (mode == 4);
                const bool stereo = stereoLR || stereoTB;
                const float PI = 3.14159265f;
                for (int e = 0; e < (stereo ? 2 : 1); ++e) {
                    // welcher Bildteil gehört zu diesem Auge?
                    int part = stereo ? (a.swapEyes ? 1 - e : e) : 0;
                    XrRect2Di rect{{0, 0}, {W, H}};
                    if (stereoLR) rect = {{part * (W / 2), 0}, {W / 2, H}};
                    if (stereoTB) rect = {{0, (1 - part) * (H / 2)}, {W, H / 2}};   // GL: oben = großes y
                    const XrEyeVisibility vis = stereo ? (e == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT)
                                                       : XR_EYE_VISIBILITY_BOTH;
                    const XrSwapchainSubImage sub{a.imgSwapchain, rect, 0};
                    if (mode >= 1 && mode <= 4) {
                        XrCompositionLayerEquirect2KHR& L = eq[e];
                        L.next = chain;
                        L.layerFlags = 0;
                        L.space = a.localSpace;
                        L.eyeVisibility = vis;
                        L.subImage = sub;
                        L.pose = {{0, 0, 0, 1}, {0, 0, 0}};
                        L.radius = 0.f;   // unendlich weit weg
                        L.centralHorizontalAngle = (mode <= 2) ? PI : 2.f * PI;
                        L.upperVerticalAngle = PI / 2.f;
                        L.lowerVerticalAngle = -PI / 2.f;
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&L;
                    } else {
                        float aspect = (float)rect.extent.width / (float)std::max(1, rect.extent.height);
                        float qw = 4.8f, qh = qw / aspect;
                        if (qh > 3.2f) { qh = 3.2f; qw = qh * aspect; }
                        XrCompositionLayerQuad& L = qd[e];
                        L.next = chain;
                        L.layerFlags = 0;
                        L.space = a.localSpace;
                        L.eyeVisibility = vis;
                        L.subImage = sub;
                        L.pose = {{0, 0, 0, 1}, {0, 0, -3.f}};
                        L.size = {qw, qh};
                        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&L;
                    }
                }
            }
            for (int e = 0; e < 2; ++e) {
                renderEye(a, e, views[e], head, t, layersOnly);
                projViews[e].pose = views[e].pose;
                projViews[e].fov = views[e].fov;
                projViews[e].subImage.swapchain = a.swapchains[e].handle;
                projViews[e].subImage.imageRect = {{0, 0}, {a.swapchains[e].width, a.swapchains[e].height}};
                projViews[e].subImage.imageArrayIndex = 0;
            }
            layer.space = a.localSpace;
            layer.layerFlags = layersOnly ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            layer.viewCount = 2;
            layer.views = projViews;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&layer;
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : nullptr;
    XR_CHECK(xrEndFrame(a.session, &fei));
}

// ------------------------------------------------------------------ Einstieg
void android_main(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    App a;
    a.android = app;
    app->userData = &a;
    app->onAppCmd = onCmd;

    a.java.init(env, app->activity->clazz);

    if (!initXr(a)) {
        LOGE("OpenXR-Initialisierung fehlgeschlagen");
        ANativeActivity_finish(app->activity);
    } else {
        a.filterMode = std::min(2, std::max(0, a.java.setting("filter", 0)));
        a.volume = std::min(1.f, std::max(0.f, a.java.volume()));
        a.shownVol = (int)lroundf(a.volume * 20.f) * 5;
        a.lastInput = nowSec();
        a.count = a.java.count();
        if (a.count > 0) loadIndex(a, a.java.start());
        else showLabel(a, "Keine Videos", "Zuerst in der App Videos laden oder in Movies/XCAM kopieren", 600.0);
    }

    bool finishCalled = false;
    while (!app->destroyRequested) {
        for (;;) {
            int events = 0;
            android_poll_source* src = nullptr;
            const int timeout = (!a.resumed && !a.running && !app->destroyRequested) ? 250 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, (void**)&src) < 0) break;
            if (src) src->process(app, src);
            if (app->destroyRequested) break;
        }
        if (app->destroyRequested) break;
        if (a.session == XR_NULL_HANDLE) continue;

        pollXrEvents(a);
        if (a.exitRequested && !finishCalled) {
            finishCalled = true;
            if (a.running) xrRequestExitSession(a.session);
            if (a.galleryRequested) { a.java.env->CallVoidMethod(a.java.act, a.java.openGallery); a.java.check(); }
            else ANativeActivity_finish(app->activity);
        }
        if (!a.running) continue;
        renderFrame(a);
    }

    if (a.session != XR_NULL_HANDLE) {
        stopVideo(a);
        for (auto& sc : a.swapchains) if (sc.handle) xrDestroySwapchain(sc.handle);
        if (a.imgSwapchain) xrDestroySwapchain(a.imgSwapchain);
        if (a.actionSet) xrDestroyActionSet(a.actionSet);
        if (a.localSpace) xrDestroySpace(a.localSpace);
        if (a.viewSpace) xrDestroySpace(a.viewSpace);
        xrDestroySession(a.session);
    }
    if (a.instance) xrDestroyInstance(a.instance);
    if (a.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(a.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (a.surface != EGL_NO_SURFACE) eglDestroySurface(a.display, a.surface);
        if (a.context != EGL_NO_CONTEXT) eglDestroyContext(a.display, a.context);
        eglTerminate(a.display);
    }
    app->activity->vm->DetachCurrentThread();
}
