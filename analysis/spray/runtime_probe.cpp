#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <set>

// Read-only class metadata/bytecode export. No business calls, heap writes,
// retransformation, event callbacks, or game state changes.
namespace {
HMODULE self;
std::filesystem::path root;
std::ofstream logFile;
void log(const std::string& s) { logFile << s << '\n'; logFile.flush(); }
void freeJvmti(jvmtiEnv* ti, void* p) { if (p) ti->Deallocate(static_cast<unsigned char*>(p)); }
std::string hex(const unsigned char* p, int n) {
    const char* d = "0123456789abcdef"; std::string r; r.reserve(n * 2);
    for (int i = 0; i < n; ++i) { r += d[p[i] >> 4]; r += d[p[i] & 15]; }
    return r;
}
DWORD WINAPI run(void*) {
    wchar_t path[32768]{}; GetModuleFileNameW(self, path, 32768);
    root = std::filesystem::path(path).parent_path();
    std::filesystem::create_directories(root / L"classes");
    logFile.open(root / L"probe.log", std::ios::app);
    log("READ_ONLY_PROBE pid=" + std::to_string(GetCurrentProcessId()));
    auto jvm = GetModuleHandleW(L"jvm.dll");
    using GetVMs = jint (JNICALL*)(JavaVM**, jsize, jsize*);
    auto getVMs = jvm ? reinterpret_cast<GetVMs>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs")) : nullptr;
    JavaVM* vm = nullptr; jsize n = 0;
    if (!getVMs || getVMs(&vm, 1, &n) != JNI_OK || !n) { log("no VM"); return 1; }
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) return 2;
    jvmtiEnv* ti = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&ti), JVMTI_VERSION_1_2) != JNI_OK) { vm->DetachCurrentThread(); return 3; }
    jvmtiCapabilities caps{}; caps.can_get_constant_pool = 1; caps.can_get_bytecodes = 1;
    const auto capError = ti->AddCapabilities(&caps); log("capabilities=" + std::to_string(capError));
    if (capError != JVMTI_ERROR_NONE) { ti->DisposeEnvironment(); vm->DetachCurrentThread(); return 4; }
    jint count = 0; jclass* classes = nullptr;
    const auto ce = ti->GetLoadedClasses(&count, &classes);
    log("GetLoadedClasses=" + std::to_string(ce) + " count=" + std::to_string(count));
    std::set<std::string> targets = {"net/badlion/a/aCV", "net/badlion/a/aCY", "net/badlion/a/aDp", "net/badlion/a/aNI",
        "net/badlion/clientcommon/type/cosmetics/response/a", "net/badlion/clientcommon/type/cosmetics/response/b"};
    std::ifstream extra(root / L"targets.txt"); std::string line;
    while (std::getline(extra, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) targets.insert(line); }
    int exported = 0, scanned = 0;
    for (int i = 0; i < count; ++i) {
        char* sig = nullptr; ti->GetClassSignature(classes[i], &sig, nullptr);
        std::string signature = sig ? sig : ""; freeJvmti(ti, sig);
        if (signature.rfind("Lnet/badlion/", 0) != 0) { env->DeleteLocalRef(classes[i]); continue; }
        std::string name = signature.substr(1, signature.size() - 2);
        jint cpCount = 0, cpSize = 0; unsigned char* cp = nullptr;
        const auto cpError = ti->GetConstantPool(classes[i], &cpCount, &cpSize, &cp); ++scanned;
        std::string strings = cp ? std::string(reinterpret_cast<char*>(cp), cpSize) : "";
        std::transform(strings.begin(), strings.end(), strings.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        const bool selected = targets.count(name) || strings.find("spray") != std::string::npos;
        if (selected) {
            auto prefix = root / L"classes" / std::filesystem::path(name);
            std::filesystem::create_directories(prefix.parent_path());
            std::ofstream out(prefix.string() + ".meta", std::ios::binary);
            jint mods = 0; ti->GetClassModifiers(classes[i], &mods);
            out << "CLASS\t" << name << '\t' << mods << '\t' << cpCount << '\t' << cpError << '\n';
            if (cp) { std::ofstream binary(prefix.string() + ".cp", std::ios::binary); binary.write(reinterpret_cast<char*>(cp), cpSize); }
            jclass super = env->GetSuperclass(classes[i]);
            if (super) { char* s = nullptr; ti->GetClassSignature(super, &s, nullptr); out << "SUPER\t" << (s ? s : "") << '\n'; freeJvmti(ti, s); env->DeleteLocalRef(super); }
            jint fieldCount = 0; jfieldID* fields = nullptr; ti->GetClassFields(classes[i], &fieldCount, &fields);
            for (int f = 0; f < fieldCount; ++f) {
                char *fn = nullptr, *fs = nullptr; jint fm = 0;
                ti->GetFieldName(classes[i], fields[f], &fn, &fs, nullptr); ti->GetFieldModifiers(classes[i], fields[f], &fm);
                out << "FIELD\t" << fm << '\t' << (fn ? fn : "") << '\t' << (fs ? fs : "") << '\n';
                freeJvmti(ti, fn); freeJvmti(ti, fs);
            }
            freeJvmti(ti, fields);
            jint methodCount = 0; jmethodID* methods = nullptr; ti->GetClassMethods(classes[i], &methodCount, &methods);
            for (int m = 0; m < methodCount; ++m) {
                char *mn = nullptr, *ms = nullptr; jint mm = 0;
                ti->GetMethodName(methods[m], &mn, &ms, nullptr); ti->GetMethodModifiers(methods[m], &mm);
                jint len = 0; unsigned char* bytes = nullptr; auto e = ti->GetBytecodes(methods[m], &len, &bytes);
                out << "METHOD\t" << mm << '\t' << (mn ? mn : "") << '\t' << (ms ? ms : "") << '\t' << e << '\t' << hex(bytes, len) << '\n';
                freeJvmti(ti, mn); freeJvmti(ti, ms); freeJvmti(ti, bytes);
            }
            freeJvmti(ti, methods); ++exported;
            log("EXPORT " + name + " cp=" + std::to_string(cpCount) + " error=" + std::to_string(cpError) + " methods=" + std::to_string(methodCount));
        }
        freeJvmti(ti, cp); env->DeleteLocalRef(classes[i]);
    }
    freeJvmti(ti, classes);
    log("DONE scanned=" + std::to_string(scanned) + " exported=" + std::to_string(exported));
    ti->DisposeEnvironment(); vm->DetachCurrentThread(); return 0;
}
}
BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { self = m; DisableThreadLibraryCalls(m); HANDLE t = CreateThread(nullptr, 0, run, nullptr, 0, nullptr); if (t) CloseHandle(t); }
    return TRUE;
}
