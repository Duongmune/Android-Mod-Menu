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
//  GAME OBJECT WRAPPERS - THROWIO
// ================================================================
struct Vector2 { float x, y; };

class PlayerBalance {
public:
    void* instance;
    PlayerBalance(void* obj) : instance(obj) {}

    long GetSoftMoney() { return SafeRead<long>((uintptr_t)instance + 0x18); }
    void SetSoftMoney(long v) { SafeWrite<long>((uintptr_t)instance + 0x18, v); }
    long GetHardMoney() { return SafeRead<long>((uintptr_t)instance + 0x20); }
    void SetHardMoney(long v) { SafeWrite<long>((uintptr_t)instance + 0x20, v); }
    int  GetLevel()  { return SafeRead<int>((uintptr_t)instance + 0x30); }
    void SetLevel(int v) { SafeWrite<int>((uintptr_t)instance + 0x30, v); }
    int  GetExp()    { return SafeRead<int>((uintptr_t)instance + 0x34); }
    void SetExp(int v)   { SafeWrite<int>((uintptr_t)instance + 0x34, v); }
    int  GetCoins()  { return SafeRead<int>((uintptr_t)instance + 0x2C); }
    void SetCoins(int v) { SafeWrite<int>((uintptr_t)instance + 0x2C, v); }
};

class PlayerEquipment {
public:
    void* instance;
    PlayerEquipment(void* obj) : instance(obj) {}

    int  GetWeapon() { return SafeRead<int>((uintptr_t)instance + 0x10); }
    void SetWeapon(int id) { SafeWrite<int>((uintptr_t)instance + 0x10, id); }
    int  GetArmor()  { return SafeRead<int>((uintptr_t)instance + 0x14); }
    void SetArmor(int id)  { SafeWrite<int>((uintptr_t)instance + 0x14, id); }
};

class PlayerController {
public:
    void* instance;
    PlayerController(void* obj) : instance(obj) {}

    bool GetFire() { return SafeRead<bool>((uintptr_t)instance + 0x30); }
    void SetFire(bool v) { SafeWrite<bool>((uintptr_t)instance + 0x30, v); }
};

// ================================================================
//  GLOBAL STATE - THROWIO
// ================================================================
void* g_PlayerDataInstance       = nullptr;
void* g_PlayerBalanceInstance    = nullptr;
void* g_PlayerEquipmentInstance  = nullptr;
void* g_PlayerControllerInstance = nullptr;

// Toggles
bool bInfiniteMoney    = false;
bool bInfinitePremium  = false;
bool bAutoMaxLevel     = false;
bool bAutoFire         = false;
bool bInfiniteEnergy   = false;
bool bBypassAntiCheat  = true;

// Values
int targetLevel    = 99;
int targetWeaponId = 0;
int targetArmorId  = 0;

// ================================================================
//  HOOKS - THROWIO NEW OFFSETS
// ================================================================

// --- PlayerController::update() RVA: 0x187B4A4 ---
typedef void(*PlayerControllerUpdateFn)(void* self, float dt);
PlayerControllerUpdateFn old_PlayerController_update = nullptr;

void hk_PlayerController_update(void* self, float dt) {
    old_PlayerController_update(self, dt);
    if (!self || isHookFailed) return;

    g_PlayerControllerInstance = self;
    PlayerController ctrl(self);

    if (bAutoFire) ctrl.SetFire(true);
}

// --- PlayerData::Awake() - Capture instances ---
typedef void(*PlayerDataAwakeFn)(void* self);
PlayerDataAwakeFn old_PlayerData_Awake = nullptr;

void hk_PlayerData_Awake(void* self) {
    if (!self) { old_PlayerData_Awake(self); return; }

    g_PlayerDataInstance = self;
    g_PlayerBalanceInstance   = SafeRead<void*>((uintptr_t)self + 0x30);
    g_PlayerEquipmentInstance = SafeRead<void*>((uintptr_t)self + 0x28);

    LOGI("ThrowIO: PlayerData captured | Balance=%p | Equipment=%p",
         g_PlayerBalanceInstance, g_PlayerEquipmentInstance);

    old_PlayerData_Awake(self);
}

