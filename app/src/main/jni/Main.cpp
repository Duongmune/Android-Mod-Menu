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
//  THROW.IO NEW OFFSETS (dump mới - RVA)
// ================================================================
namespace Offsets {
    // PlayerBalanceNew
    constexpr uintptr_t set_SoftMoney_New    = 0x1315B48;
    constexpr uintptr_t set_HardMoney_New    = 0x1315B58;
    constexpr uintptr_t set_Level_New        = 0x1315B68;
    constexpr uintptr_t set_Exp_New          = 0x1315B78;
    constexpr uintptr_t set_Energy_New       = 0x1313408;
    constexpr uintptr_t set_NoAds_New        = 0x130B728;
    constexpr uintptr_t set_NoAds2_New       = 0x1312AE4;
    constexpr uintptr_t set_VipActive_New    = 0x130B874;
    constexpr uintptr_t set_Key1_New         = 0x1315B88;
    constexpr uintptr_t set_Key2_New         = 0x1315B98;

    // PlayerBalance cũ (vẫn tồn tại)
    constexpr uintptr_t set_SoftMoney_Old    = 0x1315CFC;
    constexpr uintptr_t set_HardMoney_Old    = 0x1315D7C;
    constexpr uintptr_t set_Level_Old        = 0x1315DFC;
    constexpr uintptr_t set_Energy_Old       = 0x1315EFC;
    constexpr uintptr_t set_NoAds_Old        = 0x1315C0C;
    constexpr uintptr_t set_VipActive_Old    = 0x1315F5C;

    // Character
    constexpr uintptr_t set_undead          = 0x12FA72C;
    constexpr uintptr_t SetMoveSpeedFactor  = 0x12FB514;
    constexpr uintptr_t ApplyDamage         = 0x12FB55C;

    // PlayerData
    constexpr uintptr_t SaveLocal           = 0x1312B10;
    constexpr uintptr_t UnlockSkin          = 0x130B4A8;
    constexpr uintptr_t AddMoney            = 0x130B2C0;
    constexpr uintptr_t AddExp              = 0x1313788;

    // AuthManager
    constexpr uintptr_t Authenticate        = 0x12EA730;
    constexpr uintptr_t SavePlayerData      = 0x12EAEF0;

    // AppStore
    constexpr uintptr_t giveRewards         = 0x12E7BF0;
}

// ================================================================
//  GLOBAL TOGGLES
// ================================================================
bool bInfiniteMoneySoft = false;
bool bInfiniteMoneyHard = false;
bool bInfiniteEnergy    = false;
bool bAutoMaxLevel      = false;
bool bGodMode           = false;
bool bSpeedHack         = false;
float speedFactor       = 3.0f;
bool bNoDamage          = false;
bool bNoAds             = false;
bool bVipActive         = false;
bool bUnlockAllWeapons  = false;
bool bUnlockAllSkins    = false;
bool bBypassAntiCheat   = true;

int targetLevel = 99;

// ================================================================
//  FUNCTION POINTERS
// ================================================================
using fn_void_int64 = void (*)(void*, int64_t);
using fn_void_int   = void (*)(void*, int);
using fn_void_bool  = void (*)(void*, bool);
using fn_void_float = void (*)(void*, float);
using fn_bool_self_damage = bool (*)(void*, int64_t, void*, bool, bool, int);

// ================================================================
//  OLD HOOK TARGETS
// ================================================================
fn_void_int64 old_set_SoftMoney_New = nullptr;
fn_void_int64 old_set_HardMoney_New = nullptr;
fn_void_int   old_set_Level_New     = nullptr;
fn_void_int   old_set_Energy_New    = nullptr;
fn_void_bool  old_set_NoAds_New     = nullptr;
fn_void_bool  old_set_VipActive_New = nullptr;
fn_void_int64 old_set_SoftMoney_Old = nullptr;
fn_void_int64 old_set_HardMoney_Old = nullptr;
fn_void_int   old_set_Level_Old     = nullptr;
fn_void_int   old_set_Energy_Old    = nullptr;
fn_void_bool  old_set_NoAds_Old     = nullptr;
fn_void_bool  old_set_VipActive_Old = nullptr;
fn_void_bool  old_set_undead        = nullptr;
fn_void_float old_SetMoveSpeedFactor = nullptr;
fn_bool_self_damage old_ApplyDamage = nullptr;

