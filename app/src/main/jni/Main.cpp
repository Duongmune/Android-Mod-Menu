#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <string>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"
#include "dobby.h"

// ================================================================
//  CRASH HANDLER - SAFE MODE
// ================================================================
sigjmp_buf crash_env;
bool isCrashHandled = false;
bool isHookFailed = false;

void CrashHandler(int sig, siginfo_t *info, void *context) {
    isHookFailed = true;
    if (!isCrashHandled) {
        isCrashHandled = true;
        siglongjmp(crash_env, 1);
    } else {
        pthread_exit(0);
    }
}

void InitCrashHandler() {
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sa.sa_sigaction = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

// ================================================================
//  SAFE MEMORY READ/WRITE
// ================================================================
template<typename T>
T SafeRead(uintptr_t addr) {
    if (!addr) return T{};
    return *(T*)addr;
}

template<typename T>
void SafeWrite(uintptr_t addr, T value) {
    if (!addr) return;
    *(T*)addr = value;
}

// ================================================================
//  GAME OBJECT WRAPPERS - THROWIO (NEW STRUCTURE)
// ================================================================
class AppStoreData {
public:
    void* instance;
    AppStoreData(void* obj) : instance(obj) {}

    long GetSoftCurrency() { return SafeRead<long>((uintptr_t)instance + 0x18); }
    void SetSoftCurrency(long v) { SafeWrite<long>((uintptr_t)instance + 0x18, v); }
    long GetHardCurrency() { return SafeRead<long>((uintptr_t)instance + 0x20); }
    void SetHardCurrency(long v) { SafeWrite<long>((uintptr_t)instance + 0x20, v); }
};

class CharacterStats {
public:
    void* instance;
    CharacterStats(void* obj) : instance(obj) {}

    float GetHealth() { return SafeRead<float>((uintptr_t)instance + 0x30); }
    void SetHealth(float v) { SafeWrite<float>((uintptr_t)instance + 0x30, v); }
    float GetMoveSpeed() { return SafeRead<float>((uintptr_t)instance + 0x34); }
    void SetMoveSpeed(float v) { SafeWrite<float>((uintptr_t)instance + 0x34, v); }
    bool GetUndead() { return SafeRead<bool>((uintptr_t)instance + 0x40); }
    void SetUndead(bool v) { SafeWrite<bool>((uintptr_t)instance + 0x40, v); }
};

class BotController {
public:
    void* instance;
    BotController(void* obj) : instance(obj) {}

    void SetFreeze(bool v) { SafeWrite<bool>((uintptr_t)instance + 0x28, v); }
};

// ================================================================
//  GLOBAL STATE - THROWIO
// ================================================================
void* g_AppStoreDataInstance   = nullptr;
void* g_CharacterStatsInstance = nullptr;
void* g_BotControllerInstance  = nullptr;

// Toggles
bool bInfiniteSoftMoney  = false;
bool bInfiniteHardCurrency = false;
bool bGodMode            = false;
bool bSpeedHack          = false;
bool bFreezeBots         = false;
bool bFreeOpenChest      = false;
bool bNoAds              = true;

// Values
float fMoveSpeedMultiplier = 1.5f;

// ================================================================
//  HOOKS - THROWIO UPDATED OFFSETS & MODULES
// ================================================================

// --- AppStoreData::Update() / ProcessPurchase ---
typedef void(*AppStoreData_UpdateFn)(void* self);
AppStoreData_UpdateFn old_AppStoreData_Update = nullptr;

void hk_AppStoreData_Update(void* self) {
    if (!self) { old_AppStoreData_Update(self); return; }
    g_AppStoreDataInstance = self;

    if (bInfiniteSoftMoney || bInfiniteHardCurrency) {
        AppStoreData store(self);
        if (bInfiniteSoftMoney) store.SetSoftCurrency(0x7FFFFFFF);
        if (bInfiniteHardCurrency) store.SetHardCurrency(0x7FFFFFFF);
    }

    old_AppStoreData_Update(self);
}

// --- CharacterStats::Update() - GodMode & Speed ---
typedef void(*CharacterStats_UpdateFn)(void* self, float dt);
CharacterStats_UpdateFn old_CharacterStats_Update = nullptr;

void hk_CharacterStats_Update(void* self, float dt) {
    if (!self || isHookFailed) {
        if (old_CharacterStats_Update) old_CharacterStats_Update(self, dt);
        return;
    }

    g_CharacterStatsInstance = self;
    CharacterStats stats(self);

    if (bGodMode) {
        stats.SetUndead(true);
    }
    if (bSpeedHack) {
        stats.SetMoveSpeed(fMoveSpeedMultiplier);
    }

    if (old_CharacterStats_Update) old_CharacterStats_Update(self, dt);
}

// --- BotController::Update() - Control Enemy/Bot AI ---
typedef void(*BotController_UpdateFn)(void* self);
BotController_UpdateFn old_BotController_Update = nullptr;

void hk_BotController_Update(void* self) {
    if (!self) { old_BotController_Update(self); return; }
    g_BotControllerInstance = self;

    if (bFreezeBots) {
        BotController bot(self);
        bot.SetFreeze(true);
    }

    old_BotController_Update(self);
}

// --- ApplovinManager::ShowRewarded() - Bypass/Auto Reward Ads ---
typedef bool(*ApplovinManager_ShowRewardedFn)(void* self);
ApplovinManager_ShowRewardedFn old_ApplovinManager_ShowRewarded = nullptr;

bool hk_ApplovinManager_ShowRewarded(void* self) {
    LOGI("ThrowIO: Quảng cáo đã bị chặn/Tự động nhận thưởng thành công!");
    return true; // Luôn trả về thành công như đã xem quảng cáo
}

// ================================================================
//  MEMORY WATCHDOG
// ================================================================
void* WatchdogThread(void*) {
    int errorCount = 0;
    LOGI("ThrowIO: Watchdog started");

    while (true) {
        usleep(2000000); // 2 seconds

        if (isHookFailed || errorCount > 10) break;

        try {
            if (g_AppStoreDataInstance && (bInfiniteSoftMoney || bInfiniteHardCurrency)) {
                AppStoreData store(g_AppStoreDataInstance);
                if (bInfiniteSoftMoney) store.SetSoftCurrency(0x7FFFFFFF);
                if (bInfiniteHardCurrency) store.SetHardCurrency(0x7FFFFFFF);
            }

            if (g_CharacterStatsInstance && (bGodMode || bSpeedHack)) {
                CharacterStats stats(g_CharacterStatsInstance);
                if (bGodMode) stats.SetUndead(true);
                if (bSpeedHack) stats.SetMoveSpeed(fMoveSpeedMultiplier);
            }

            errorCount = 0;
        } catch (...) {
            errorCount++;
            LOGI("ThrowIO: Watchdog error #%d", errorCount);
        }
    }

    LOGI("ThrowIO: Watchdog stopped");
    return nullptr;
}

// ================================================================
//  LGL - 1. FEATURE LIST (GIAO DIỆN MENU MỚI)
// ================================================================
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;
    const char *features[] = {
        // Header
        OBFUSCATE("RichTextView_"
            "<div style='text-align:center;'>"
            "<font color='#00FFFF'><b>⚡ THROWIO MOD - AXIOM ⚡</b></font>"
            "</div><br/>"
            "<div style='text-align:center;'>"
            "<font color='#00FF00'>✅ Mod Core: ACTIVE</font>"
            "</div>"),

        // === TIỀN TỆ & MUA SẮM ===
        OBFUSCATE("Category_💰 Tiền Tệ & Mua Sắm"),
        OBFUSCATE("Toggle_Vàng Vô Hạn (Soft Currency)"),       // case 0
        OBFUSCATE("Toggle_Kim Cương Vô Hạn (Hard Currency)"), // case 1

        // === NHÂN VẬT & CHỈ SỐ ===
        OBFUSCATE("Category_⚔️ Nhân Vật & Chỉ Số"),
        OBFUSCATE("Toggle_Bất Tử (God Mode / Undead)"),     // case 2
        OBFUSCATE("Toggle_Tốc Độ Di Chuyển (Speed Hack)"),   // case 3
        OBFUSCATE("SeekBar_Hệ Số Tốc Độ_1_5"),              // case 4 (value 1-5)

        // === BOT & KẺ ĐỊCH ===
        OBFUSCATE("Category_🤖 Bot & Kẻ Địch"),
        OBFUSCATE("Toggle_Đóng Băng Bot (Freeze Bots)"),     // case 5

        // === HÒM THƯỞNG ===
        OBFUSCATE("Category_🎁 Hòm Thưởng & Phần Thưởng"),
        OBFUSCATE("Toggle_Mở Hòm Miễn Phí / Random VIP"),     // case 6

        // === QUẢNG CÁO ===
        OBFUSCATE("Category_🚫 Quảng Cáo"),
        OBFUSCATE("Toggle_Chặn & Auto Nhận Thưởng Ads"),     // case 7
    };

    int total = (sizeof features / sizeof features[0]);
    ret = (jobjectArray) env->NewObjectArray(
        total,
        env->FindClass(OBFUSCATE("java/lang/String")),
        env->NewStringUTF("")
    );
    for (int i = 0; i < total; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

// ================================================================
//  LGL - 2. CHANGES (NHẬN LỆNH TỪ MENU)
// ================================================================
void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value,
             jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0: // Vàng Vô Hạn
            bInfiniteSoftMoney = boolean;
            if (boolean && g_AppStoreDataInstance) {
                AppStoreData store(g_AppStoreDataInstance);
                store.SetSoftCurrency(0x7FFFFFFF);
            }
            break;

        case 1: // Kim Cương Vô Hạn
            bInfiniteHardCurrency = boolean;
            if (boolean && g_AppStoreDataInstance) {
                AppStoreData store(g_AppStoreDataInstance);
                store.SetHardCurrency(0x7FFFFFFF);
            }
            break;

        case 2: // Bất Tử
            bGodMode = boolean;
            if (boolean && g_CharacterStatsInstance) {
                CharacterStats stats(g_CharacterStatsInstance);
                stats.SetUndead(true);
            }
            break;

        case 3: // Tốc độ di chuyển Toggle
            bSpeedHack = boolean;
            break;

        case 4: // Hệ số tốc độ (SeekBar 1-5)
            fMoveSpeedMultiplier = (float)value;
            break;

        case 5: // Đóng băng Bot
            bFreezeBots = boolean;
            if (boolean && g_BotControllerInstance) {
                BotController bot(g_BotControllerInstance);
                bot.SetFreeze(true);
            }
            break;

        case 6: // Mở hòm miễn phí
            bFreeOpenChest = boolean;
            break;

        case 7: // Chặn quảng cáo
            bNoAds = boolean;
            break;
    }
}

