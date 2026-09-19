#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <android/choreographer.h>
#include <android/log.h>

#include <stdbool.h>
#include <X11/Xdefs.h>
#include <X11/keysymdef.h>
#include <jni.h>
#include <screenint.h>
#include <errno.h>
#include <sys/socket.h>
#include "linux/input-event-codes.h"
#include "buffer.h"

#define PORT 7892
#define MAGIC "0xDEADBEEF"

/* Private Present capability bits shared with the matching Mesa loader.
 * Standard Present capabilities currently occupy the low bits.  These bits
 * describe Termux:X11 implementation details; clients which do not know them
 * simply ignore them.
 */
#define LORIE_PRESENT_CAP_BACKEND_RELEASE         (1u << 27)
#define LORIE_PRESENT_CAP_ACTUAL_FEEDBACK         (1u << 28)
#define LORIE_PRESENT_CAP_WAIT_FENCE_REQUEUE_SAFE (1u << 29)
#define LORIE_PRESENT_CAP_VBLANK_COMPLETE         (1u << 30)
#define LORIE_PRESENT_CAP_FRAME_TIMELINE          (1u << 31)
/* The server attaches the API-33 Choreographer deadline, expected-present
 * time, and opportunity MSC to private backend-release events.  Keep this
 * separate from FRAME_TIMELINE so a new Mesa does not assume that older
 * experimental servers publish the payload merely because they use the
 * Choreographer callback internally. */
#define LORIE_PRESENT_CAP_TIMELINE_NOTIFY          (1u << 26)
/* Version 2 also identifies the callback at which X contents were selected.
 * Android's later render deadline is not that X-side selection boundary. */
#define LORIE_PRESENT_CAP_TIMELINE_OPPORTUNITY     (1u << 25)

/* Per-request opt-in.  The matching Mesa loader only sets this after the
 * capability above was advertised, so unmodified servers never see it. */
#define LORIE_PRESENT_OPTION_BACKEND_RELEASE      (1u << 30)
#define LORIE_PRESENT_OPTION_ACTUAL_FEEDBACK      (1u << 31)
#define LORIE_PRESENT_COMPLETE_KIND_ACTUAL        2
#define LORIE_PRESENT_COMPLETE_KIND_BACKEND_RELEASE 3
#define LORIE_PRESENT_COMPLETE_MODE_ACTUAL        1
#define LORIE_PRESENT_COMPLETE_MODE_UNKNOWN       2
#define LORIE_PRESENT_BACKEND_RELEASE_CONSUMED    1
#define LORIE_PRESENT_BACKEND_RELEASE_RETIRED     2

#define LORIE_PRESENT_FEEDBACK_PRESENTED 1
#define LORIE_PRESENT_FEEDBACK_UNKNOWN   2

/* Versioned cross-process renderer wakeup.  pthread_cond_t deliberately
 * remains the first member so an older X server, which maps only that object,
 * can continue to signal a newer activity.  A matching server sets
 * lockedProtocol after validating magic and the backing-region size. */
#define LORIE_RENDERER_WAKEUP_MAGIC 0x4c525731u
typedef struct {
    pthread_cond_t cond;
    pthread_mutex_t lock;
    uint32_t magic;
    volatile uint32_t sequence;
    volatile uint8_t lockedProtocol;
    uint8_t reserved[3];
} LorieRendererWakeup;

/* DEBUG: Identity of the newest X Present whose contents were incorporated
 * into an Android renderer frame.  tag is server-monotonic and is the
 * publication/correlation key; window and serial retain the originating
 * Present identity without changing standard Present completion semantics.
 * The event and window generation cookies prevent delayed renderer feedback
 * from being delivered to a resource which reused the same XID. */
typedef struct __attribute__((aligned(8))) {
    uint64_t tag;
    uint64_t windowGeneration;
    uint64_t eventGeneration;
    uint32_t window;
    uint32_t serial;
    uint32_t feedbackEid;
    uint32_t options;
} LoriePresentTag;

/* Scheduling metadata for the Android opportunity sampled when a renderer
 * frame begins.  It never establishes producer or consumer completion. */
typedef struct __attribute__((aligned(8))) {
    uint64_t deadlineUs;
    uint64_t expectedUs;
    uint64_t opportunityUs;
    uint64_t opportunityMsc;
    int64_t vsyncId;
} LorieFrameTimeline;

enum {
    LORIE_SURFACECONTROL_REQUEST_NONE = 0,
    LORIE_SURFACECONTROL_REQUEST_FLIP = 1,
    LORIE_SURFACECONTROL_REQUEST_UNFLIP = 2,
};

/* DEBUG: one request is sufficient because Present permits only one pending
 * flip per screen.  The X server is the sole writer; the Android renderer is
 * the sole reader.  Producer completion remains PR96's responsibility. */
typedef struct __attribute__((aligned(8))) {
    volatile uint32_t version;
    uint32_t kind;
    uint64_t eventId;
    uint64_t bufferId;
    uint64_t targetMsc;
} LorieSurfaceControlRequest;

