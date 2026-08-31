#include <windows.h>
#include <jni.h>
#include <jvmti.h>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <set>
#include <system_error>

namespace {
HMODULE g_module = nullptr;
std::wstring g_logPath;
std::wstring g_selectionPath;
jobject g_persistentCatalog = nullptr;

struct SelectionKey {
    std::string type;
    jint id = 0;

    bool operator<(const SelectionKey& other) const {
        return type < other.type || (type == other.type && id < other.id);
    }

    bool operator==(const SelectionKey& other) const {
        return type == other.type && id == other.id;
    }
};

using SelectionSet = std::set<SelectionKey>;
SelectionSet g_selectionToRestore;
bool g_hasSavedSelection = false;
bool g_selectionPrepared = false;
bool g_unlockInstalled = false;
bool g_catalogReady = false;

void logLine(const std::string& line) {
    std::ofstream file(g_logPath, std::ios::app);
    file << line << "\n";
    OutputDebugStringA(("[blc_unlock_agent] " + line + "\n").c_str());
}

std::string jstringToUtf8(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

bool clearException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return false;
    jthrowable throwable = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string detail;
    if (throwable) {
        jclass throwableClass = env->GetObjectClass(throwable);
        jmethodID toString = throwableClass ? env->GetMethodID(throwableClass, "toString", "()Ljava/lang/String;") : nullptr;
        jstring text = (toString && throwableClass) ? static_cast<jstring>(env->CallObjectMethod(throwable, toString)) : nullptr;
        if (!env->ExceptionCheck() && text) detail = jstringToUtf8(env, text);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (text) env->DeleteLocalRef(text);
        if (throwableClass) env->DeleteLocalRef(throwableClass);
        env->DeleteLocalRef(throwable);
    }
    logLine(std::string("JNI exception at ") + where + (detail.empty() ? "" : ": " + detail));
    return true;
}

void initializeSelectionPath() {
    std::vector<wchar_t> localAppData(32768);
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(),
                                                  static_cast<DWORD>(localAppData.size()));
    if (length == 0 || length >= localAppData.size()) {
        logLine("LOCALAPPDATA is unavailable; selection persistence disabled");
        return;
    }

    const std::filesystem::path directory = std::filesystem::path(localAppData.data()) /
        L"ColdEternityTeam" / L"BadlionLocalCosmetics";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        logLine("selection directory creation failed: " + error.message());
        return;
    }
    g_selectionPath = (directory / L"selection-v1.txt").wstring();
    logLine("selection persistence ready");
}

bool loadSelection(SelectionSet& selection) {
    selection.clear();
    if (g_selectionPath.empty()) return false;
    std::ifstream input{std::filesystem::path(g_selectionPath)};
    if (!input) return false;

    std::string line;
    if (!std::getline(input, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "BLC_LOCAL_COSMETICS_V1") {
        logLine("selection file has an unsupported format");
        return false;
    }

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) continue;
        try {
            size_t consumed = 0;
            const int id = std::stoi(line.substr(separator + 1), &consumed);
            if (consumed != line.size() - separator - 1 || id < 0) continue;
            selection.insert({line.substr(0, separator), static_cast<jint>(id)});
        } catch (...) {
            continue;
        }
    }
    return true;
}