// ================================================================
//  JVM HELPER - Lấy JavaVM không cần JNI_OnLoad
// ================================================================
static JavaVM* jvm = nullptr;

JNIEnv* GetJNIEnv() {
    JNIEnv* env = nullptr;
    if (!jvm) {
        jsize count = 0;
        if (JNI_GetCreatedJavaVMs(&jvm, 1, &count) != JNI_OK || count == 0) {
            return nullptr;
        }
    }
    if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    return nullptr;
}

jobject GetGlobalContext() {
    JNIEnv* env = GetJNIEnv();
    if (!env) return nullptr;
    jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
    if (!activityThreadClass) return nullptr;
    jmethodID currentActivityThread = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThread);
    if (!activityThread) return nullptr;
    jmethodID getApplication = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
    jobject app = env->CallObjectMethod(activityThread, getApplication);
    if (!app) return nullptr;
    jclass appClass = env->GetObjectClass(app);
    jmethodID getApplicationContext = env->GetMethodID(appClass, "getApplicationContext", "()Landroid/content/Context;");
    jobject context = env->CallObjectMethod(app, getApplicationContext);
    env->DeleteLocalRef(activityThreadClass);
    env->DeleteLocalRef(activityThread);
    env->DeleteLocalRef(appClass);
    env->DeleteLocalRef(app);
    return context;
}

// ================================================================
//  HOOK FUNCTIONS
// ================================================================

void hk_set_undead(void* self, bool value) {
    if (bGodMode) value = true;
    if (old_set_undead) old_set_undead(self, value);
}

void hk_SetMoveSpeedFactor(void* self, float factor) {
    if (bSpeedHack) factor *= speedFactor;
    if (old_SetMoveSpeedFactor) old_SetMoveSpeedFactor(self, factor);
}

bool hk_ApplyDamage(void* self, int64_t damage, void* from, bool isCritical,
                    bool poisonAttack, int damageSource) {
    if (bNoDamage) return false;
    return old_ApplyDamage ? old_ApplyDamage(self, damage, from, isCritical,
                                             poisonAttack, damageSource) : false;
}

void hk_set_SoftMoney_New(void* self, int64_t value) {
    if (bInfiniteMoneySoft) value = 999999999;
    if (old_set_SoftMoney_New) old_set_SoftMoney_New(self, value);
}
void hk_set_SoftMoney_Old(void* self, int64_t value) {
    if (bInfiniteMoneySoft) value = 999999999;
    if (old_set_SoftMoney_Old) old_set_SoftMoney_Old(self, value);
}

void hk_set_HardMoney_New(void* self, int64_t value) {
    if (bInfiniteMoneyHard) value = 999999999;
    if (old_set_HardMoney_New) old_set_HardMoney_New(self, value);
}
void hk_set_HardMoney_Old(void* self, int64_t value) {
    if (bInfiniteMoneyHard) value = 999999999;
    if (old_set_HardMoney_Old) old_set_HardMoney_Old(self, value);
}

void hk_set_Level_New(void* self, int value) {
    if (bAutoMaxLevel) value = 99;
    if (old_set_Level_New) old_set_Level_New(self, value);
}
void hk_set_Level_Old(void* self, int value) {
    if (bAutoMaxLevel) value = 99;
    if (old_set_Level_Old) old_set_Level_Old(self, value);
}