#ifdef __cplusplus
extern "C" {
#endif

struct lorie_shared_server_state;

void lorieConfigureNotify(int width, int height, int framerate, size_t name_size, char* name);
void lorieEnableClipboardSync(Bool enable);
void lorieSendClipboardData(const char* data);
void lorieInitClipboard(void);
void lorieRequestClipboard(void);
void lorieHandleClipboardAnnounce(void);
void lorieHandleClipboardData(const char* data);
void lorieSetStylusEnabled(Bool enabled);
void lorieSyncLockKeysState(uint8_t state);
void lorieWakeServer(void);
void lorieRecheckGpuCopies(void);
void lorieHandlePresentFeedback(uint8_t status, uint32_t surface_generation,
                                uint64_t renderer_serial, uint64_t gpu_copy_serial,
                                LoriePresentTag present_tag,
                                uint64_t egl_frame_id, int64_t submit_ns,
                                int64_t present_ns);
void lorieHandlePresentBackendRelease(uint8_t mode,
                                      LoriePresentTag present_tag,
                                      uint64_t deadline_us,
                                      uint64_t expected_us,
                                      uint64_t opportunity_us,
                                      uint64_t opportunity_msc);
void lorieHandleSurfaceControlComplete(uint64_t event_id,
                                       int64_t observed_present_ns);
void lorieChoreographerStart(AChoreographer *choreographer);
void lorieActivityConnected(void);
void lorieSendSharedServerState(int memfd);
void lorieRegisterBuffer(LorieBuffer* buffer);
void lorieUnregisterBuffer(LorieBuffer* buffer);
bool lorieConnectionAlive(void);
extern bool lorieDebugEnabled; // Set in activity.cpp's startLogcat, only called when TERMUX_X11_DEBUG=1.
void lorieSetRendererWakeupCond(int fd);
void lorieSetCursorVisible(Bool visible);
void lorieSendSyncReply(uint32_t serial);
void registerCmdEntryPointNatives(JNIEnv *env);
void lorieListenForKnocks(void);

__unused void rendererTestCapabilities(int* legacy_drawing, int* gpu_present_disabled,
                                       int* direct_allocation_validated);

static inline __always_inline void lorie_mutex_lock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    // Unfortunately there is no robust mutexes in bionic.
    // Posix does not define any valid way to unlock stuck non-robust mutex
    // so in the case if renderer or X server process unexpectedly die with locked mutex
    // we will simply reinitialize it.
    struct timespec ts = {0};
    while(true) {
        clock_gettime(CLOCK_MONOTONIC, &ts);

        // 33 msec is enough to complete any drawing operation on both X server and renderer side
        // In the case if mutex is locked most likely other thread died with the mutex locked
        ts.tv_nsec += 33UL * 1000000UL;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec  = ts.tv_nsec % 1000000000L;
        }

        int ret = pthread_mutex_timedlock(mutex, &ts);
        if (ret == ETIMEDOUT) {
            if (*lockingPid == getpid() || lorieConnectionAlive())
                continue;

            pthread_mutexattr_t attr;
            pthread_mutex_t initializer = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            memcpy(mutex, &initializer, sizeof(initializer));
            pthread_mutex_init(mutex, &attr);
            // Mutex will be locked fine on the next iteration
        } else {
            *lockingPid = getpid();
            return;
        }
    }
}

static inline __always_inline void lorie_mutex_unlock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    *lockingPid = 0;
    pthread_mutex_unlock(mutex);
}

typedef enum {
    EVENT_UNKNOWN __unused = 0,
    EVENT_SHARED_SERVER_STATE,
    EVENT_ADD_BUFFER,
    EVENT_REMOVE_BUFFER,
    EVENT_SCREEN_SIZE,
    EVENT_TOUCH,
    EVENT_MOUSE,
    EVENT_KEY,
    EVENT_STYLUS,
    EVENT_STYLUS_ENABLE,
    EVENT_UNICODE,
    EVENT_CLIPBOARD_ENABLE,
    EVENT_CLIPBOARD_ANNOUNCE,
    EVENT_CLIPBOARD_REQUEST,
    EVENT_CLIPBOARD_SEND,
    EVENT_WINDOW_FOCUS_CHANGED,
    EVENT_RENDERER_WAKEUP_COND,
    EVENT_GPU_COPY_DONE,
    EVENT_PRESENT_FEEDBACK,
    EVENT_LOCK_KEYS_STATE,
    EVENT_SYNC,
    EVENT_SYNC_REPLY,
    EVENT_PRESENT_BACKEND_RELEASE,
    EVENT_SURFACE_CONTROL_COMPLETE,
} eventType;