// --- PlayerData::SaveLocal() RVA: 0x185C9EC ---
typedef void(*PlayerData_SaveLocalFn)(void* self);
PlayerData_SaveLocalFn old_PlayerData_SaveLocal = nullptr;

void hk_PlayerData_SaveLocal(void* self) {
    if (self && bBypassAntiCheat && g_PlayerBalanceInstance && !isHookFailed) {
        PlayerBalance bal(g_PlayerBalanceInstance);
        if (bInfiniteMoney)   bal.SetSoftMoney(0x7FFFFFFF);
        if (bInfinitePremium) bal.SetHardMoney(0x7FFFFFFF);
        if (bInfiniteEnergy)  bal.SetCoins(9999);
        if (bAutoMaxLevel)  { bal.SetLevel(99); bal.SetExp(0x7FFFFFFF); }
    }

    old_PlayerData_SaveLocal(self);

    // Restore after save
    if (self && bBypassAntiCheat && g_PlayerBalanceInstance && !isHookFailed) {
        PlayerBalance bal(g_PlayerBalanceInstance);
        if (bInfiniteMoney)   bal.SetSoftMoney(0x7FFFFFFF);
        if (bInfinitePremium) bal.SetHardMoney(0x7FFFFFFF);
        if (bAutoMaxLevel)    bal.SetLevel(99);
    }
}

// --- AuthManager::Authenticate() RVA: 0x17485AC ---
typedef void(*AuthManager_AuthenticateFn)(void* self);
AuthManager_AuthenticateFn old_AuthManager_Authenticate = nullptr;

void hk_AuthManager_Authenticate(void* self) {
    old_AuthManager_Authenticate(self);
    LOGI("ThrowIO: Auth bypassed");
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
        if (!g_PlayerBalanceInstance || !g_PlayerEquipmentInstance) continue;

        try {
            PlayerBalance bal(g_PlayerBalanceInstance);
            PlayerEquipment eq(g_PlayerEquipmentInstance);

            if (bInfiniteMoney)   bal.SetSoftMoney(0x7FFFFFFF);
            if (bInfinitePremium) bal.SetHardMoney(0x7FFFFFFF);
            if (bInfiniteEnergy)  bal.SetCoins(9999);
            if (bAutoMaxLevel)  { bal.SetLevel(99); bal.SetExp(0x7FFFFFFF); }
            if (targetWeaponId > 0) eq.SetWeapon(targetWeaponId);
            if (targetArmorId  > 0) eq.SetArmor(targetArmorId);

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
//  LGL - 1. FEATURE LIST (GIAO DIỆN MENU)
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
            "<font color='#00FF00'>✅ Anti-Cheat Bypass: ACTIVE</font>"
            "</div>"),

        // === TIỀN TỆ ===
        OBFUSCATE("Category_💰 Tiền Tệ"),
        OBFUSCATE("Toggle_Tiền Mềm Vô Hạn"),      // case 0
        OBFUSCATE("Toggle_Tiền Hạp Tác Vô Hạn"),   // case 1

        // === TIẾN TRÌNH ===
        OBFUSCATE("Category_⭐ Tiến Trình"),
        OBFUSCATE("Toggle_Auto Max Level 99"),       // case 2
        OBFUSCATE("SeekBar_Set Level_1_99"),         // case 3 (value = 1-99)

        // === TRANG BỊ ===
        OBFUSCATE("Category_⚔️ Trang Bị"),
        OBFUSCATE("SeekBar_Vũ Khí ID_0_50"),        // case 4
        OBFUSCATE("SeekBar_Giáp ID_0_50"),           // case 5

        // === CHƠI GAME ===
        OBFUSCATE("Category_🎮 Chơi Game"),
        OBFUSCATE("Toggle_Auto Fire"),               // case 6
        OBFUSCATE("Toggle_Năng Lượng Vô Hạn"),      // case 7

        // === BẢO MẬT ===
        OBFUSCATE("Category_🛡️ Anti-Cheat"),
        OBFUSCATE("Toggle_Bypass Anti-Cheat"),       // case 8
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
        case 0: // Tiền Mềm Vô Hạn
            bInfiniteMoney = boolean;
            if (boolean && g_PlayerBalanceInstance) {
                PlayerBalance bal(g_PlayerBalanceInstance);
                bal.SetSoftMoney(0x7FFFFFFF);
            }
            break;

        case 1: // Tiền Hạp Tác Vô Hạn
            bInfinitePremium = boolean;
            if (boolean && g_PlayerBalanceInstance) {
                PlayerBalance bal(g_PlayerBalanceInstance);
                bal.SetHardMoney(0x7FFFFFFF);
            }
            break;

        case 2: // Auto Max Level
            bAutoMaxLevel = boolean;
            if (boolean && g_PlayerBalanceInstance) {
                PlayerBalance bal(g_PlayerBalanceInstance);
                bal.SetLevel(99);
                bal.SetExp(0x7FFFFFFF);
            }
            break;

        case 3: // Set Level (SeekBar 1-99)
            targetLevel = value;
            if (g_PlayerBalanceInstance) {
                PlayerBalance bal(g_PlayerBalanceInstance);
                bal.SetLevel(value);
                bal.SetExp(0x7FFFFFFF);
            }
            break;

        case 4: // Weapon ID (SeekBar 0-50)
            targetWeaponId = value;
            if (g_PlayerEquipmentInstance && value > 0) {
                PlayerEquipment eq(g_PlayerEquipmentInstance);
                eq.SetWeapon(value);
            }
            break;

        case 5: // Armor ID (SeekBar 0-50)
            targetArmorId = value;
            if (g_PlayerEquipmentInstance && value > 0) {
                PlayerEquipment eq(g_PlayerEquipmentInstance);
                eq.SetArmor(value);
            }
            break;

        case 6: // Auto Fire
            bAutoFire = boolean;
            break;

        case 7: // Năng Lượng Vô Hạn
            bInfiniteEnergy = boolean;
            if (boolean && g_PlayerBalanceInstance) {
                PlayerBalance bal(g_PlayerBalanceInstance);
                bal.SetCoins(9999);
            }
            break;

        case 8: // Bypass Anti-Cheat
            bBypassAntiCheat = boolean;
            break;
    }
}

