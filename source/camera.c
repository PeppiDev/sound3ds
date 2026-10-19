#include "camera.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_BYTES (CAM_W * CAM_H * 2)

static Thread s_thread;
static Handle s_stop;
static bool s_running = false;
static bool s_lock_ready = false;
static LightLock s_lock;
static LightEvent s_event;
static u16* s_frame;            // último cuadro completo
static volatile u32 s_seq;
static volatile u64 s_last_ms;

// Pide el próximo cuadro; 0 si la cámara no lo aceptó
static Handle arm_receive(u16* buf, u32 unit) {
    Handle h = 0;
    if (R_FAILED(CAMU_SetReceiving(&h, buf, PORT_CAM1, FRAME_BYTES, (s16)unit))) return 0;
    return h;
}

static void cam_thread(void* arg) {
    (void)arg;
    u16* rx = (u16*)malloc(FRAME_BYTES);
    if (!rx) return;
    if (R_FAILED(camInit())) {
        free(rx);
        return;
    }
    u32 unit = 0;
    Handle buf_err = 0;
    CAMU_SetSize(SELECT_OUT1, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
    CAMU_SetNoiseFilter(SELECT_OUT1, true);
    CAMU_SetAutoExposure(SELECT_OUT1, true);
    CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
    CAMU_Activate(SELECT_OUT1);
    CAMU_GetBufferErrorInterruptEvent(&buf_err, PORT_CAM1);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetMaxBytes(&unit, CAM_W, CAM_H);
    CAMU_SetTransferBytes(PORT_CAM1, unit, CAM_W, CAM_H);
    CAMU_ClearBuffer(PORT_CAM1);
    Handle recv = arm_receive(rx, unit);
    CAMU_StartCapture(PORT_CAM1);

    while (true) {
        Handle ev[3] = { s_stop, buf_err, recv };
        s32 idx = -1;
        if (R_FAILED(svcWaitSynchronizationN(&idx, ev, recv ? 3 : 2, false, 500 * 1000 * 1000LL))) {
            svcSleepThread(10 * 1000 * 1000LL);
            idx = -1;
        }
        if (idx == 0) break;
        if (idx == 2) {
            // Cuadro completo: publicarlo y pedir el siguiente de inmediato
            svcCloseHandle(recv);
            recv = 0;
            LightLock_Lock(&s_lock);
            memcpy(s_frame, rx, FRAME_BYTES);
            LightLock_Unlock(&s_lock);
            s_seq++;
            s_last_ms = osGetTime();
            LightEvent_Signal(&s_event);
            recv = arm_receive(rx, unit);
        } else if (idx == 1 || !recv) {
            // Búfer lleno (la cámara se detuvo) o sin recepción pedida: relanzar la captura
            if (recv) svcCloseHandle(recv);
            CAMU_ClearBuffer(PORT_CAM1);
            recv = arm_receive(rx, unit);
            CAMU_StartCapture(PORT_CAM1);
        }
    }

    CAMU_StopCapture(PORT_CAM1);
    bool busy = false;
    for (int i = 0; i < 50 && R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy; i++)
        svcSleepThread(2 * 1000 * 1000LL);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    if (recv) svcCloseHandle(recv);
    if (buf_err) svcCloseHandle(buf_err);
    camExit();
    free(rx);
}

bool camera_start(void) {
    if (s_running) return true;
    if (!s_lock_ready) {
        LightLock_Init(&s_lock);
        LightEvent_Init(&s_event, RESET_ONESHOT);
        s_lock_ready = true;
    }
    if (!s_frame) {
        s_frame = (u16*)calloc(1, FRAME_BYTES);
        if (!s_frame) return false;
    }
    if (R_FAILED(svcCreateEvent(&s_stop, RESET_ONESHOT))) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_last_ms = osGetTime();
    // Más prioridad que el hilo principal: casi siempre está esperando y debe
    // responder al instante cuando llega un cuadro
    s_thread = threadCreate(cam_thread, NULL, 0x4000, prio - 2, -2, false);
    if (!s_thread) {
        svcCloseHandle(s_stop);
        return false;
    }
    s_running = true;
    return true;
}

void camera_stop(void) {
    if (!s_running) return;
    svcSignalEvent(s_stop);
    threadJoin(s_thread, U64_MAX);
    threadFree(s_thread);
    svcCloseHandle(s_stop);
    s_running = false;
}

// Libera el cuadro guardado (con la cámara detenida)
void camera_free(void) {
    if (s_running) return;
    free(s_frame);
    s_frame = NULL;
}

bool camera_running(void) { return s_running; }
u32 camera_frame_seq(void) { return s_seq; }
u64 camera_ms_since_frame(void) { return osGetTime() - s_last_ms; }

const u16* camera_lock_frame(void) {
    LightLock_Lock(&s_lock);
    return s_frame;
}

void camera_unlock_frame(void) { LightLock_Unlock(&s_lock); }

LightEvent* camera_frame_event(void) { return &s_event; }