typedef union {
    uint8_t type;
    struct {
        uint8_t t;
        uint16_t width, height, framerate;
        size_t name_size;
        char *name;
    } screenSize;
    struct {
        uint8_t t;
        unsigned long id;
    } removeBuffer;
    struct {
        uint8_t t;
        uint16_t type, id, x, y;
    } touch;
    struct {
        uint8_t t;
        float x, y;
        uint8_t detail, down, relative;
    } mouse;
    struct {
        uint8_t t;
        uint16_t key;
        uint8_t state;
    } key;
    struct {
        uint8_t t;
        float x, y;
        uint16_t pressure;
        int8_t tilt_x, tilt_y;
        int16_t orientation;
        uint8_t buttons, eraser, mouse;
    } stylus;
    struct {
        uint8_t t, enable;
    } stylusEnable;
    struct {
        uint8_t t;
        uint32_t code;
    } unicode;
    struct {
        uint8_t t;
        uint8_t enable;
    } clipboardEnable;
    struct {
        uint8_t t;
        uint32_t count;
    } clipboardSend;
    struct {
        uint8_t t;
        uint8_t state; // bit0 = Caps Lock, bit1 = Num Lock, bit2 = Scroll Lock
    } lockKeysState;
    struct {
        uint8_t t;
        uint8_t status;
        uint16_t reserved;
        uint32_t surfaceGeneration;
        uint64_t rendererSerial;
        uint64_t gpuCopySerial;
        LoriePresentTag presentTag;
        uint64_t eglFrameId;
        int64_t submitNs;
        int64_t presentNs;
    } presentFeedback;
    struct {
        uint8_t t;
        uint8_t mode;
        uint16_t reserved;
        LoriePresentTag presentTag;
        /* API-33 scheduling metadata only.  These fields are zero on the
         * compatibility path and never establish producer completion. */
        uint64_t deadlineUs;
        uint64_t expectedUs;
        uint64_t opportunityUs;
        uint64_t opportunityMsc;
    } presentBackendRelease;
    struct {
        uint8_t t;
        uint32_t serial;
    } sync;
    struct {
        uint8_t t;
        uint8_t reserved[7];
        uint64_t eventId;
        int64_t observedPresentNs;
    } surfaceControlComplete;
} lorieEvent;

typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;

#define LORIE_GPU_COPY_MAX_RECTS 16
#define LORIE_GPU_COPY_QUEUE_CAPACITY 8

typedef struct {
    uint64_t serial;
    uint64_t srcBufferId;
    uint64_t dstBufferId;
    LoriePresentTag presentTag;
    int16_t xOff, yOff;
    uint16_t numRects;
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
} LorieGpuCopyEntry;

struct lorie_shared_server_state {
    /*
     * Renderer and X server are separated into 2 different processes.
     * Root window and cursor content and properties are shared across these 2 processes.
     * Reading/drawing root window in renderer the same time X server writes it can cause
     * tearing, texture garbling and other visual artifacts so we should block X server while we are drawing.
     */
    pthread_mutex_t lock; // initialized at X server side.
    pid_t lockingPid;

    /*
     * Single-producer (X server, present_execute_copy)/single-consumer (renderer) ring buffer
     * of deferred GPU copies to be applied to the root window texture before it is drawn to screen.
     * X server only ever advances writeIndex, renderer only ever advances readIndex and completedSerial.
     */
    struct {
        volatile uint32_t writeIndex;
        volatile uint32_t readIndex;
        volatile uint64_t completedSerial;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;

    /* DEBUG: seqlock-style publication of CPU-copy/flip Present identity.
     * The X server is the sole writer and the renderer is the sole reader.
     * An odd version is being written; an unchanged even version is a
     * consistent snapshot.  claimedTag identifies the exact slot value the
     * renderer has either attached to a submission or explicitly retired.
     * Both sides update it while holding the root-buffer lock, so replacing
     * an unclaimed value can be retired as timing-unknown without racing a
     * renderer submission.  GPU-copy tags travel in their queue entry. */
    struct {
        volatile uint32_t version;
        volatile uint64_t claimedTag __attribute__((aligned(8)));
        LoriePresentTag value;
    } latestPresentTag;

    /* API-33 Choreographer scheduling metadata, published by the X-server
     * callback and sampled by the renderer when it consumes root contents.
     * The seqlock avoids torn 64-bit observations on armeabi-v7a. */
    struct {
        volatile uint32_t version;
        uint32_t reserved;
        uint64_t deadlineUs __attribute__((aligned(8)));
        uint64_t expectedUs;
        uint64_t opportunityUs;
        uint64_t opportunityMsc;
        int64_t vsyncId;
    } latestFrameTimeline;

    /* DEBUG: opt-in SurfaceControl handoff.  Availability and active are
     * renderer-owned observations; request is X-server-owned. */
    volatile uint8_t surfaceControlEnabled;
    volatile uint8_t surfaceControlAvailable;
    volatile uint8_t surfaceControlActive;
    volatile uint8_t surfaceControlEligible;
    LorieSurfaceControlRequest surfaceControlRequest;

    /* ID of root window texture to be drawn. */
    uint64_t rootWindowTextureID;

    /* A signal to renderer to update root window texture content from shared fragment if needed */
    volatile uint8_t drawRequested;

    /* We should avoid triggering renderer if there is no output surface */
    volatile uint8_t surfaceAvailable;

