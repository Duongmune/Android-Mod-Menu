#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <cstring>
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

// ================= HỆ THỐNG NUỐT LỖI (SAFE-MODE CHỐNG CRASH) =================
sigjmp_buf crash_env;
bool isCrashHandled = false;
bool isHookFailed = false;

void CrashHandler(int sig, siginfo_t *info, void *context) {
    isHookFailed = true; // Bật cờ lỗi để vô hiệu hóa chức năng ngầm
    if (!isCrashHandled) {
        isCrashHandled = true;
        siglongjmp(crash_env, 1); // Đẩy luồng về điểm Checkpoint an toàn
    } else {
        pthread_exit(0); // Nếu cố cứu vẫn chết -> Hủy Mod
    }
}

void InitCrashHandler() {
    struct sigaction sa;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER; 
    sa.sa_sigaction = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); // Bắt lỗi tràn RAM
    sigaction(SIGABRT, &sa, NULL); // Bắt lỗi đóng ứng dụng
    sigaction(SIGBUS, &sa, NULL);  // Bắt lỗi rác bộ nhớ
}
// ==============================================================================


// ================= KHAI BÁO BIẾN & LOGIC CAM XA 2026 ==========================
bool isCamXa = false;
float doXaCam = 1.2f;

float (*old_get_currentZoomRate)(void* instance);

float hook_get_currentZoomRate(void* instance) {
    // Nếu hệ thống an toàn và bật Cam -> Trả về độ xa
    if (isCamXa && !isHookFailed) return doXaCam;
    // Nếu Lỗi hoặc chưa Bật -> Trả về Camera gốc
    if (old_get_currentZoomRate != nullptr) return old_get_currentZoomRate(instance);
    return 1.0f; 
}
// ==============================================================================


// ================= 1. THIẾT KẾ GIAO DIỆN MENU LGL ==============================
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;
    const char *features[] = {
        // --- TEXT TRANG TRÍ (KHÔNG ĐƯỢC TÍNH LÀ CHỨC NĂNG) ---
        OBFUSCATE("RichTextView_<div style='text-align:center;'><font color='#00FFFF'><b>CHANEL TEAM MOD LQM BETA × [64]</b></font></div><br/>"
                  "<div style='text-align:center;'><font color='#00FF00'>Trạng thái: An toàn (Bypass MTP)</font></div>"),
        OBFUSCATE("Category_Tính Năng VIP"),

        // --- CÁC NÚT BẤM (BẮT ĐẦU TÍNH TỪ 0) ---
        OBFUSCATE("Toggle_Bật Cam Xa (Safe Mode)"), // Tương ứng với case 0
        OBFUSCATE("SeekBar_Độ xa Camera_10_18")     // Tương ứng với case 1 (10=1.0f, 18=1.8f)
    };
    
    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray) env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < Total_Feature; i++) env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return (ret);
}


// ================= 2. NHẬN LỆNH TỪ GIAO DIỆN ==================================
void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0:
            isCamXa = boolean;
            break;
        case 1:
            // SeekBar của LGL chỉ dùng số nguyên, nên phải chia 10
            doXaCam = (float)value / 10.0f; 
            break;
    }
}


// ================= 3. LUỒNG HOẠT ĐỘNG CHÍNH (NINJA MODE 15s) ===================
void hack_thread() {
    // 1. DỰNG LÁ CHẮN BẢO VỆ
    InitCrashHandler();
    if (sigsetjmp(crash_env, 1) != 0) {
        LOGI("ChanelTeam Safe-Mode: Nuốt lỗi Crash thành công, trả lại Game gốc!");
        pthread_exit(0);
        return;
    }

    // 2. NGỦ ĐÔNG 15 GIÂY: Qua mặt hoàn toàn Tencent MTP lúc Load Tài Nguyên
    sleep(15);

    // 3. TÌM LÕI VÀ MÓC OFFSET (ĐÃ FIX LỖI ÉP KIỂU UINTPTR_T)
    uintptr_t il2cppBase = (uintptr_t)getAbsoluteAddress(OBFUSCATE("libil2cpp.so"), 0);
    if (il2cppBase == 0) {
        pthread_exit(0);
        return;
    }

    uintptr_t target = il2cppBase + 0x71BB344;
    int status = DobbyHook((void*)target, (void*)hook_get_currentZoomRate, (void**)&old_get_currentZoomRate);
    
    if (status != 0) {
        isHookFailed = true; // Lỗi Offset thì tự khóa tính năng, không làm văng game
    }

    LOGI(OBFUSCATE("ChanelTeam Mod Loaded Successfully"));
}

// Hàm chạy đầu tiên khi load thư viện
__attribute__((constructor))
void lib_main() {
    // Tạo Thread ngầm, không block Game
    std::thread(hack_thread).detach();
}