bool saveSelection(const SelectionSet& selection) {
    if (g_selectionPath.empty()) return false;
    const std::filesystem::path destination(g_selectionPath);
    const std::filesystem::path temporary = destination.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << "BLC_LOCAL_COSMETICS_V1\n";
        output << "# One active local cosmetic per TYPE=ID line.\n";
        for (const auto& item : selection) output << item.type << '=' << item.id << '\n';
        output.flush();
        if (!output.good()) return false;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

struct CosmeticAccess {
    jclass listClass = nullptr;
    jclass cosmeticClass = nullptr;
    jclass typeClass = nullptr;
    jmethodID listSize = nullptr;
    jmethodID listGet = nullptr;
    jmethodID typeName = nullptr;
    jmethodID isActive = nullptr;
    jmethodID setActive = nullptr;
    jfieldID cosmeticId = nullptr;
    jfieldID cosmeticType = nullptr;
};

bool initializeCosmeticAccess(JNIEnv* env, CosmeticAccess& access) {
    access.listClass = env->FindClass("java/util/List");
    access.cosmeticClass = env->FindClass("net/badlion/a/aCV");
    access.typeClass = env->FindClass("net/badlion/a/aNI");
    if (!access.listClass || !access.cosmeticClass || !access.typeClass) {
        clearException(env, "selection persistence classes");
        return false;
    }
    access.listSize = env->GetMethodID(access.listClass, "size", "()I");
    access.listGet = env->GetMethodID(access.listClass, "get", "(I)Ljava/lang/Object;");
    access.typeName = env->GetMethodID(access.typeClass, "name", "()Ljava/lang/String;");
    access.isActive = env->GetMethodID(access.cosmeticClass, "isActive", "()Z");
    access.setActive = env->GetMethodID(access.cosmeticClass, "setActive", "(Z)V");
    access.cosmeticId = env->GetFieldID(access.cosmeticClass, "cosmeticId", "I");
    access.cosmeticType = env->GetFieldID(access.cosmeticClass, "cosmeticType", "Lnet/badlion/a/aNI;");
    if (clearException(env, "selection persistence members") || !access.listSize || !access.listGet ||
        !access.typeName || !access.isActive || !access.setActive || !access.cosmeticId || !access.cosmeticType) {
        return false;
    }
    return true;
}

void releaseCosmeticAccess(JNIEnv* env, CosmeticAccess& access) {
    if (access.listClass) env->DeleteLocalRef(access.listClass);
    if (access.cosmeticClass) env->DeleteLocalRef(access.cosmeticClass);
    if (access.typeClass) env->DeleteLocalRef(access.typeClass);
}

bool readCosmeticKey(JNIEnv* env, const CosmeticAccess& access, jobject cosmetic, SelectionKey& key) {
    jobject type = env->GetObjectField(cosmetic, access.cosmeticType);
    jstring name = type ? static_cast<jstring>(env->CallObjectMethod(type, access.typeName)) : nullptr;
    if (clearException(env, "read cosmetic selection key") || !type || !name) {
        if (name) env->DeleteLocalRef(name);
        if (type) env->DeleteLocalRef(type);
        return false;
    }
    key.type = jstringToUtf8(env, name);
    key.id = env->GetIntField(cosmetic, access.cosmeticId);
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(type);
    return !clearException(env, "read cosmetic id") && !key.type.empty() && key.id >= 0;
}

bool captureActiveSelection(JNIEnv* env, jobject catalog, SelectionSet& selection) {
    selection.clear();
    if (!catalog) return false;
    CosmeticAccess access;
    if (!initializeCosmeticAccess(env, access)) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    const jint count = env->CallIntMethod(catalog, access.listSize);
    if (clearException(env, "selection catalog size")) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    for (jint i = 0; i < count; ++i) {
        jobject cosmetic = env->CallObjectMethod(catalog, access.listGet, i);
        if (clearException(env, "selection catalog get")) {
            if (cosmetic) env->DeleteLocalRef(cosmetic);
            releaseCosmeticAccess(env, access);
            return false;
        }
        if (cosmetic && env->IsInstanceOf(cosmetic, access.cosmeticClass) &&
            env->CallBooleanMethod(cosmetic, access.isActive) == JNI_TRUE) {
            SelectionKey key;
            if (!clearException(env, "cosmetic isActive") && readCosmeticKey(env, access, cosmetic, key)) {
                selection.insert(std::move(key));
            }
        } else {
            clearException(env, "cosmetic isActive");
        }
        if (cosmetic) env->DeleteLocalRef(cosmetic);
    }
    releaseCosmeticAccess(env, access);
    return true;
}

bool applySavedSelection(JNIEnv* env, jobject catalog, const SelectionSet& selection) {
    if (!catalog) return false;
    if (selection.empty()) {
        logLine("SELECTION_RESTORE requested=0 matched=0");
        return true;
    }
    CosmeticAccess access;
    if (!initializeCosmeticAccess(env, access)) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    const jint count = env->CallIntMethod(catalog, access.listSize);
    if (clearException(env, "restore catalog size")) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    size_t restored = 0;
    for (jint i = 0; i < count; ++i) {
        jobject cosmetic = env->CallObjectMethod(catalog, access.listGet, i);
        if (clearException(env, "restore catalog get")) {
            if (cosmetic) env->DeleteLocalRef(cosmetic);
            releaseCosmeticAccess(env, access);
            return false;
        }
        if (cosmetic && env->IsInstanceOf(cosmetic, access.cosmeticClass)) {
            SelectionKey key;
            if (readCosmeticKey(env, access, cosmetic, key) && selection.find(key) != selection.end()) {
                // setActive(false) also removes catalog entries from some client views.
                // A fresh process already supplies the correct default for every unsaved item.
                env->CallVoidMethod(cosmetic, access.setActive, JNI_TRUE);
                if (!clearException(env, "cosmetic setActive")) ++restored;
            }
        }
        if (cosmetic) env->DeleteLocalRef(cosmetic);
    }
    releaseCosmeticAccess(env, access);
    logLine("SELECTION_RESTORE requested=" + std::to_string(selection.size()) +
            " matched=" + std::to_string(restored));
    return true;
}

bool activateAllCatalog(JNIEnv* env, jobject catalog) {
    if (!catalog) return false;
    CosmeticAccess access;
    if (!initializeCosmeticAccess(env, access)) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    const jint count = env->CallIntMethod(catalog, access.listSize);
    if (clearException(env, "unlock catalog size")) {
        releaseCosmeticAccess(env, access);
        return false;
    }
    size_t activated = 0;
    for (jint i = 0; i < count; ++i) {
        jobject cosmetic = env->CallObjectMethod(catalog, access.listGet, i);
        if (clearException(env, "unlock catalog get")) {
            if (cosmetic) env->DeleteLocalRef(cosmetic);
            releaseCosmeticAccess(env, access);
            return false;
        }
        if (cosmetic && env->IsInstanceOf(cosmetic, access.cosmeticClass)) {
            env->CallVoidMethod(cosmetic, access.setActive, JNI_TRUE);
            if (!clearException(env, "unlock cosmetic setActive")) ++activated;
        }
        if (cosmetic) env->DeleteLocalRef(cosmetic);
    }
    releaseCosmeticAccess(env, access);
    logLine("UNLOCK_ACTIVE_ALL count=" + std::to_string(activated));
    g_catalogReady = activated != 0;
    return true;
}

void prepareSelectionPersistence(JNIEnv* env, jobject catalog) {
    if (!catalog || g_persistentCatalog || g_selectionPrepared) return;
    g_hasSavedSelection = loadSelection(g_selectionToRestore);
    g_selectionPrepared = true;
    if (g_hasSavedSelection && !applySavedSelection(env, catalog, g_selectionToRestore)) {
        logLine("SELECTION_RESTORE failed");
    }
}

void finalizeSelectionPersistence(JNIEnv* env, jobject catalog) {
    if (!catalog || g_persistentCatalog) return;
    if (!g_selectionPrepared) prepareSelectionPersistence(env, catalog);
    if (g_hasSavedSelection && !applySavedSelection(env, catalog, g_selectionToRestore)) {
        logLine("SELECTION_RESTORE final pass failed");
    }
    g_persistentCatalog = env->NewGlobalRef(catalog);
    if (!g_persistentCatalog) {
        clearException(env, "selection catalog global reference");
        return;
    }

    SelectionSet current;
    if (captureActiveSelection(env, catalog, current)) {
        if (!g_hasSavedSelection && saveSelection(current)) {
            logLine("SELECTION_SAVE initial count=" + std::to_string(current.size()));
        }
    }
}

void monitorSelection(JNIEnv* env) {
    if (!g_persistentCatalog) return;
    SelectionSet previous;
    if (!captureActiveSelection(env, g_persistentCatalog, previous)) return;
    if (!saveSelection(previous)) {
        logLine("SELECTION_SAVE monitor baseline failed");
    }
    logLine("SELECTION_MONITOR started count=" + std::to_string(previous.size()));
    while (true) {
        Sleep(750);
        SelectionSet current;
        if (!captureActiveSelection(env, g_persistentCatalog, current)) continue;
        if (current == previous) continue;
        if (saveSelection(current)) {
            logLine("SELECTION_SAVE changed count=" + std::to_string(current.size()));
            previous = std::move(current);
        } else {
            logLine("SELECTION_SAVE failed");
        }
    }
}

void reflectClass(JNIEnv* env, const std::string& binaryName) {
    jclass klass = env->FindClass(binaryName.c_str());
    if (!klass) { clearException(env, ("FindClass " + binaryName).c_str()); return; }
    logLine("CLASS " + binaryName);
    jclass classClass = env->FindClass("java/lang/Class");
    jclass methodClass = env->FindClass("java/lang/reflect/Method");
    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    jclass ctorClass = env->FindClass("java/lang/reflect/Constructor");
    if (!classClass || !methodClass || !fieldClass || !ctorClass) { clearException(env, "reflection classes"); return; }
    jmethodID getMethods = env->GetMethodID(classClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jmethodID getFields = env->GetMethodID(classClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID methodToString = env->GetMethodID(methodClass, "toString", "()Ljava/lang/String;");
    jmethodID fieldToString = env->GetMethodID(fieldClass, "toString", "()Ljava/lang/String;");
    jmethodID getConstructors = env->GetMethodID(classClass, "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;");
    jmethodID ctorToString = env->GetMethodID(ctorClass, "toString", "()Ljava/lang/String;");
    jmethodID getSuperclass = env->GetMethodID(classClass, "getSuperclass", "()Ljava/lang/Class;");
    if (!getMethods || !getFields || !methodToString || !fieldToString || !getConstructors || !ctorToString || !getSuperclass) { clearException(env, "reflection methods"); return; }
    jstring classText = static_cast<jstring>(env->CallObjectMethod(klass, env->GetMethodID(classClass, "getName", "()Ljava/lang/String;")));
    if (!clearException(env, "Class.getName") && classText) {
        logLine("  NAME " + jstringToUtf8(env, classText));
        env->DeleteLocalRef(classText);
    }
    jobject superclass = env->CallObjectMethod(klass, getSuperclass);
    if (!clearException(env, "Class.getSuperclass") && superclass) {
        jstring superName = static_cast<jstring>(env->CallObjectMethod(superclass, env->GetMethodID(classClass, "getName", "()Ljava/lang/String;")));
        if (!clearException(env, "superclass.getName") && superName) {
            logLine("  SUPER " + jstringToUtf8(env, superName));
            env->DeleteLocalRef(superName);
        }
        env->DeleteLocalRef(superclass);
    }
    jobjectArray ctors = static_cast<jobjectArray>(env->CallObjectMethod(klass, getConstructors));
    if (!clearException(env, "getDeclaredConstructors") && ctors) {
        const jsize count = env->GetArrayLength(ctors);
        for (jsize i = 0; i < count; ++i) {
            jobject ctor = env->GetObjectArrayElement(ctors, i);
            jstring text = static_cast<jstring>(env->CallObjectMethod(ctor, ctorToString));
            if (!clearException(env, "Constructor.toString")) logLine("  CTOR " + jstringToUtf8(env, text));
            env->DeleteLocalRef(text); env->DeleteLocalRef(ctor);
        }
        env->DeleteLocalRef(ctors);
    }
    jobjectArray methods = static_cast<jobjectArray>(env->CallObjectMethod(klass, getMethods));
    if (!clearException(env, "getDeclaredMethods") && methods) {
        const jsize count = env->GetArrayLength(methods);
        for (jsize i = 0; i < count; ++i) {
            jobject method = env->GetObjectArrayElement(methods, i);
            jstring text = static_cast<jstring>(env->CallObjectMethod(method, methodToString));
            if (!clearException(env, "Method.toString")) logLine("  METHOD " + jstringToUtf8(env, text));
            env->DeleteLocalRef(text); env->DeleteLocalRef(method);
        }
    }
    jobjectArray fields = static_cast<jobjectArray>(env->CallObjectMethod(klass, getFields));
    if (!clearException(env, "getDeclaredFields") && fields) {
        const jsize count = env->GetArrayLength(fields);
        for (jsize i = 0; i < count; ++i) {
            jobject field = env->GetObjectArrayElement(fields, i);
            jstring text = static_cast<jstring>(env->CallObjectMethod(field, fieldToString));
            if (!clearException(env, "Field.toString")) logLine("  FIELD " + jstringToUtf8(env, text));
            env->DeleteLocalRef(text); env->DeleteLocalRef(field);
        }
    }
    if (methods) env->DeleteLocalRef(methods);
    if (fields) env->DeleteLocalRef(fields);
    env->DeleteLocalRef(klass); env->DeleteLocalRef(classClass);
    env->DeleteLocalRef(methodClass); env->DeleteLocalRef(fieldClass); env->DeleteLocalRef(ctorClass);
}

void probeJvmti(JavaVM* vm) {
    void* out = nullptr;
    const jint result = vm->GetEnv(&out, JVMTI_VERSION_1_2);
    logLine("JVMTI GetEnv result=" + std::to_string(result) + " ptr=" + std::to_string(reinterpret_cast<uintptr_t>(out)));
}

struct HeapProbe {
    jlong marker = 0x424C43434F534D31LL;
};

jvmtiIterationControl JNICALL tagHeapObject(jlong, jlong, jlong* tagPtr, void* userData) {
    auto* probe = static_cast<HeapProbe*>(userData);
    if (tagPtr) *tagPtr = probe->marker;
    return static_cast<jvmtiIterationControl>(JVMTI_VISIT_OBJECTS);
}

std::string objectClassName(JNIEnv* env, jobject object) {
    if (!object) return "null";
    jclass objectClass = env->GetObjectClass(object);
    if (!objectClass) { clearException(env, "GetObjectClass"); return "<unknown>"; }
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID getName = classClass ? env->GetMethodID(classClass, "getName", "()Ljava/lang/String;") : nullptr;
    jstring name = (getName && classClass) ? static_cast<jstring>(env->CallObjectMethod(objectClass, getName)) : nullptr;
    std::string out = (!clearException(env, "object class name") && name) ? jstringToUtf8(env, name) : "<unknown>";
    if (name) env->DeleteLocalRef(name);
    if (classClass) env->DeleteLocalRef(classClass);
    env->DeleteLocalRef(objectClass);
    return out;
}

std::string objectToString(JNIEnv* env, jobject object) {
    if (!object) return "null";
    jclass objectClass = env->GetObjectClass(object);
    jmethodID toString = objectClass ? env->GetMethodID(objectClass, "toString", "()Ljava/lang/String;") : nullptr;
    jstring text = (toString && objectClass) ? static_cast<jstring>(env->CallObjectMethod(object, toString)) : nullptr;
    std::string out = (!clearException(env, "Object.toString") && text) ? jstringToUtf8(env, text) : "<toString failed>";
    if (text) env->DeleteLocalRef(text);
    if (objectClass) env->DeleteLocalRef(objectClass);
    return out;
}

void inspectCosmeticsInstance(JNIEnv* env, jobject instance) {
    logLine("COSMETICS_INSTANCE class=" + objectClassName(env, instance));
    jclass klass = env->GetObjectClass(instance);
    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    jclass classClass = env->FindClass("java/lang/Class");
    if (!klass || !fieldClass || !classClass) { clearException(env, "instance reflection classes"); return; }
    jmethodID getFields = env->GetMethodID(classClass, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID fieldName = env->GetMethodID(fieldClass, "getName", "()Ljava/lang/String;");
    jmethodID trySetAccessible = env->GetMethodID(fieldClass, "trySetAccessible", "()Z");
    jmethodID getValue = env->GetMethodID(fieldClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    if (!getFields || !fieldName || !trySetAccessible || !getValue) { clearException(env, "instance reflection methods"); return; }
    jobjectArray fields = static_cast<jobjectArray>(env->CallObjectMethod(klass, getFields));
    if (clearException(env, "instance getDeclaredFields") || !fields) return;
    const jsize count = env->GetArrayLength(fields);
    for (jsize i = 0; i < count; ++i) {
        jobject field = env->GetObjectArrayElement(fields, i);
        const jboolean accessible = env->CallBooleanMethod(field, trySetAccessible);
        if (clearException(env, "Field.trySetAccessible") || accessible != JNI_TRUE) {
            logLine("  INSTANCE_FIELD inaccessible");
            env->DeleteLocalRef(field); continue;
        }
        jstring jname = static_cast<jstring>(env->CallObjectMethod(field, fieldName));
        const std::string name = (!clearException(env, "Field.getName") && jname) ? jstringToUtf8(env, jname) : "<field>";
        jobject value = env->CallObjectMethod(field, getValue, instance);
        if (clearException(env, "Field.get")) {
            logLine("  INSTANCE_FIELD " + name + "=<get failed>");
        } else if (!value) {
            logLine("  INSTANCE_FIELD " + name + "=null");
        } else {
            std::string summary = objectClassName(env, value) + " " + objectToString(env, value);
            if (summary.size() > 500) summary.resize(500);
            logLine("  INSTANCE_FIELD " + name + "=" + summary);
            jclass listClass = env->FindClass("java/util/List");
            jclass mapClass = env->FindClass("java/util/Map");
            if (listClass && env->IsInstanceOf(value, listClass)) {
                jmethodID size = env->GetMethodID(listClass, "size", "()I");
                jmethodID get = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
                jint n = (size ? env->CallIntMethod(value, size) : 0);
                if (!clearException(env, "List.size")) {
                    logLine("    LIST_SIZE " + name + "=" + std::to_string(n));
                    const jint limit = (n < 12 ? n : 12);
                    for (jint k = 0; k < limit; ++k) {
                        jobject item = get ? env->CallObjectMethod(value, get, k) : nullptr;
                        if (!clearException(env, "List.get")) logLine("    LIST_ITEM " + name + "[" + std::to_string(k) + "]=" + objectClassName(env, item) + " " + objectToString(env, item));
                        if (item) env->DeleteLocalRef(item);
                    }
                }
            } else if (mapClass && env->IsInstanceOf(value, mapClass)) {
                jmethodID size = env->GetMethodID(mapClass, "size", "()I");
                jint n = size ? env->CallIntMethod(value, size) : 0;
                if (!clearException(env, "Map.size")) logLine("    MAP_SIZE " + name + "=" + std::to_string(n));
                jmethodID entrySet = env->GetMethodID(mapClass, "entrySet", "()Ljava/util/Set;");
                jobject entries = entrySet ? env->CallObjectMethod(value, entrySet) : nullptr;
                if (!clearException(env, "Map.entrySet") && entries) {
                    jclass iterable = env->FindClass("java/lang/Iterable");
                    jmethodID iterator = iterable ? env->GetMethodID(iterable, "iterator", "()Ljava/util/Iterator;") : nullptr;
                    jobject it = iterator ? env->CallObjectMethod(entries, iterator) : nullptr;
                    jclass iteratorClass = env->FindClass("java/util/Iterator");
                    jmethodID hasNext = iteratorClass ? env->GetMethodID(iteratorClass, "hasNext", "()Z") : nullptr;
                    jmethodID next = iteratorClass ? env->GetMethodID(iteratorClass, "next", "()Ljava/lang/Object;") : nullptr;
                    for (int k = 0; it && hasNext && next && k < 12 && env->CallBooleanMethod(it, hasNext); ++k) {
                        jobject entry = env->CallObjectMethod(it, next);
                        jclass entryClass = env->FindClass("java/util/Map$Entry");
                        jmethodID getKey = entryClass ? env->GetMethodID(entryClass, "getKey", "()Ljava/lang/Object;") : nullptr;
                        jmethodID getVal = entryClass ? env->GetMethodID(entryClass, "getValue", "()Ljava/lang/Object;") : nullptr;
                        jobject key = getKey ? env->CallObjectMethod(entry, getKey) : nullptr;
                        jobject val = getVal ? env->CallObjectMethod(entry, getVal) : nullptr;
                        if (!clearException(env, "Map entry") && entry) logLine("    MAP_ITEM " + name + "=" + objectToString(env, key) + " -> " + objectClassName(env, val) + " " + objectToString(env, val));
                        if (key) env->DeleteLocalRef(key); if (val) env->DeleteLocalRef(val); if (entryClass) env->DeleteLocalRef(entryClass); if (entry) env->DeleteLocalRef(entry);
                    }
                    if (it) env->DeleteLocalRef(it); if (iterable) env->DeleteLocalRef(iterable); env->DeleteLocalRef(entries);
                }
            }
            if (listClass) env->DeleteLocalRef(listClass); if (mapClass) env->DeleteLocalRef(mapClass);
            env->DeleteLocalRef(value);
        }
        if (jname) env->DeleteLocalRef(jname); env->DeleteLocalRef(field);
    }
    env->DeleteLocalRef(fields); env->DeleteLocalRef(klass); env->DeleteLocalRef(fieldClass); env->DeleteLocalRef(classClass);
}

void dumpList(JNIEnv* env, const std::string& label, jobject listObject) {
    if (!listObject) { logLine("PUBLIC_LIST " + label + "=null"); return; }
    jclass listClass = env->FindClass("java/util/List");
    if (!listClass) { clearException(env, "dump list class"); return; }
    jmethodID size = env->GetMethodID(listClass, "size", "()I");
    jmethodID get = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
    jint n = size ? env->CallIntMethod(listObject, size) : 0;
    if (clearException(env, "PUBLIC_LIST size")) { env->DeleteLocalRef(listClass); return; }
    logLine("PUBLIC_LIST " + label + " size=" + std::to_string(n));
    const jint limit = n < 20 ? n : 20;
    for (jint i = 0; i < limit; ++i) {
        jobject item = get ? env->CallObjectMethod(listObject, get, i) : nullptr;
        if (!clearException(env, "PUBLIC_LIST get")) logLine("PUBLIC_LIST_ITEM " + label + "[" + std::to_string(i) + "]=" + objectClassName(env, item) + " " + objectToString(env, item));
        if (item) env->DeleteLocalRef(item);
    }
    env->DeleteLocalRef(listClass);
}

void inspectUserCosmetics(JNIEnv* env, jobject user) {
    if (!user) return;
    jclass klass = env->GetObjectClass(user);
    if (!klass) { clearException(env, "user cosmetics class"); return; }
    jmethodID all = env->GetMethodID(klass, "buF", "()Ljava/util/List;");
    if (all) {
        jobject list = env->CallObjectMethod(user, all);
        if (!clearException(env, "aDp.buF")) { dumpList(env, "aDp.buF", list); if (list) env->DeleteLocalRef(list); }
    }
    jclass enumClass = env->FindClass("net/badlion/a/aNI");
    jmethodID byType = env->GetMethodID(klass, "f", "(Lnet/badlion/a/aNI;)Ljava/util/List;");
    jmethodID values = enumClass ? env->GetStaticMethodID(enumClass, "values", "()[Lnet/badlion/a/aNI;") : nullptr;
    jobjectArray constants = (values && enumClass) ? static_cast<jobjectArray>(env->CallStaticObjectMethod(enumClass, values)) : nullptr;
    if (!clearException(env, "aNI.values for aDp") && constants && byType) {
        const jsize count = env->GetArrayLength(constants);
        for (jsize i = 0; i < count; ++i) {
            jobject type = env->GetObjectArrayElement(constants, i);
            jobject list = env->CallObjectMethod(user, byType, type);
            if (!clearException(env, "aDp.f")) {
                jclass ec = env->GetObjectClass(type); jmethodID name = ec ? env->GetMethodID(ec, "name", "()Ljava/lang/String;") : nullptr;
                jstring jn = (name && ec) ? static_cast<jstring>(env->CallObjectMethod(type, name)) : nullptr;
                const std::string tn = (!clearException(env, "aDp type name") && jn) ? jstringToUtf8(env, jn) : std::to_string(i);
                dumpList(env, "aDp.f(" + tn + ")", list);
                if (jn) env->DeleteLocalRef(jn); if (ec) env->DeleteLocalRef(ec); if (list) env->DeleteLocalRef(list);
            }
            if (type) env->DeleteLocalRef(type);
        }
        env->DeleteLocalRef(constants);
    }
    if (enumClass) env->DeleteLocalRef(enumClass); env->DeleteLocalRef(klass);
}

void inspectCosmeticsPublicApi(JNIEnv* env, jobject instance);

// The manager setter updates a separate path in this build. Patch the response
// object that the cosmetics screen actually queries, while retaining the
// catalog's original aCV instances and their resource metadata.
void installOwnedCatalog(JNIEnv* env, jobject responseObject, jobject catalogList, jobject ownedObject) {
    if (!responseObject || !catalogList) return;
    jclass responseClass = env->GetObjectClass(responseObject);
    jclass listClass = env->FindClass("java/util/List");
    jclass mapClass = env->FindClass("java/util/Map");
    if (!responseClass || !listClass) {
        clearException(env, "install response/list class");
        if (mapClass) env->DeleteLocalRef(mapClass);
        if (listClass) env->DeleteLocalRef(listClass);
        if (responseClass) env->DeleteLocalRef(responseClass);
        return;
    }

    jfieldID cosmeticsField = env->GetFieldID(responseClass, "cosmetics", "Ljava/util/List;");
    jfieldID userField = env->GetFieldID(responseClass, "userCosmetics", "Lnet/badlion/a/aDp;");
    jfieldID cacheField = env->GetFieldID(responseClass, "ownedCosmeticCache", "Ljava/util/Map;");
    jobject currentList = cosmeticsField ? env->GetObjectField(responseObject, cosmeticsField) : nullptr;
    bool installedInPlace = false;
    if (currentList) {
        jmethodID clear = env->GetMethodID(listClass, "clear", "()V");
        jmethodID addAll = env->GetMethodID(listClass, "addAll", "(Ljava/util/Collection;)Z");
        if (clear && addAll) {
            env->CallVoidMethod(currentList, clear);
            if (!clearException(env, "response cosmetics.clear")) {
                env->CallBooleanMethod(currentList, addAll, catalogList);
                installedInPlace = !clearException(env, "response cosmetics.addAll");
            }
        }
    }
    if (!installedInPlace && cosmeticsField) {
        env->SetObjectField(responseObject, cosmeticsField, catalogList);
        installedInPlace = !clearException(env, "response cosmetics field install");
    }
    logLine(std::string("UNLOCK_RESPONSE_LIST ") + (installedInPlace ? "succeeded" : "failed"));

    if (userField && ownedObject) {
        env->SetObjectField(responseObject, userField, ownedObject);
        if (!clearException(env, "response userCosmetics field install")) logLine("UNLOCK_RESPONSE_USER succeeded");
    }
    if (cacheField && mapClass) {
        jobject cache = env->GetObjectField(responseObject, cacheField);
        if (cache) {
            jmethodID clear = env->GetMethodID(mapClass, "clear", "()V");
            if (clear) {
                env->CallVoidMethod(cache, clear);
                if (!clearException(env, "response ownedCosmeticCache.clear")) logLine("UNLOCK_RESPONSE_CACHE cleared");
            }
            env->DeleteLocalRef(cache);
        }
    }
    if (currentList) env->DeleteLocalRef(currentList);
    if (mapClass) env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(listClass);
    env->DeleteLocalRef(responseClass);
}

void inspectCosmeticsManager(JNIEnv* env, jobject manager) {
    logLine("COSMETICS_MANAGER_INSTANCE class=" + objectClassName(env, manager));
    jclass klass = env->GetObjectClass(manager);
    if (!klass) { clearException(env, "manager class"); return; }
    jmethodID response = env->GetMethodID(klass, "bsd", "()Lnet/badlion/clientcommon/type/cosmetics/response/a;");
    if (response) {
        jobject value = env->CallObjectMethod(manager, response);
        if (!clearException(env, "aCY.bsd") && value) {
            logLine("MANAGER_RESPONSE class=" + objectClassName(env, value));
            inspectCosmeticsPublicApi(env, value);
            env->DeleteLocalRef(value);
        }
    }
    jmethodID metadata = env->GetMethodID(klass, "bsj", "()Lnet/badlion/clientcommon/type/cosmetics/response/b;");
    if (metadata) {
        jobject value = env->CallObjectMethod(manager, metadata);
        if (!clearException(env, "aCY.bsj") && value) {
            logLine("MANAGER_METADATA class=" + objectClassName(env, value) + " text=" + objectToString(env, value));
            jclass bklass = env->GetObjectClass(value);
            jmethodID all = bklass ? env->GetMethodID(bklass, "buL", "()Ljava/util/List;") : nullptr;
            jmethodID latest = bklass ? env->GetMethodID(bklass, "bwR", "()Ljava/util/List;") : nullptr;
            jobject list = all ? env->CallObjectMethod(value, all) : nullptr;
            if (!clearException(env, "response.b.buL")) {
                dumpList(env, "response.b.buL", list);
                if (list) {
                    prepareSelectionPersistence(env, list);
                    // The owning wrapper is built from active catalog objects. Mark
                    // the full catalog active before constructing it so every item
                    // is present in the local "Your Cosmetics" list.
                    activateAllCatalog(env, list);
                    jclass userClass = env->FindClass("net/badlion/a/aDp");
                    jmethodID userCtor = userClass ? env->GetMethodID(userClass, "<init>", "(Ljava/util/List;)V") : nullptr;
                    if (userCtor) {
                        jobject allOwned = env->NewObject(userClass, userCtor, list);
                        if (!clearException(env, "new aDp(all cosmetics)") && allOwned) {
                            logLine("UNLOCK_BUILD aDp from catalog");
                            jmethodID responseMethod = env->GetMethodID(klass, "bsd", "()Lnet/badlion/clientcommon/type/cosmetics/response/a;");
                            jobject responseObject = responseMethod ? env->CallObjectMethod(manager, responseMethod) : nullptr;
                            if (!clearException(env, "aCY.bsd after install") && responseObject) {
                                installOwnedCatalog(env, responseObject, list, allOwned);
                                if (g_catalogReady) g_unlockInstalled = true;
                                logLine("UNLOCK_DIRECT response state");
                                inspectCosmeticsPublicApi(env, responseObject);
                                env->DeleteLocalRef(responseObject);
                            }
                            env->DeleteLocalRef(allOwned);
                        }
                    } else {
                        clearException(env, "aDp constructor or manager setter");
                    }
                    if (userClass) env->DeleteLocalRef(userClass);
                    finalizeSelectionPersistence(env, list);
                    env->DeleteLocalRef(list);
                }
            }
            list = latest ? env->CallObjectMethod(value, latest) : nullptr;
            if (!clearException(env, "response.b.bwR")) { dumpList(env, "response.b.bwR", list); if (list) env->DeleteLocalRef(list); }
            if (bklass) env->DeleteLocalRef(bklass); env->DeleteLocalRef(value);
        }
    }
    jmethodID map = env->GetMethodID(klass, "bsc", "()Ljava/util/Map;");
    if (map) {
        jobject value = env->CallObjectMethod(manager, map);
        if (!clearException(env, "aCY.bsc") && value) { logLine("MANAGER_MAP class=" + objectClassName(env, value) + " text=" + objectToString(env, value)); env->DeleteLocalRef(value); }
    }
    jmethodID listAll = env->GetMethodID(klass, "bse", "()Ljava/util/List;");
    if (listAll) {
        jobject value = env->CallObjectMethod(manager, listAll);
        if (!clearException(env, "aCY.bse")) { dumpList(env, "manager.bse", value); if (value) env->DeleteLocalRef(value); }
    }
    jmethodID byType = env->GetMethodID(klass, "a", "(Lnet/badlion/a/aNI;)Ljava/util/List;");
    jclass enumClass = env->FindClass("net/badlion/a/aNI");
    jmethodID values = enumClass ? env->GetStaticMethodID(enumClass, "values", "()[Lnet/badlion/a/aNI;") : nullptr;
    jobjectArray constants = (values && enumClass) ? static_cast<jobjectArray>(env->CallStaticObjectMethod(enumClass, values)) : nullptr;
    if (!clearException(env, "aNI.values for aCY") && constants && byType) {
        const jsize count = env->GetArrayLength(constants);
        for (jsize i = 0; i < count; ++i) {
            jobject type = env->GetObjectArrayElement(constants, i);
            jobject list = env->CallObjectMethod(manager, byType, type);
            if (!clearException(env, "aCY.a(type)")) {
                jclass ec = env->GetObjectClass(type); jmethodID name = ec ? env->GetMethodID(ec, "name", "()Ljava/lang/String;") : nullptr;
                jstring jn = (name && ec) ? static_cast<jstring>(env->CallObjectMethod(type, name)) : nullptr;
                const std::string tn = (!clearException(env, "aCY type name") && jn) ? jstringToUtf8(env, jn) : std::to_string(i);
                dumpList(env, "aCY.a(" + tn + ")", list);
                if (jn) env->DeleteLocalRef(jn); if (ec) env->DeleteLocalRef(ec); if (list) env->DeleteLocalRef(list);
            }
            if (type) env->DeleteLocalRef(type);
        }
        env->DeleteLocalRef(constants);
    }
    if (enumClass) env->DeleteLocalRef(enumClass); env->DeleteLocalRef(klass);
}

void inspectCosmeticsPublicApi(JNIEnv* env, jobject instance) {
    jclass klass = env->GetObjectClass(instance);
    if (!klass) { clearException(env, "public api object class"); return; }
    jmethodID all = env->GetMethodID(klass, "buL", "()Ljava/util/List;");
    jmethodID user = env->GetMethodID(klass, "bwQ", "()Lnet/badlion/a/aDp;");
    jmethodID byType = env->GetMethodID(klass, "i", "(Lnet/badlion/a/aNI;)Ljava/util/List;");
    jmethodID privateByType = env->GetStaticMethodID(klass, "j", "(Lnet/badlion/a/aNI;)Ljava/util/List;");
    if (!privateByType) clearException(env, "response.a private j method");
    if (all) {
        jobject list = env->CallObjectMethod(instance, all);
        if (!clearException(env, "response.a.buL")) { dumpList(env, "buL", list); if (list) env->DeleteLocalRef(list); }
    }
    if (user) {
        jobject value = env->CallObjectMethod(instance, user);
        if (!clearException(env, "response.a.bwQ")) {
            logLine("PUBLIC_USER_COSMETICS class=" + objectClassName(env, value) + " text=" + objectToString(env, value));
            inspectUserCosmetics(env, value);
            if (value) env->DeleteLocalRef(value);
        }
    }
    jclass enumClass = env->FindClass("net/badlion/a/aNI");
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID enumConstants = classClass ? env->GetMethodID(classClass, "getEnumConstants", "()[Ljava/lang/Object;") : nullptr;
    jobjectArray constants = (enumConstants && enumClass) ? static_cast<jobjectArray>(env->CallObjectMethod(enumClass, enumConstants)) : nullptr;
    if (!clearException(env, "aNI.getEnumConstants") && constants && byType) {
        const jsize count = env->GetArrayLength(constants);
        logLine("A_NI_ENUM_COUNT=" + std::to_string(count));
        jmethodID enumName = enumClass ? env->GetMethodID(enumClass, "name", "()Ljava/lang/String;") : nullptr;
        for (jsize i = 0; i < count; ++i) {
            jobject constant = env->GetObjectArrayElement(constants, i);
            jstring name = (enumName && constant) ? static_cast<jstring>(env->CallObjectMethod(constant, enumName)) : nullptr;
            const std::string typeName = (!clearException(env, "aNI.name") && name) ? jstringToUtf8(env, name) : std::to_string(i);
            jobject list = env->CallObjectMethod(instance, byType, constant);
            if (!clearException(env, "response.a.i")) { dumpList(env, "i(" + typeName + ")", list); if (list) env->DeleteLocalRef(list); }
            if (privateByType) {
                jobject allList = env->CallStaticObjectMethod(klass, privateByType, constant);
                if (!clearException(env, "response.a.j")) { dumpList(env, "j(" + typeName + ")", allList); if (allList) env->DeleteLocalRef(allList); }
            }
            if (name) env->DeleteLocalRef(name); if (constant) env->DeleteLocalRef(constant);
        }
        env->DeleteLocalRef(constants);
    }
    if (enumClass) env->DeleteLocalRef(enumClass); if (classClass) env->DeleteLocalRef(classClass); env->DeleteLocalRef(klass);
}

void enumerateCosmeticsInstances(JNIEnv* env, jvmtiEnv* jvmti, jclass klass) {
    if (!jvmti || !klass) return;
    jvmtiCapabilities caps{};
    if (jvmti->GetCapabilities(&caps) != JVMTI_ERROR_NONE || !caps.can_tag_objects) {
        jvmtiCapabilities requested{}; requested.can_tag_objects = 1;
        const jvmtiError add = jvmti->AddCapabilities(&requested);
        logLine("JVMTI AddCapabilities(tag_objects)=" + std::to_string(add));
    }
    HeapProbe probe;
    const jvmtiError iter = jvmti->IterateOverInstancesOfClass(klass, JVMTI_HEAP_OBJECT_EITHER, tagHeapObject, &probe);
    logLine("JVMTI IterateOverInstancesOfClass=" + std::to_string(iter));
    if (iter != JVMTI_ERROR_NONE) return;
    jint found = 0; jobject* objects = nullptr; jlong* tags = nullptr;
    const jvmtiError get = jvmti->GetObjectsWithTags(1, &probe.marker, &found, &objects, &tags);
    logLine("JVMTI GetObjectsWithTags=" + std::to_string(get) + " count=" + std::to_string(found));
    if (get == JVMTI_ERROR_NONE && objects) {
        for (jint i = 0; i < found; ++i) {
            inspectCosmeticsInstance(env, objects[i]);
            inspectCosmeticsPublicApi(env, objects[i]);
        }
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
        if (tags) jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));
    }
}

void enumerateManagerInstances(JNIEnv* env, jvmtiEnv* jvmti, jclass klass) {
    if (!jvmti || !klass) return;
    HeapProbe probe;
    const jvmtiError iter = jvmti->IterateOverInstancesOfClass(klass, JVMTI_HEAP_OBJECT_EITHER, tagHeapObject, &probe);
    logLine("JVMTI IterateOverInstancesOfClass(aCY)=" + std::to_string(iter));
    if (iter != JVMTI_ERROR_NONE) return;
    jint found = 0; jobject* objects = nullptr; jlong* tags = nullptr;
    const jvmtiError get = jvmti->GetObjectsWithTags(1, &probe.marker, &found, &objects, &tags);
    logLine("JVMTI GetObjectsWithTags(aCY)=" + std::to_string(get) + " count=" + std::to_string(found));
    if (get == JVMTI_ERROR_NONE && objects) {
        for (jint i = 0; i < found; ++i) inspectCosmeticsManager(env, objects[i]);
        jvmti->Deallocate(reinterpret_cast<unsigned char*>(objects));
        if (tags) jvmti->Deallocate(reinterpret_cast<unsigned char*>(tags));
    }
}

void scanLoadedByLoader(JNIEnv* env, jclass knownClass) {
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID getLoader = classClass ? env->GetMethodID(classClass, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = (getLoader && knownClass) ? env->CallObjectMethod(knownClass, getLoader) : nullptr;
    if (clearException(env, "Class.getClassLoader") || !loader) {
        logLine("known class has no usable ClassLoader");
        if (classClass) env->DeleteLocalRef(classClass);
        return;
    }
    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = loaderClass ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    if (!loadClass) {
        clearException(env, "ClassLoader.loadClass");
        env->DeleteLocalRef(loader); if (loaderClass) env->DeleteLocalRef(loaderClass); if (classClass) env->DeleteLocalRef(classClass);
        return;
    }
    wchar_t modulePath[MAX_PATH]{}; GetModuleFileNameW(g_module, modulePath, MAX_PATH);
    std::filesystem::path listPath = std::filesystem::path(modulePath).parent_path() / L"target-classes.txt";
    std::ifstream list(listPath);
    if (!list) { logLine("target-classes.txt not found: " + listPath.string()); return; }
    std::string name; size_t attempted = 0, loaded = 0;
    while (std::getline(list, name)) {
        if (name.empty()) continue;
        ++attempted; jstring jname = env->NewStringUTF(name.c_str());
        jclass target = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, jname));
        env->DeleteLocalRef(jname);
        if (clearException(env, ("ClassLoader.loadClass " + name).c_str()) || !target) continue;
        ++loaded;
        const bool interesting = name.find("cosmetic") != std::string::npos || name.find("emote") != std::string::npos ||
                                 name.find("Cosmetic") != std::string::npos || name.find("Emote") != std::string::npos ||
                                 name.find("request") != std::string::npos || name.find("response") != std::string::npos;
        if (interesting) {
            std::string internal = name;
            std::replace(internal.begin(), internal.end(), '.', '/');
            reflectClass(env, internal);
        }
        env->DeleteLocalRef(target);
    }
    logLine("loader scan attempted=" + std::to_string(attempted) + " loaded=" + std::to_string(loaded));
    env->DeleteLocalRef(loader); env->DeleteLocalRef(loaderClass); env->DeleteLocalRef(classClass);
}

void runAgent() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(g_module, modulePath, MAX_PATH);
    g_logPath = (std::filesystem::path(modulePath).parent_path() / L"blc_unlock_agent.log").wstring();
    logLine("agent loaded");
    initializeSelectionPath();
    HMODULE jvm = nullptr;
    for (int i = 0; i < 100 && !jvm; ++i) { jvm = GetModuleHandleW(L"jvm.dll"); if (!jvm) Sleep(100); }
    if (!jvm) { logLine("jvm.dll not found"); return; }
    using GetCreatedVMs = jint (JNICALL*)(JavaVM**, jsize, jsize*);
    auto getCreatedVMs = reinterpret_cast<GetCreatedVMs>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs"));
    if (!getCreatedVMs) { logLine("JNI_GetCreatedJavaVMs not exported"); return; }
    JavaVM* vms[8]{}; jsize count = 0;
    if (getCreatedVMs(vms, 8, &count) != JNI_OK || count == 0) { logLine("no created JVM"); return; }
    JNIEnv* env = nullptr; jint getEnv = vms[0]->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8); bool attached = false;
    if (getEnv == JNI_EDETACHED) {
        if (vms[0]->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) { logLine("AttachCurrentThread failed"); return; }
        attached = true;
    } else if (getEnv != JNI_OK) { logLine("GetEnv failed: " + std::to_string(getEnv)); return; }
    logLine("JNI attached; VM count=" + std::to_string(count));
    probeJvmti(vms[0]);
    jclass system = env->FindClass("java/lang/System");
    if (system) {
        jmethodID getProperty = env->GetStaticMethodID(system, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key = env->NewStringUTF("java.version");
        jstring version = static_cast<jstring>(env->CallStaticObjectMethod(system, getProperty, key));
        if (!clearException(env, "System.getProperty")) logLine("java.version=" + jstringToUtf8(env, version));
        env->DeleteLocalRef(key); if (version) env->DeleteLocalRef(version); env->DeleteLocalRef(system);
    } else clearException(env, "FindClass java/lang/System");
    const std::vector<std::string> targets = {
        "net/badlion/clientcommon/type/cosmetics/response/SelectCosmeticsResponse",
        "net/badlion/clientcommon/type/cosmetics/response/SelectCosmeticResponse",
        "net/badlion/clientcommon/type/cosmetics/request/SelectCosmeticRequest",
        "net/badlion/clientcommon/type/cosmetics/request/CosmeticsMessage",
        "net/badlion/clientcommon/type/cosmetics/Cosmetic",
        "net/badlion/clientcommon/type/cosmetics/CosmeticManager",
        "net/badlion/clientcommon/emotes/Emote",
        "net/badlion/clientcommon/hooks/EncryptionHooks",
        "net/badlion/clientcommon/hooks/CosmeticHooks",
        "net/badlion/clientcommon/CosmeticsManager",
        "net/badlion/clientcommon/ClientCommon",
        "net/badlion/a/aNI",
        "net/badlion/a/aCV",
        "net/badlion/a/aDp",
        "net/badlion/a/aCY",
        "net/badlion/a/db"
    };
    for (const auto& target : targets) reflectClass(env, target);
    jclass known = env->FindClass("net/badlion/clientcommon/type/cosmetics/response/SelectCosmeticsResponse");
    if (known) scanLoadedByLoader(env, known); else clearException(env, "known cosmetic class");
    if (known) env->DeleteLocalRef(known);
    jclass cosmeticsResponse = env->FindClass("net/badlion/clientcommon/type/cosmetics/response/a");
    jvmtiEnv* jvmti = nullptr;
    if (vms[0]->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) == JNI_OK && cosmeticsResponse) {
        enumerateCosmeticsInstances(env, jvmti, cosmeticsResponse);
    } else {
        clearException(env, "cosmetics response class for heap probe");
    }
    if (cosmeticsResponse) env->DeleteLocalRef(cosmeticsResponse);
    jclass managerClass = env->FindClass("net/badlion/a/aCY");
    if (jvmti && managerClass) {
        // Badlion creates the manager before the catalog response arrives. Retry
        // the same manager probe until the 424-item catalog is available.
        for (int attempt = 0; attempt < 60 && !g_unlockInstalled; ++attempt) {
            enumerateManagerInstances(env, jvmti, managerClass);
            if (!g_unlockInstalled) Sleep(1000);
        }
        if (!g_unlockInstalled) logLine("UNLOCK_TIMEOUT catalog response not ready");
    } else clearException(env, "aCY class for heap probe");
    if (managerClass) env->DeleteLocalRef(managerClass);
    logLine("diagnostic reflection complete");
    monitorSelection(env);
    if (attached) vms[0]->DetachCurrentThread();
}

DWORD WINAPI worker(void*) { runAgent(); return 0; }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module; DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