    /*
     * We do not want to block the X server for an extended period; ideally, we would avoid blocking it at all.
     * However, if we don’t block the X server, it will overwrite root window memory fragment, causing tearing or frame distortion.
     * On some devices, there is no way to make EGL/GLES2 render a frame without calling eglSwapBuffers;
     * calls like glFinish, eglWaitGL, and eglWaitClient have no effect.
     * The only way to force EGL to render a frame and flush the command queue is by invoking eglSwapBuffers.
     * But eglSwapBuffers will not return until Android actually displays the frame.
     * Since we want to proceed as quickly as possible, waiting for the frame to be shown is not acceptable.
     *
     * Therefore, we set eglSwapInterval(dpy, 1), so that eglSwapBuffers does not block until the frame is displayed.
     * Even then, we do not want to waste GPU resources rendering more than one full-screen quad per vsync,
     * because that would spend GPU time on a frame that will never be shown.
     * To handle this, we use a waitForNextFrame flag, which we set after a successful render and clear from the AChoreographer’s frame callback.
     */
    volatile uint8_t waitForNextFrame;

    /* DEBUG: Monotonic Choreographer opportunity token.  The legacy Boolean
     * above can lose a newly cleared value when a callback races the tail of
     * the preceding renderer submission.  Matching peers use this token as
     * the predicate and retain the Boolean only for old-peer compatibility. */
    volatile uint32_t frameOpportunitySequence;

    /* Needed to show FPS counter in logcat */
    volatile int renderedFrames;

    /* DEBUG: observation-only renderer timing.  Keep these counters 32-bit so
     * all supported ABIs can update them atomically without imposing another
     * alignment contract on the shared region.  The X server drains them
     * every five seconds while TERMUX_X11_DEBUG is enabled. */
    volatile uint8_t rendererTimingEnabled;
    struct {
        volatile uint32_t swapCount;
        volatile uint32_t swapTotalUs;
        volatile uint32_t swapMaxUs;
        volatile uint32_t swapOverPeriod;
        volatile uint32_t acquireCount;
        volatile uint32_t acquireTotalUs;
        volatile uint32_t acquireMaxUs;
        volatile uint32_t acquireOverPeriod;
        volatile uint32_t opportunityAdvancedDuringDraw;
        /* DEBUG: identify where a published X Present tag stops advancing
         * through the renderer's duplicate-submission guard. */
        volatile uint32_t presentTagReads;
        volatile uint32_t presentTagAdvances;
        volatile uint32_t presentTagEligible;
        volatile uint32_t presentTagSuppressed;
        volatile uint32_t presentTagAttached;
        volatile uint64_t lastPublishedPresentTag;
        volatile uint64_t lastContentPresentTag;
        volatile uint64_t lastSubmittedPresentTag;
    } rendererTiming;

    /* DEBUG: presentation-timestamp collection is explicitly enabled by the
     * X server and only keeps the renderer awake while records are pending. */
    volatile uint8_t presentFeedbackEnabled;
    volatile uint8_t presentFeedbackPending;
    volatile uint32_t presentFeedbackPollSerial;

    struct {
        // We should not allow updating cursor content the same time renderer draws it.
        // locking the mutex protecting the root window can cause waiting for the frame to be drawn which is unacceptable
        pthread_mutex_t lock; // initialized at X server side.
        pid_t lockingPid;
        uint32_t x, y, xhot, yhot, width, height;
        uint32_t bits[512*512]; // 1 megabyte should be enough for any cursor up to 512x512
        // Signals to renderer to update cursor's texture or its coordinates
        volatile uint8_t updated, moved, visible;
    } cursor;
};

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "list.h"

struct SurfaceControlBridge;

struct Renderer {
    static constexpr uint32_t PRESENT_FEEDBACK_QUEUE_CAPACITY = 32;

    struct PendingPresentFeedback {
        bool valid = false;
        uint32_t surfaceGeneration = 0;
        uint64_t rendererSerial = 0;
        uint64_t gpuCopySerial = 0;
        LoriePresentTag presentTag{};
        EGLuint64KHR eglFrameId = 0;
        int64_t submitNs = 0;
    };

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface defaultSfc = EGL_NO_SURFACE, sfc = EGL_NO_SURFACE;
    EGLConfig cfg = nullptr;
    ANativeWindow *defaultWin = nullptr, *win = nullptr;
    struct xorg_list addedBuffers{}, buffers{}, removedBuffers{};
    volatile jint filtering = GL_NEAREST;

    pthread_t thread = 0;
    volatile bool stopping = false;
    volatile bool stateChanged = false, windowChanged = false, viewportChanged = false;
    struct lorie_shared_server_state* pendingState = nullptr;
    ANativeWindow* pendingWin = nullptr;
    volatile int viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0, expectedW = 0, expectedH = 0;
    volatile int hiddenBottom = 0;
    volatile int zoomPercent = 100;
    // Source point a pinch wants kept at a given viewport fraction. Negative sourceX = no pinch.
    volatile float pinchAnchorSourceX = -1.f, pinchAnchorSourceY = -1.f;
    volatile float pinchAnchorFracX = 0.5f, pinchAnchorFracY = 0.5f;
    volatile bool followCursorPan = true;
    float panSourceLeft = 0.f, panSourceTop = 0.f;
    float hiddenPanSourceTop = -1.f; // the vertical pan of the other keyboard state, negative until there was one
    bool bottomWasHidden = false;
    JNIEnv* rendererEnv = nullptr;
    JavaVM* jvm = nullptr; // Stashed by init() so initThread() can be reached via `this` from a plain (non-capturing) pthread_create callback.
    jclass lorieViewClass = nullptr;
    jobject thiz = nullptr; // global ref to the owning LorieView
    jmethodID setRendererViewportMethod = nullptr;
    int reportedViewportX = -1, reportedViewportY = -1, reportedViewportW = -1, reportedViewportH = -1;
    float reportedSourceLeft = -1.f, reportedSourceTop = -1.f, reportedSourceWidth = -1.f, reportedSourceHeight = -1.f;