// ================================================================
//  3. HACK THREAD - MAIN LOGIC
// ================================================================
void hack_thread() {
    // Dựng lá chắn crash
    InitCrashHandler();
    if (sigsetjmp(crash_env, 1) != 0) {
        LOGI("ThrowIO: Safe-Mode nuốt lỗi thành công");
        pthread_exit(0);
        return;
    }

    // Ngủ 15 giây - bypass anti-cheat lúc load
    sleep(15);

    // Tìm libil2cpp
    uintptr_t il2cppBase = (uintptr_t)getAbsoluteAddress(
        OBFUSCATE("libil2cpp.so"), 0
    );
    if (il2cppBase == 0) {
        LOGI("ThrowIO: libil2cpp.so NOT FOUND!");
        pthread_exit(0);
        return;
    }
    LOGI("ThrowIO: libil2cpp base = 0x%lx", il2cppBase);

    // Install hooks
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

    DoHook(0x187B4A4, (void*)hk_PlayerController_update,
           (void**)&old_PlayerController_update, "PlayerController::update");

    DoHook(0x185C9EC, (void*)hk_PlayerData_SaveLocal,
           (void**)&old_PlayerData_SaveLocal, "PlayerData::SaveLocal");

    DoHook(0x17485AC, (void*)hk_AuthManager_Authenticate,
           (void**)&old_AuthManager_Authenticate, "AuthManager::Authenticate");

    // Start watchdog thread
    pthread_t watchdog_t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&watchdog_t, &attr, WatchdogThread, nullptr);
    pthread_attr_destroy(&attr);

    LOGI(OBFUSCATE("ThrowIO Mod Loaded Successfully - AXIOM"));
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
