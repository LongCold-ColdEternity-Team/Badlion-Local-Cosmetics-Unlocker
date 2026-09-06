// Runs the production include against a separate, synthetic JVM. Never inject
// this DLL into Minecraft; fixture classes are NOT recovered client classes.
#include <jni.h>
#include <set>
#include <string>
#include <iostream>
namespace {
void logLine(const std::string& text) { std::cout << text << std::endl; }
bool clearException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return false;
    std::cout << "FIXTURE JNI exception at " << where << std::endl;
    env->ExceptionDescribe(); env->ExceptionClear(); return true;
}
#include "../../spray_support.inl"
}
extern "C" JNIEXPORT jobject JNICALL Java_SprayFixture_merge(
    JNIEnv* e, jclass, jobject manager, jobject store) {
    return buildSprayCatalog(e, manager, store);
}
extern "C" JNIEXPORT jboolean JNICALL Java_SprayFixture_install(
    JNIEnv* e, jclass, jobject response, jobject catalog, jobject wrapper) {
    return installOwnedCatalog(e, response, catalog, wrapper) ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_SprayFixture_verify(
    JNIEnv* e, jclass, jobject manager) { verifySprayOwnership(e, manager); }