    pthread_mutex_t stateLock{};
    // Shared with the X server so it can signal us directly.  New peers use
    // stateWakeup->lock plus sequence to close the predicate-check/sleep race;
    // cond remains at offset zero for compatibility with an older server.
    LorieRendererWakeup* stateWakeup = nullptr;
    pthread_cond_t* stateCond = nullptr;
    pthread_cond_t stateChangeFinishCond{};
    pthread_spinlock_t bufferLock{};
    int stateCondFd = -1;
    struct lorie_shared_server_state* state = nullptr;
    struct {
        GLuint id;
        bool cursorChanged;
    } cursor{};

    // FBO used to blit deferred Present "copy" entries (see lorieTryScheduleGpuCopy) into the root texture.
    GLuint gpuCopyFbo = 0;

    GLuint g_texture_program = 0, gv_pos = 0, gv_coords = 0;
    GLuint g_texture_program_bgra = 0, gv_pos_bgra = 0, gv_coords_bgra = 0;

    EGLint configAttribs[13] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE
    };

    // Formerly function-local statics; moved here for the same reason as everything else above -
    // they are per-instance state, not per-process.
    uint64_t dstSizeLogCount = 0, srcSizeLogCount = 0;
    uint64_t lastRequestedBufferId = 0;

    /* DEBUG: EGL_ANDROID_get_frame_timestamps is resolved dynamically so the
     * API-24 minimum remains loadable.  No timestamp is used as a producer or
     * consumer fence. */
    PFNEGLGETNEXTFRAMEIDANDROIDPROC getNextFrameIdANDROID = nullptr;
    PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC getFrameTimestampSupportedANDROID = nullptr;
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC getFrameTimestampsANDROID = nullptr;
    PendingPresentFeedback pendingPresentFeedback[PRESENT_FEEDBACK_QUEUE_CAPACITY]{};
    uint32_t presentFeedbackRead = 0, presentFeedbackWrite = 0;
    uint32_t presentFeedbackSurfaceGeneration = 0;
    uint32_t presentFeedbackPollSerialSeen = 0;
    uint64_t rendererPresentSerial = 0;
    LoriePresentTag latestContentPresentTag{};
    uint64_t lastSubmittedPresentTag = 0;
    uint32_t lastRenderedOpportunity = 0;
    bool presentFeedbackExtensionAvailable = false;
    bool presentFeedbackSurfaceEnabled = false;

    /* DEBUG: API-29+ opt-in presentation transport.  The bridge owns its
     * callback worker and may outlive a retired Android window until all
     * SurfaceFlinger release fences have signalled. */
    SurfaceControlBridge* surfaceControlBridge = nullptr;
    uint32_t lastSurfaceControlRequestVersion = 0;

    volatile int* connFdPtr = nullptr;

    void init(JNIEnv* env, jobject thiz);
    void destroy();
    void* initThread();
    int getWakeupCondFd() const;
    uint32_t rendererWakeSequence() const;
    bool frameOpportunityAvailable() const;
    void signalRenderer();
    void waitForRendererSignal(uint32_t expectedSequence);
    void setFiltering(jint f);
    void testCapabilities(int* legacy_drawing, int* gpu_present_disabled,
                          int* direct_allocation_validated);
    void setSharedState(struct lorie_shared_server_state* newState);
    void addBuffer(LorieBuffer* buf);
    void removeBuffer(uint64_t id);
    void removeAllBuffers();
    void setWindow(JNIEnv* env, jobject jsfc);
    void setViewport(int x, int y, int w, int h, int ew, int eh, int hidden);
    void setZoom(int percent);
    void setZoomAnchor(float sourceX, float sourceY, float fracX, float fracY);
    void clearZoomAnchor();
    void setFollowCursorPan(bool enabled);
    void releaseWinAndSurface(ANativeWindow** anw, EGLSurface* esfc);
    void refreshContext();
    LorieBuffer* findBufferWithRetry(uint64_t id);
    uint64_t applyPendingGpuCopiesLocked();
    void applyPendingGpuCopies();
    void redrawLocked(bool* waitingForBuffers);
    bool shouldWait(bool* waitingForBuffers);
    void threadLoop();
    void bindTexture(GLuint id) const;
    void notifyGpuCopyDone() const;
    void initializePresentFeedbackApi();
    void configurePresentFeedbackSurface();
    void configureSurfaceControl();
    void stopSurfaceControl(uint64_t completionEventId = 0);
    void resetPresentFeedback(bool notifyUnknown);
    void pollPresentFeedback();
    void retirePresentTagUnknown(const LoriePresentTag& presentTag,
                                 uint64_t gpuCopySerial = 0);
    void recordPresentFeedback(uint64_t gpuCopySerial,
                               const LoriePresentTag& presentTag,
                               int64_t submitNs, EGLuint64KHR eglFrameId);
    void notifyPresentFeedback(const PendingPresentFeedback& pending,
                               uint8_t status, int64_t presentNs) const;
    void notifyPresentBackendRelease(const LoriePresentTag& presentTag,
                                     uint8_t mode,
                                     const LorieFrameTimeline* timeline = nullptr) const;
    void reportViewport(int dstX, int dstY, int dstW, int dstH, float left, float top, float width, float height);
    void drawRegion(GLuint id, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t flip);
    void drawCursor(float displayWidth, float displayHeight, float sourceLeft, float sourceTop, float cursorX, float cursorY);
};
#endif

