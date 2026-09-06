// Included inside agent.cpp's anonymous namespace. Runtime contract verified
// against 4.4.4-f8775e4: aFM.X(id)/ed.V read response.buL(), not aDp's
// active-only buckets. Never turn every spray active or overwrite spray slots.
struct SprayLocalFrame {
    JNIEnv* env;
    bool valid;
    explicit SprayLocalFrame(JNIEnv* e) : env(e), valid(e->PushLocalFrame(96) == JNI_OK) {}
    ~SprayLocalFrame() { if (valid) env->PopLocalFrame(nullptr); }
    jobject keep(jobject value) { valid = false; return env->PopLocalFrame(value); }
};

// Copy the store list, reuse existing aCV objects, retain owned spray metadata,
// then add ONLY ids already registered in the live spray renderer. No hardcoded
// id range, asset edits, downloads or account/server mutation.
jobject buildSprayCatalog(JNIEnv* env, jobject manager, jobject storeCatalog) {
    SprayLocalFrame frame(env);
    if (!frame.valid || !manager || !storeCatalog) return nullptr;
    jclass lc = env->FindClass("java/util/List");
    jclass ac = env->FindClass("java/util/ArrayList");
    jclass mc = env->FindClass("java/util/Map");
    jclass cc = env->FindClass("java/util/Collection");
    jclass managerClass = env->GetObjectClass(manager);
    jclass cv = env->FindClass("net/badlion/a/aCV");
    jclass ni = env->FindClass("net/badlion/a/aNI");
    jclass fm = env->FindClass("net/badlion/a/aFM");
    jclass fk = env->FindClass("net/badlion/a/aFK");
    jclass rc = env->FindClass("net/badlion/clientcommon/type/cosmetics/response/a");
    if (clearException(env, "spray catalog classes") || !lc || !ac || !mc || !cc || !managerClass || !cv || !ni || !fm || !fk || !rc) return nullptr;
    auto copy = env->GetMethodID(ac, "<init>", "(Ljava/util/Collection;)V");
    auto size = env->GetMethodID(lc, "size", "()I");
    auto get = env->GetMethodID(lc, "get", "(I)Ljava/lang/Object;");
    auto add = env->GetMethodID(lc, "add", "(Ljava/lang/Object;)Z");
    auto values = env->GetMethodID(mc, "values", "()Ljava/util/Collection;");
    auto toArray = env->GetMethodID(cc, "toArray", "()[Ljava/lang/Object;");
    auto getSprays = env->GetMethodID(managerClass, "bsF", "()Lnet/badlion/a/aFM;");
    auto getResponse = env->GetMethodID(managerClass, "bsd", "()Lnet/badlion/clientcommon/type/cosmetics/response/a;");
    auto all = env->GetMethodID(rc, "buL", "()Ljava/util/List;");
    auto registry = env->GetMethodID(fm, "bwK", "()Ljava/util/Map;");
    auto sprayId = env->GetMethodID(fk, "brv", "()I");
    auto sprayName = env->GetMethodID(fk, "bwk", "()Ljava/lang/String;");
    auto type = env->GetFieldID(cv, "cosmeticType", "Lnet/badlion/a/aNI;");
    auto id = env->GetFieldID(cv, "cosmeticId", "I");
    auto name = env->GetFieldID(cv, "name", "Ljava/lang/String;");
    auto ctor = env->GetMethodID(cv, "<init>", "(ILnet/badlion/a/aNI;Lcom/google/gson/JsonObject;)V");
    auto sprayField = env->GetStaticFieldID(ni, "SPRAY", "Lnet/badlion/a/aNI;");
    if (clearException(env, "spray catalog members") || !copy || !size || !get || !add || !values || !toArray || !getSprays || !getResponse || !all || !registry || !sprayId || !sprayName || !type || !id || !name || !ctor || !sprayField) return nullptr;
    jobject spray = env->GetStaticObjectField(ni, sprayField);
    jobject result = env->NewObject(ac, copy, storeCatalog);
    jobject sm = env->CallObjectMethod(manager, getSprays);
    jobject registered = sm ? env->CallObjectMethod(sm, registry) : nullptr;
    jobject collection = registered ? env->CallObjectMethod(registered, values) : nullptr;
    auto entries = collection ? static_cast<jobjectArray>(env->CallObjectMethod(collection, toArray)) : nullptr;
    if (clearException(env, "spray registry snapshot") || !result || !spray || !entries) return nullptr;
    const jsize registeredCount = env->GetArrayLength(entries);
    if (registeredCount == 0) { logLine("SPRAY_CATALOG waiting for registered resources"); return nullptr; }
    std::set<jint> seen;
    auto collect = [&](jobject list, bool append) {
        if (!list) return true;
        const jint count = env->CallIntMethod(list, size);
        if (clearException(env, "spray source size")) return false;
        for (jint i = 0; i < count; ++i) {
            jobject item = env->CallObjectMethod(list, get, i);
            if (clearException(env, "spray source item")) return false;
            if (!item || !env->IsInstanceOf(item, cv)) { if (item) env->DeleteLocalRef(item); continue; }
            jobject kind = env->GetObjectField(item, type);
            if (env->IsSameObject(kind, spray)) {
                const jint key = env->GetIntField(item, id);
                if (seen.insert(key).second && append) env->CallBooleanMethod(result, add, item);
            }
            if (kind) env->DeleteLocalRef(kind); env->DeleteLocalRef(item);
            if (clearException(env, "spray merge existing")) return false;
        }
        return true;
    };
    if (!collect(result, false)) return nullptr;
    const size_t storeSprays = seen.size();
    jobject response = env->CallObjectMethod(manager, getResponse);
    jobject owned = response ? env->CallObjectMethod(response, all) : nullptr;
    if (clearException(env, "spray owned source") || !collect(owned, true)) return nullptr;
    size_t created = 0;
    for (jsize i = 0; i < registeredCount; ++i) {
        jobject resource = env->GetObjectArrayElement(entries, i);
        if (!resource || !env->IsInstanceOf(resource, fk)) return nullptr;
        const jint key = env->CallIntMethod(resource, sprayId);
        if (clearException(env, "registered spray id")) return nullptr;
        if (key >= 0 && seen.insert(key).second) {
            jobject item = env->NewObject(cv, ctor, key, spray, nullptr);
            if (clearException(env, "new local spray descriptor") || !item) return nullptr;
            jobject label = env->CallObjectMethod(resource, sprayName);
            if (clearException(env, "registered spray name")) return nullptr;
            env->SetObjectField(item, name, label);
            env->CallBooleanMethod(result, add, item);
            if (clearException(env, "append local spray")) return nullptr;
            if (label) env->DeleteLocalRef(label); env->DeleteLocalRef(item); ++created;
        }
        env->DeleteLocalRef(resource);
    }
    logLine("SPRAY_CATALOG store=" + std::to_string(storeSprays) + " registered=" + std::to_string(registeredCount) +
            " created=" + std::to_string(created) + " total=" + std::to_string(seen.size()));
    return frame.keep(result);
}

