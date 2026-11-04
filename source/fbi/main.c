#include <sys/iosupport.h>
#include <malloc.h>

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
}

void cleanup() {
    clipboard_clear();

    task_exit();
    http_shutdown();
    thumbs_exit();
    bgmusic_exit();
    sound_exit();
    ui_exit();
    screen_exit();

    if(old_time_limit != UINT32_MAX) {
        APT_SetAppCpuTimeLimit(old_time_limit);
    }

    osSetSpeedupEnable(false);

    cleanup_services();

    romfsExit();

    gfxExit();
}

// The bottom screen's toast: a restart countdown wins over an FTP transfer.
static bool main_notice(char* out, size_t size, float* progress) {
    return relaunch_notice(out, size, progress) || ftpserver_notice(out, size, progress);
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

    while(aptMainLoop() && ui_update()) {
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

    ftpserver_stop();
    webserver_stop();

    cleanup();

    return 0;
}