void hk_set_Energy_New(void* self, int64_t value) {
    if (bInfiniteEnergy) value = 9999;
    if (old_set_Energy_New) old_set_Energy_New(self, value);
}
void hk_set_Energy_Old(void* self, int64_t value) {
    if (bInfiniteEnergy) value = 9999;
    if (old_set_Energy_Old) old_set_Energy_Old(self, value);
}

void hk_set_NoAds_New(void* self, bool value) {
    if (bNoAds) value = true;
    if (old_set_NoAds_New) old_set_NoAds_New(self, value);
}
void hk_set_NoAds_Old(void* self, bool value) {
    if (bNoAds) value = true;
    if (old_set_NoAds_Old) old_set_NoAds_Old(self, value);
}

void hk_set_VipActive_New(void* self, bool value) {
    if (bVipActive) value = true;
    if (old_set_VipActive_New) old_set_VipActive_New(self, value);
}
void hk_set_VipActive_Old(void* self, bool value) {
    if (bVipActive) value = true;
    if (old_set_VipActive_Old) old_set_VipActive_Old(self, value);
}

// ================================================================
//  LGL MENU - FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;
    const char *features[] = {
        OBFUSCATE("RichTextView_"
            "<div style='text-align:center;'>"
            "<font color='#00FFFF'><b>⚡ THROW.IO MOD - AXIOM ⚡</b></font>"
            "</div><br/>"
            "<div style='text-align:center;'>"
            "<font color='#00FF00'>✅ Anti-Cheat Bypass: ACTIVE</font>"
            "</div>"),

        OBFUSCATE("Category_💰 Tiền Tệ"),
        OBFUSCATE("Toggle_Tiền Mềm Vô Hạn"),
        OBFUSCATE("Toggle_Tiền Cứng Vô Hạn"),
        OBFUSCATE("Toggle_Năng Lượng Vô Hạn"),

        OBFUSCATE("Category_⭐ Tiến Trình"),
        OBFUSCATE("Toggle_Auto Max Level 99"),
        OBFUSCATE("SeekBar_Set Level_1_99"),

        OBFUSCATE("Category_⚔️ Chiến Đấu"),
        OBFUSCATE("Toggle_God Mode"),
        OBFUSCATE("Toggle_Không Sát Thương"),
        OBFUSCATE("SeekBar_Tốc Độ_1_5"),

        OBFUSCATE("Category_🛡️ Bảo Mật"),
        OBFUSCATE("Toggle_Bypass Anti-Cheat"),
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
//  LGL MENU - CHANGES
// ================================================================
void Changes(JNIEnv *env, jclass clazz, jobject obj,
             jint featNum, jstring featName, jint value,
             jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0: bInfiniteMoneySoft = boolean; break;
        case 1: bInfiniteMoneyHard = boolean; break;
        case 2: bInfiniteEnergy    = boolean; break;
        case 3: bAutoMaxLevel      = boolean; break;
        case 4: targetLevel        = value; break;
        case 5: bGodMode           = boolean; break;
        case 6: bNoDamage          = boolean; break;
        case 7:
            bSpeedHack = true;
            speedFactor = 1.0f + (value / 100.0f) * 4.0f; // 1-5
            if (value == 0) bSpeedHack = false;
            break;
        case 8: bBypassAntiCheat   = boolean; break;
    }
}

