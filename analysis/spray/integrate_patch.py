from pathlib import Path
p = Path(__file__).resolve().parents[2] / 'agent.cpp'
s = p.read_text(encoding='utf-8')
a = s.index('// The manager setter updates a separate path')
b = s.index('void inspectCosmeticsManager', a)
s = s[:a] + '#include "spray_support.inl"\n\n' + s[b:]
old = '''            if (!clearException(env, "response.b.buL")) {
                dumpList(env, "response.b.buL", list);'''
new = '''            if (!clearException(env, "response.b.buL")) {
                if (list) {
                    jobject merged = buildSprayCatalog(env, manager, list);
                    env->DeleteLocalRef(list);
                    list = merged;
                }
                dumpList(env, "response.b.buL + registered sprays", list);'''
assert old in s
s = s.replace(old, new, 1)
old = '''                                installOwnedCatalog(env, responseObject, list, currentUser);
                                g_catalogReady = true;
                                g_unlockInstalled = true;
                                logLine("UNLOCK_DIRECT response state");
                                inspectCosmeticsPublicApi(env, responseObject);'''
new = '''                                if (installOwnedCatalog(env, responseObject, list, currentUser)) {
                                    g_catalogReady = true;
                                    g_unlockInstalled = true;
                                    logLine("UNLOCK_DIRECT response state");
                                    verifySprayOwnership(env, manager);
                                    inspectCosmeticsPublicApi(env, responseObject);
                                }'''
assert old in s
s = s.replace(old, new, 1)
s = s.replace('                    finalizeSelectionPersistence(env, list);', '                    if (g_unlockInstalled) finalizeSelectionPersistence(env, list);', 1)
p.write_text(s, encoding='utf-8')