#ifdef __cplusplus
extern "C" {
#endif

static int android_to_linux_keycode[304] = {
        [ 4   /* ANDROID_KEYCODE_BACK */] = KEY_ESC,
        [ 7   /* ANDROID_KEYCODE_0 */] = KEY_0,
        [ 8   /* ANDROID_KEYCODE_1 */] = KEY_1,
        [ 9   /* ANDROID_KEYCODE_2 */] = KEY_2,
        [ 10  /* ANDROID_KEYCODE_3 */] = KEY_3,
        [ 11  /* ANDROID_KEYCODE_4 */] = KEY_4,
        [ 12  /* ANDROID_KEYCODE_5 */] = KEY_5,
        [ 13  /* ANDROID_KEYCODE_6 */] = KEY_6,
        [ 14  /* ANDROID_KEYCODE_7 */] = KEY_7,
        [ 15  /* ANDROID_KEYCODE_8 */] = KEY_8,
        [ 16  /* ANDROID_KEYCODE_9 */] = KEY_9,
        [ 17  /* ANDROID_KEYCODE_STAR */] = KEY_KPASTERISK,
        [ 19  /* ANDROID_KEYCODE_DPAD_UP */] = KEY_UP,
        [ 20  /* ANDROID_KEYCODE_DPAD_DOWN */] = KEY_DOWN,
        [ 21  /* ANDROID_KEYCODE_DPAD_LEFT */] = KEY_LEFT,
        [ 22  /* ANDROID_KEYCODE_DPAD_RIGHT */] = KEY_RIGHT,
        [ 23  /* ANDROID_KEYCODE_DPAD_CENTER */] = KEY_ENTER,
        [ 24  /* ANDROID_KEYCODE_VOLUME_UP */] = KEY_VOLUMEUP, // XF86XK_AudioRaiseVolume
        [ 25  /* ANDROID_KEYCODE_VOLUME_DOWN */] = KEY_VOLUMEDOWN, // XF86XK_AudioLowerVolume
        [ 26  /* ANDROID_KEYCODE_POWER */] = KEY_POWER,
        [ 27  /* ANDROID_KEYCODE_CAMERA */] = KEY_CAMERA,
        [ 28  /* ANDROID_KEYCODE_CLEAR */] = KEY_CLEAR,
        [ 29  /* ANDROID_KEYCODE_A */] = KEY_A,
        [ 30  /* ANDROID_KEYCODE_B */] = KEY_B,
        [ 31  /* ANDROID_KEYCODE_C */] = KEY_C,
        [ 32  /* ANDROID_KEYCODE_D */] = KEY_D,
        [ 33  /* ANDROID_KEYCODE_E */] = KEY_E,
        [ 34  /* ANDROID_KEYCODE_F */] = KEY_F,
        [ 35  /* ANDROID_KEYCODE_G */] = KEY_G,
        [ 36  /* ANDROID_KEYCODE_H */] = KEY_H,
        [ 37  /* ANDROID_KEYCODE_I */] = KEY_I,
        [ 38  /* ANDROID_KEYCODE_J */] = KEY_J,
        [ 39  /* ANDROID_KEYCODE_K */] = KEY_K,
        [ 40  /* ANDROID_KEYCODE_L */] = KEY_L,
        [ 41  /* ANDROID_KEYCODE_M */] = KEY_M,
        [ 42  /* ANDROID_KEYCODE_N */] = KEY_N,
        [ 43  /* ANDROID_KEYCODE_O */] = KEY_O,
        [ 44  /* ANDROID_KEYCODE_P */] = KEY_P,
        [ 45  /* ANDROID_KEYCODE_Q */] = KEY_Q,
        [ 46  /* ANDROID_KEYCODE_R */] = KEY_R,
        [ 47  /* ANDROID_KEYCODE_S */] = KEY_S,
        [ 48  /* ANDROID_KEYCODE_T */] = KEY_T,
        [ 49  /* ANDROID_KEYCODE_U */] = KEY_U,
        [ 50  /* ANDROID_KEYCODE_V */] = KEY_V,
        [ 51  /* ANDROID_KEYCODE_W */] = KEY_W,
        [ 52  /* ANDROID_KEYCODE_X */] = KEY_X,
        [ 53  /* ANDROID_KEYCODE_Y */] = KEY_Y,
        [ 54  /* ANDROID_KEYCODE_Z */] = KEY_Z,
        [ 55  /* ANDROID_KEYCODE_COMMA */] = KEY_COMMA,
        [ 56  /* ANDROID_KEYCODE_PERIOD */] = KEY_DOT,
        [ 57  /* ANDROID_KEYCODE_ALT_LEFT */] = KEY_LEFTALT,
        [ 58  /* ANDROID_KEYCODE_ALT_RIGHT */] = KEY_RIGHTALT,
        [ 59  /* ANDROID_KEYCODE_SHIFT_LEFT */] = KEY_LEFTSHIFT,
        [ 60  /* ANDROID_KEYCODE_SHIFT_RIGHT */] = KEY_RIGHTSHIFT,
        [ 61  /* ANDROID_KEYCODE_TAB */] = KEY_TAB,
        [ 62  /* ANDROID_KEYCODE_SPACE */] = KEY_SPACE,
        [ 64  /* ANDROID_KEYCODE_EXPLORER */] = KEY_WWW,
        [ 65  /* ANDROID_KEYCODE_ENVELOPE */] = KEY_MAIL,
        [ 66  /* ANDROID_KEYCODE_ENTER */] = KEY_ENTER,
        [ 67  /* ANDROID_KEYCODE_DEL */] = KEY_BACKSPACE,
        [ 68  /* ANDROID_KEYCODE_GRAVE */] = KEY_GRAVE,
        [ 69  /* ANDROID_KEYCODE_MINUS */] = KEY_MINUS,
        [ 70  /* ANDROID_KEYCODE_EQUALS */] = KEY_EQUAL,
        [ 71  /* ANDROID_KEYCODE_LEFT_BRACKET */] = KEY_LEFTBRACE,
        [ 72  /* ANDROID_KEYCODE_RIGHT_BRACKET */] = KEY_RIGHTBRACE,
        [ 73  /* ANDROID_KEYCODE_BACKSLASH */] = KEY_BACKSLASH,
        [ 74  /* ANDROID_KEYCODE_SEMICOLON */] = KEY_SEMICOLON,
        [ 75  /* ANDROID_KEYCODE_APOSTROPHE */] = KEY_APOSTROPHE,
        [ 76  /* ANDROID_KEYCODE_SLASH */] = KEY_SLASH,
        [ 81  /* ANDROID_KEYCODE_PLUS */] = KEY_KPPLUS,
        [ 82  /* ANDROID_KEYCODE_MENU */] = KEY_CONTEXT_MENU,
        [ 84  /* ANDROID_KEYCODE_SEARCH */] = KEY_SEARCH,
        [ 85  /* ANDROID_KEYCODE_MEDIA_PLAY_PAUSE */] = KEY_PLAYPAUSE,
        [ 86  /* ANDROID_KEYCODE_MEDIA_STOP */] = KEY_STOP_RECORD,
        [ 87  /* ANDROID_KEYCODE_MEDIA_NEXT */] = KEY_NEXTSONG,
        [ 88  /* ANDROID_KEYCODE_MEDIA_PREVIOUS */] = KEY_PREVIOUSSONG,
        [ 89  /* ANDROID_KEYCODE_MEDIA_REWIND */] = KEY_REWIND,
        [ 90  /* ANDROID_KEYCODE_MEDIA_FAST_FORWARD */] = KEY_FASTFORWARD,
        [ 91  /* ANDROID_KEYCODE_MUTE */] = KEY_MUTE,
        [ 92  /* ANDROID_KEYCODE_PAGE_UP */] = KEY_PAGEUP,
        [ 93  /* ANDROID_KEYCODE_PAGE_DOWN */] = KEY_PAGEDOWN,
        [ 111  /* ANDROID_KEYCODE_ESCAPE */] = KEY_ESC,
        [ 112  /* ANDROID_KEYCODE_FORWARD_DEL */] = KEY_DELETE,
        [ 113  /* ANDROID_KEYCODE_CTRL_LEFT */] = KEY_LEFTCTRL,
        [ 114  /* ANDROID_KEYCODE_CTRL_RIGHT */] = KEY_RIGHTCTRL,
        [ 115  /* ANDROID_KEYCODE_CAPS_LOCK */] = KEY_CAPSLOCK,
        [ 116  /* ANDROID_KEYCODE_SCROLL_LOCK */] = KEY_SCROLLLOCK,
        [ 117  /* ANDROID_KEYCODE_META_LEFT */] = KEY_LEFTMETA,
        [ 118  /* ANDROID_KEYCODE_META_RIGHT */] = KEY_RIGHTMETA,
        [ 120  /* ANDROID_KEYCODE_SYSRQ */] = KEY_PRINT,
        [ 121  /* ANDROID_KEYCODE_BREAK */] = KEY_BREAK,
        [ 122  /* ANDROID_KEYCODE_MOVE_HOME */] = KEY_HOME,
        [ 123  /* ANDROID_KEYCODE_MOVE_END */] = KEY_END,
        [ 124  /* ANDROID_KEYCODE_INSERT */] = KEY_INSERT,
        [ 125  /* ANDROID_KEYCODE_FORWARD */] = KEY_FORWARD,
        [ 126  /* ANDROID_KEYCODE_MEDIA_PLAY */] = KEY_PLAYCD,
        [ 127  /* ANDROID_KEYCODE_MEDIA_PAUSE */] = KEY_PAUSECD,
        [ 128  /* ANDROID_KEYCODE_MEDIA_CLOSE */] = KEY_CLOSECD,
        [ 129  /* ANDROID_KEYCODE_MEDIA_EJECT */] = KEY_EJECTCD,
        [ 130  /* ANDROID_KEYCODE_MEDIA_RECORD */] = KEY_RECORD,
        [ 131  /* ANDROID_KEYCODE_F1 */] = KEY_F1,
        [ 132  /* ANDROID_KEYCODE_F2 */] = KEY_F2,
        [ 133  /* ANDROID_KEYCODE_F3 */] = KEY_F3,
        [ 134  /* ANDROID_KEYCODE_F4 */] = KEY_F4,
        [ 135  /* ANDROID_KEYCODE_F5 */] = KEY_F5,
        [ 136  /* ANDROID_KEYCODE_F6 */] = KEY_F6,
        [ 137  /* ANDROID_KEYCODE_F7 */] = KEY_F7,
        [ 138  /* ANDROID_KEYCODE_F8 */] = KEY_F8,
        [ 139  /* ANDROID_KEYCODE_F9 */] = KEY_F9,
        [ 140  /* ANDROID_KEYCODE_F10 */] = KEY_F10,
        [ 141  /* ANDROID_KEYCODE_F11 */] = KEY_F11,
        [ 142  /* ANDROID_KEYCODE_F12 */] = KEY_F12,
        [ 143  /* ANDROID_KEYCODE_NUM_LOCK */] = KEY_NUMLOCK,
        [ 144  /* ANDROID_KEYCODE_NUMPAD_0 */] = KEY_KP0,
        [ 145  /* ANDROID_KEYCODE_NUMPAD_1 */] = KEY_KP1,
        [ 146  /* ANDROID_KEYCODE_NUMPAD_2 */] = KEY_KP2,
        [ 147  /* ANDROID_KEYCODE_NUMPAD_3 */] = KEY_KP3,
        [ 148  /* ANDROID_KEYCODE_NUMPAD_4 */] = KEY_KP4,
        [ 149  /* ANDROID_KEYCODE_NUMPAD_5 */] = KEY_KP5,
        [ 150  /* ANDROID_KEYCODE_NUMPAD_6 */] = KEY_KP6,
        [ 151  /* ANDROID_KEYCODE_NUMPAD_7 */] = KEY_KP7,
        [ 152  /* ANDROID_KEYCODE_NUMPAD_8 */] = KEY_KP8,
        [ 153  /* ANDROID_KEYCODE_NUMPAD_9 */] = KEY_KP9,
        [ 154  /* ANDROID_KEYCODE_NUMPAD_DIVIDE */] = KEY_KPSLASH,
        [ 155  /* ANDROID_KEYCODE_NUMPAD_MULTIPLY */] = KEY_KPASTERISK,
        [ 156  /* ANDROID_KEYCODE_NUMPAD_SUBTRACT */] = KEY_KPMINUS,
        [ 157  /* ANDROID_KEYCODE_NUMPAD_ADD */] = KEY_KPPLUS,
        [ 158  /* ANDROID_KEYCODE_NUMPAD_DOT */] = KEY_KPDOT,
        [ 159  /* ANDROID_KEYCODE_NUMPAD_COMMA */] = KEY_KPCOMMA,
        [ 160  /* ANDROID_KEYCODE_NUMPAD_ENTER */] = KEY_KPENTER,
        [ 161  /* ANDROID_KEYCODE_NUMPAD_EQUALS */] = KEY_KPEQUAL,
        [ 162  /* ANDROID_KEYCODE_NUMPAD_LEFT_PAREN */] = KEY_KPLEFTPAREN,
        [ 163  /* ANDROID_KEYCODE_NUMPAD_RIGHT_PAREN */] = KEY_KPRIGHTPAREN,
        [ 164  /* ANDROID_KEYCODE_VOLUME_MUTE */] = KEY_MUTE,
        [ 165  /* ANDROID_KEYCODE_INFO */] = KEY_INFO,
        [ 166  /* ANDROID_KEYCODE_CHANNEL_UP */] = KEY_CHANNELUP,
        [ 167  /* ANDROID_KEYCODE_CHANNEL_DOWN */] = KEY_CHANNELDOWN,
        [ 168  /* ANDROID_KEYCODE_ZOOM_IN */] = KEY_ZOOMIN,
        [ 169  /* ANDROID_KEYCODE_ZOOM_OUT */] = KEY_ZOOMOUT,
        [ 170  /* ANDROID_KEYCODE_TV */] = KEY_TV,
        [ 208  /* ANDROID_KEYCODE_CALENDAR */] = KEY_CALENDAR,
        [ 210  /* ANDROID_KEYCODE_CALCULATOR */] = KEY_CALC,
};

#ifdef __cplusplus
}
#endif