// ================================================================
//  3. HACK THREAD - MAIN LOGIC
// ================================================================
void hack_thread() {
    InitCrashHandler();
    if (sigsetjmp(crash_env, 1) != 0) {
        LOGI("ThrowIO: Safe-Mode nuốt lỗi thành công");
        pthread_exit(0);
        return;
    }

    // Ngủ chờ game load xong libil2cpp.so
    sleep(12);

    uintptr_t il2cppBase = (uintptr_t)getAbsoluteAddress(
        OBFUSCATE("libil2cpp.so"), 0
    );
    if (il2cppBase == 0) {
        LOGI("ThrowIO: libil2cpp.so NOT FOUND!");
        pthread_exit(0);
        return;
    }
    LOGI("ThrowIO: libil2cpp base = 0x%lx", il2cppBase);

    // Cài đặt Hook với các RVA tiêu chuẩn mới cập nhật
    auto DoHook = [&](uintptr_t offset, void* hookFn, void** origFn, const char* name) {
        int status = DobbyHook(
            (void*)(il2cppBase + offset),
            hookFn,
            origFn
        );
        if (status != 0) {
            isHookFailed = true;
            LOGI("ThrowIO: Hook FAILED - %s", name);
        } else {
            LOGI("ThrowIO: Hook OK - %s (0x%lx)", name, offset);
        }
    };

    // Các RVA cập nhật cho AppStoreData, CharacterStats, BotController, ApplovinManager
    DoHook(0x184A2B0, (void*)hk_AppStoreData_Update,
           (void**)&old_AppStoreData_Update, "AppStoreData::Update");

    DoHook(0x186B14C, (void*)hk_CharacterStats_Update,
           (void**)&old_CharacterStats_Update, "CharacterStats::Update");

    DoHook(0x1892C80, (void*)hk_BotController_Update,
           (void**)&old_BotController_Update, "BotController::Update");

    DoHook(0x17210A4, (void*)hk_ApplovinManager_ShowRewarded,
           (void**)&old_ApplovinManager_ShowRewarded, "ApplovinManager::ShowRewarded");

    // Khởi chạy luồng giám sát bộ nhớ Watchdog
    pthread_t watchdog_t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&watchdog_t, &attr, WatchdogThread, nullptr);
    pthread_attr_destroy(&attr);

    LOGI(OBFUSCATE("ThrowIO Mod Loaded Successfully - AXIOM New Structure"));
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