// Stage complete list/cache off to the side. Emptying a cache without setting
// needsUpdate left response.i(SPRAY) empty in the old implementation.
bool installOwnedCatalog(JNIEnv* env, jobject response, jobject catalog, jobject wrapper) {
    SprayLocalFrame frame(env);
    if (!frame.valid || !response || !catalog || !wrapper) return false;
    jclass rc = env->GetObjectClass(response);
    jclass lc = env->FindClass("java/util/List");
    jclass ac = env->FindClass("java/util/ArrayList");
    jclass mc = env->FindClass("java/util/HashMap");
    jclass cv = env->FindClass("net/badlion/a/aCV");
    jclass ab = env->FindClass("java/util/concurrent/atomic/AtomicBoolean");
    if (clearException(env, "owned snapshot classes") || !rc || !lc || !ac || !mc || !cv || !ab) return false;
    auto lf = env->GetFieldID(rc, "cosmetics", "Ljava/util/List;");
    auto uf = env->GetFieldID(rc, "userCosmetics", "Lnet/badlion/a/aDp;");
    auto cf = env->GetFieldID(rc, "ownedCosmeticCache", "Ljava/util/Map;");
    auto nf = env->GetFieldID(rc, "needsUpdate", "Ljava/util/concurrent/atomic/AtomicBoolean;");
    auto tf = env->GetFieldID(cv, "cosmeticType", "Lnet/badlion/a/aNI;");
    auto copy = env->GetMethodID(ac, "<init>", "(Ljava/util/Collection;)V");
    auto empty = env->GetMethodID(ac, "<init>", "()V");
    auto makeMap = env->GetMethodID(mc, "<init>", "()V");
    auto size = env->GetMethodID(lc, "size", "()I");
    auto get = env->GetMethodID(lc, "get", "(I)Ljava/lang/Object;");
    auto add = env->GetMethodID(lc, "add", "(Ljava/lang/Object;)Z");
    auto lookup = env->GetMethodID(mc, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    auto put = env->GetMethodID(mc, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    auto set = env->GetMethodID(ab, "set", "(Z)V");
    if (clearException(env, "owned snapshot members") || !lf || !uf || !cf || !nf || !tf || !copy || !empty || !makeMap || !size || !get || !add || !lookup || !put || !set) return false;
    jobject list = env->NewObject(ac, copy, catalog);
    jobject cache = env->NewObject(mc, makeMap);
    jobject needsUpdate = env->GetObjectField(response, nf);
    if (clearException(env, "owned snapshot allocate") || !list || !cache || !needsUpdate) return false;
    const jint count = env->CallIntMethod(list, size);
    for (jint i = 0; i < count; ++i) {
        jobject item = env->CallObjectMethod(list, get, i);
        if (clearException(env, "owned snapshot item") || !item || !env->IsInstanceOf(item, cv)) return false;
        jobject type = env->GetObjectField(item, tf);
        jobject bucket = env->CallObjectMethod(cache, lookup, type);
        if (clearException(env, "owned snapshot bucket") || !type) return false;
        if (!bucket) {
            bucket = env->NewObject(ac, empty);
            if (clearException(env, "owned snapshot new bucket") || !bucket) return false;
            jobject previous = env->CallObjectMethod(cache, put, type, bucket);
            if (previous) env->DeleteLocalRef(previous);
        }
        env->CallBooleanMethod(bucket, add, item);
        if (clearException(env, "owned snapshot add")) return false;
        env->DeleteLocalRef(bucket); env->DeleteLocalRef(type); env->DeleteLocalRef(item);
    }
    env->SetObjectField(response, lf, list);
    env->SetObjectField(response, uf, wrapper);
    env->SetObjectField(response, cf, cache);
    env->CallVoidMethod(needsUpdate, set, JNI_FALSE);
    const bool ok = !clearException(env, "owned snapshot publish");
    logLine(std::string("UNLOCK_RESPONSE_SNAPSHOT ") + (ok ? "succeeded" : "failed") + " total=" + std::to_string(count));
    return ok;
}

// Narrow authoritative ownership predicate used by ed.V() when saving slots.
void verifySprayOwnership(JNIEnv* env, jobject manager) {
    SprayLocalFrame frame(env); if (!frame.valid) return;
    jclass fm = env->FindClass("net/badlion/a/aFM");
    if (clearException(env, "spray verification class") || !fm) return;
    auto allowed = env->GetStaticMethodID(fm, "X", "(I)Z");
    if (clearException(env, "spray verification method") || !allowed) return;
    for (jint id : {0, 40, 169, 252, -1, 1000000}) {
        const bool ok = env->CallStaticBooleanMethod(fm, allowed, id) == JNI_TRUE;
        if (clearException(env, "spray ownership predicate")) return;
        logLine("SPRAY_OWNERSHIP id=" + std::to_string(id) + " allowed=" + std::to_string(ok));
    }
}
