#include <sys/iosupport.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>

#include <3ds.h>

#include "../core/clipboard.h"
#include "../core/error.h"
#include "../core/fs.h"
#include <jansson.h>
#include "../core/http.h"
#include "../core/screen.h"
#include "../core/sound.h"
#include "../core/bgmusic.h"
#include "../core/speedlog.h"
#include "../core/xferturn.h"
#include "launch3dsx.h"
#include "thumbs.h"
#include "../core/task/task.h"
#include "../core/ui/ui.h"
#include "section.h"
#include "ftpserver.h"
#include "webserver.h"
#include "udslink.h"
#include "selftest.h"
#include "relaunch.h"
#include "settings.h"
#include "update.h"
#include "task/uitask.h"

#define CURRENT_KPROCESS (*(void**) 0xFFFF9004)

#define KPROCESS_PID_OFFSET_OLD (0xB4)
#define KPROCESS_PID_OFFSET_NEW (0xBC)

// The share of the system core (core 1) the app keeps for its threads there; see XFERTURN_CPU_LIMIT for transfers.
#define MAIN_CPU_LIMIT 30

static bool backdoor_ran = false;
static bool n3ds = false;
static u32 old_pid = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
static __attribute__((naked)) Result svcGlobalBackdoor(s32 (*callback)()) {
    asm volatile(
            "svc 0x30\n"
            "bx lr"
    );
}
#pragma GCC diagnostic pop

static s32 patch_pid_kernel() {
    u32 *pidPtr = (u32*) (CURRENT_KPROCESS + (n3ds ? KPROCESS_PID_OFFSET_NEW : KPROCESS_PID_OFFSET_OLD));

    old_pid = *pidPtr;
    *pidPtr = 0;

    backdoor_ran = true;
    return 0;
}

static s32 restore_pid_kernel() {
    u32 *pidPtr = (u32*) (CURRENT_KPROCESS + (n3ds ? KPROCESS_PID_OFFSET_NEW : KPROCESS_PID_OFFSET_OLD));

    *pidPtr = old_pid;

    backdoor_ran = true;
    return 0;
}

static bool attempt_patch_pid() {
    backdoor_ran = false;
    APT_CheckNew3DS(&n3ds);

    svcGlobalBackdoor(patch_pid_kernel);
    srvExit();
    srvInit();
    svcGlobalBackdoor(restore_pid_kernel);

    return backdoor_ran;
}

static void (*exit_funcs[16])()= {NULL};
static u32 exit_func_count = 0;

static void* soc_buffer = NULL;

void cleanup_services() {
    for(u32 i = 0; i < exit_func_count; i++) {
        if(exit_funcs[i] != NULL) {
            exit_funcs[i]();
            exit_funcs[i] = NULL;
        }
    }

    exit_func_count = 0;

    if(soc_buffer != NULL) {
        free(soc_buffer);
        soc_buffer = NULL;
    }
}

#define INIT_SERVICE(initStatement, exitFunc) (R_SUCCEEDED(res = (initStatement)) && (exit_funcs[exit_func_count++] = (exitFunc)))