// ================================================================
//  HACK THREAD - INSTALL HOOKS
// ================================================================
void hack_thread() {
    InitCrashHandler();
    if (sigsetjmp(crash_env, 1) != 0) {
        LOGI("ThrowIO: Safe-Mode nuốt lỗi thành công");
        pthread_exit(0);
        return;
    }

    sleep(15); // chờ game load

    // ========== GỌI MENU INIT ==========
    JNIEnv* env = GetJNIEnv();
    jobject context = GetGlobalContext();
    if (env && context) {
        jstring title = env->NewStringUTF("THROW.IO MOD");
        jstring subtitle = env->NewStringUTF("by Axiom");
        Init(env, nullptr, context, title, subtitle);
        env->DeleteLocalRef(title);
        env->DeleteLocalRef(subtitle);
        LOGI("Menu Init OK");
    } else {
        LOGI("Menu Init FAIL - env=%p context=%p", env, context);
    }

    uintptr_t il2cppBase = (uintptr_t)getAbsoluteAddress(
        OBFUSCATE("libil2cpp.so"), 0
    );
    if (il2cppBase == 0) {
        LOGI("ThrowIO: libil2cpp.so NOT FOUND!");
        pthread_exit(0);
        return;
    }
    LOGI("ThrowIO: libil2cpp base = 0x%lx", il2cppBase);

    auto DoHook = [&](uintptr_t offset, void* hookFn, void** origFn, const char* name) {
        int status = DobbyHook((void*)(il2cppBase + offset), hookFn, origFn);
        if (status != 0) {
            isHookFailed = true;
            LOGI("ThrowIO: Hook FAILED - %s", name);
        } else {
            LOGI("ThrowIO: Hook OK - %s (0x%lx)", name, offset);
        }
    };

    // PlayerBalanceNew
    DoHook(Offsets::set_SoftMoney_New, (void*)hk_set_SoftMoney_New, (void**)&old_set_SoftMoney_New, "set_SoftMoney_New");
    DoHook(Offsets::set_HardMoney_New, (void*)hk_set_HardMoney_New, (void**)&old_set_HardMoney_New, "set_HardMoney_New");
    DoHook(Offsets::set_Level_New,     (void*)hk_set_Level_New,     (void**)&old_set_Level_New,     "set_Level_New");
    DoHook(Offsets::set_Energy_New,    (void*)hk_set_Energy_New,    (void**)&old_set_Energy_New,    "set_Energy_New");
    DoHook(Offsets::set_NoAds_New,     (void*)hk_set_NoAds_New,     (void**)&old_set_NoAds_New,     "set_NoAds_New");
    DoHook(Offsets::set_VipActive_New, (void*)hk_set_VipActive_New, (void**)&old_set_VipActive_New, "set_VipActive_New");

    // PlayerBalance cũ
    DoHook(Offsets::set_SoftMoney_Old, (void*)hk_set_SoftMoney_Old, (void**)&old_set_SoftMoney_Old, "set_SoftMoney_Old");
    DoHook(Offsets::set_HardMoney_Old, (void*)hk_set_HardMoney_Old, (void**)&old_set_HardMoney_Old, "set_HardMoney_Old");
    DoHook(Offsets::set_Level_Old,     (void*)hk_set_Level_Old,     (void**)&old_set_Level_Old,     "set_Level_Old");
    DoHook(Offsets::set_Energy_Old,    (void*)hk_set_Energy_Old,    (void**)&old_set_Energy_Old,    "set_Energy_Old");
    DoHook(Offsets::set_NoAds_Old,     (void*)hk_set_NoAds_Old,     (void**)&old_set_NoAds_Old,     "set_NoAds_Old");
    DoHook(Offsets::set_VipActive_Old, (void*)hk_set_VipActive_Old, (void**)&old_set_VipActive_Old, "set_VipActive_Old");

    // Character
    DoHook(Offsets::set_undead,          (void*)hk_set_undead,          (void**)&old_set_undead,          "set_undead");
    DoHook(Offsets::SetMoveSpeedFactor,  (void*)hk_SetMoveSpeedFactor,  (void**)&old_SetMoveSpeedFactor,  "SetMoveSpeedFactor");
    DoHook(Offsets::ApplyDamage,         (void*)hk_ApplyDamage,         (void**)&old_ApplyDamage,         "ApplyDamage");

    LOGI(OBFUSCATE("ThrowIO Mod Loaded Successfully - AXIOM"));
}

// ================================================================
//  ENTRY POINT
// ================================================================
__attribute__((constructor))
void lib_main() {
    std::thread(hack_thread).detach();
}