Result init_services() {
    Result res = 0;

    soc_buffer = memalign(0x1000, 0x100000);
    if(soc_buffer != NULL) {
        Handle tempAM = 0;
        if(R_SUCCEEDED(res = srvGetServiceHandle(&tempAM, "am:net"))) {
            svcCloseHandle(tempAM);

            if(INIT_SERVICE(amInit(), amExit)
               && INIT_SERVICE(cfguInit(), cfguExit)
               && INIT_SERVICE(acInit(), acExit)
               && INIT_SERVICE(ptmuInit(), ptmuExit)
               && INIT_SERVICE(pxiDevInit(), pxiDevExit)
               && INIT_SERVICE(httpcInit(0), httpcExit)
               && INIT_SERVICE(socInit(soc_buffer, 0x100000), (void (*)()) socExit));

            // devkitPro's mbedTLS takes its randomness from sslcGenerateRandomData without checking
            // the result: without an SSLC session curl's HTTPS keys would come from stack garbage.
            if(R_SUCCEEDED(res) && R_SUCCEEDED(sslcInit(0))) {
                exit_funcs[exit_func_count++] = sslcExit;
            }
        }
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    if(R_FAILED(res)) {
        cleanup_services();
    }

    return res;
}

static u32 old_time_limit = UINT32_MAX;

// The end of each line of a speed trace: whether the music plays, and how busy the cores are while
// SITE CPU's counting threads run.
static void main_speedlog_context(char* out, size_t size) {
    bgmusic_status music;
    bgmusic_get_status(&music);

    char cpu[48];
    selftest_cpu_sample(cpu, sizeof(cpu));
    snprintf(out, size, "music %d%s%s", music.playing || music.generating ? 1 : 0, cpu[0] != '\0' ? " " : "", cpu);
}

// The system turns the Wi-Fi off when the console sleeps or HOME is pressed. A server still holding its sockets keeps
// the network from coming back (no Wi-Fi after waking) and can stall HOME or the exit, so both servers are closed
// before the app sleeps or is suspended and brought back (as after the local wireless) once it is awake again.
// The hook runs on the main thread, inside aptMainLoop.
static aptHookCookie net_sleep_cookie;
static bool net_sleep_paused = false;

// DEBUG builds: sdmc:/nsa/sleep-log.txt, one flushed line per event, to see where sleep, HOME or the exit stop.
static void sleeplog(const char* format, ...) {
    if(!NSA_DEBUG) {
        return;
    }

    char text[200];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    u32 wifi = 99;
    Result wifiRes = ACU_GetWifiStatus(&wifi);
    if(R_FAILED(wifiRes)) {
        wifi = 98;
    }

    FILE* f = fopen("sdmc:/nsa/sleep-log.txt", "a");
    if(f != NULL && ftell(f) > 64 * 1024) {
        fclose(f);
        f = fopen("sdmc:/nsa/sleep-log.txt", "w");
    }

    if(f != NULL) {
        fprintf(f, "%llu wifi=%lu ftp=%d web=%d %s\n", (unsigned long long) osGetTime(), (unsigned long) wifi,
                ftpserver_running() ? 1 : 0, webserver_running() ? 1 : 0, text);
        fflush(f);
        fclose(f);
    }
}

static void net_sleep_hook(APT_HookType hook, void* param) {
    (void) param;

    static const char* const names[] = {"ONSUSPEND", "ONRESTORE", "ONSLEEP", "ONWAKEUP", "ONEXIT"};
    sleeplog("hook %s", (u32) hook < 5 ? names[hook] : "?");

    switch(hook) {
        case APTHOOK_ONSLEEP:
        case APTHOOK_ONSUSPEND:
            if(!net_sleep_paused && !udslink_active()) {
                sleeplog("pausing servers");
                ftpserver_pause();
                webserver_pause();
                net_sleep_paused = true;
                sleeplog("servers paused");
            }
            break;
        case APTHOOK_ONWAKEUP:
        case APTHOOK_ONRESTORE:
            // The app's session with the network manager does not survive the sleep (ACU_GetWifiStatus fails
            // from then on): open it again, or the app sees no Wi-Fi and the servers never come back.
            acExit();
            sleeplog("acInit -> %08lX", (unsigned long) acInit());
            if(net_sleep_paused) {
                net_sleep_paused = false;
                if(!udslink_active()) {
                    ftpserver_resume_later();
                    webserver_resume_later();
                    sleeplog("servers will resume");
                }
            }
            break;
        default:
            break;
    }

    sleeplog("hook done");
}

void init() {
    gfxInitDefault();

    Result romfsRes = romfsInit();
    if(R_FAILED(romfsRes)) {
        error_panic("Failed to mount RomFS: %08lX", romfsRes);
        return;
    }

    if(R_FAILED(init_services())) {
        if(!attempt_patch_pid()) {
            error_panic("Kernel backdoor not installed.\nPlease run a kernel exploit and try again.");
            return;
        }

        Result initRes = init_services();
        if(R_FAILED(initRes)) {
            error_panic("Failed to initialize services: %08lX", initRes);
            return;
        }
    }

    osSetSpeedupEnable(true);

    APT_GetAppCpuTimeLimit(&old_time_limit);
    Result cpuRes = APT_SetAppCpuTimeLimit(MAIN_CPU_LIMIT);
    if(R_FAILED(cpuRes)) {
        error_panic("Failed to set syscore CPU time limit: %08lX", cpuRes);
        return;
    }

    AM_InitializeExternalTitleDatabase(false);

    screen_init();
    ui_init();
    selftest_init();
    http_set_log(udslink_log);
    speedlog_init();
    xferturn_init();
    bool new3ds = false;
    APT_CheckNew3DS(&new3ds);
    if(!new3ds) {
        xferturn_cpu_limits(MAIN_CPU_LIMIT);
    }
    speedlog_set_context(main_speedlog_context);
    task_init();
    aptHook(&net_sleep_cookie, net_sleep_hook, NULL);
}

void cleanup() {
    sleeplog("cleanup: start");
    aptUnhook(&net_sleep_cookie);
    clipboard_clear();

    task_exit();
    sleeplog("cleanup: task_exit done");
    http_shutdown();
    sleeplog("cleanup: http_shutdown done");
    thumbs_exit();
    sleeplog("cleanup: thumbs_exit done");
    bgmusic_exit();
    sleeplog("cleanup: bgmusic_exit done");
    sound_exit();
    sleeplog("cleanup: sound_exit done");
    ui_exit();
    sleeplog("cleanup: ui_exit done");
    screen_exit();
    sleeplog("cleanup: screen_exit done");

    if(old_time_limit != UINT32_MAX) {
        APT_SetAppCpuTimeLimit(old_time_limit);
    }

    osSetSpeedupEnable(false);

    cleanup_services();
    sleeplog("cleanup: services done");

    romfsExit();

    gfxExit();
    sleeplog("cleanup: done");
}

// The bottom screen's toast: a restart countdown wins over an FTP transfer.
static bool main_notice(char* out, size_t size, float* progress) {
    // A transfer over FTP or the web page (with its speed) before the FTP server's own, which then only says it installs.
    return relaunch_notice(out, size, progress) || xferturn_notice(out, size, progress) || ftpserver_notice(out, size, progress);
}

// The top bar's icons: the FTP server moving data, the web server running.
static u32 main_activity() {
    return (ftpserver_transferring() ? UI_STATUS_FTP_BUSY : 0) | (webserver_running() ? UI_STATUS_WEB : 0);
}

int main(int argc, const char* argv[]) {
    if(argc > 0 && envIsHomebrew()) {
        fs_set_3dsx_path(argv[0]);
    }

    init();

    settings_load();
    settings_apply();
    ui_set_status(ftpserver_corner, main_notice, main_activity);

    mainmenu_open();
    startup_run();

    sleeplog("main loop start");
    u64 beat = osGetTime();
    bool aptAlive = true;
    bool uiAlive = true;
    while((aptAlive = aptMainLoop()) && (uiAlive = ui_update())) {
        // Each beat opens a file on the SD card and asks the network manager: seconds of a frozen screen at times, so rarely.
        if(osGetTime() - beat >= 60000) {
            beat = osGetTime();
            sleeplog("beat");
        }

        update_poll();
        ftpserver_poll_main();
        webserver_poll_main();
        udslink_poll_main();
        bgmusic_hold(settings_get()->ftpPausesMusic && ftpserver_holding_music());

        // A new NSA is in place (updater or SITE INSTALL): exit so libctru relaunches it.
        if(relaunch_poll()) {
            break;
        }

        // A 3DSX was asked for: exit so libctru starts the loader's title.
        if(launch3dsx_poll()) {
            break;
        }
    }

    sleeplog("main loop left: aptMainLoop=%d ui_update=%d", aptAlive ? 1 : 0, uiAlive ? 1 : 0);

    // The Store's threads read its data in a loop: stopped before anything they use goes away.
    store_exit();
    sleeplog("store_exit done");

    ftpserver_stop();
    sleeplog("ftpserver_stop done");
    webserver_stop();
    sleeplog("webserver_stop done");

    cleanup();

    return 0;
}