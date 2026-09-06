"""Compile the actual spray_support.inl in a fresh synthetic JNI fixture.

No client launch, injection, profile access, network, or inspection-only classes.
These tests prove local merge/cache contracts, NOT Minecraft rendering.
"""
import argparse
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SOURCES = {
    "com/google/gson/JsonObject.java": """
package com.google.gson;
public class JsonObject { public String marker; }
""",
    "net/badlion/a/aNI.java": """
package net.badlion.a;
public enum aNI { SPRAY, CAPE }
""",
    "net/badlion/a/aCV.java": """
package net.badlion.a;
import com.google.gson.JsonObject;
public class aCV {
 public int cosmeticId; public aNI cosmeticType; public String name;
 public JsonObject cosmeticData; public boolean active;
 public aCV(int id, aNI type, JsonObject data) {
  cosmeticId=id; cosmeticType=type; cosmeticData=data;
 }
}
""",
    "net/badlion/a/aFK.java": """
package net.badlion.a;
public class aFK {
 final int id; final String name;
 public aFK(int id, String name) { this.id=id; this.name=name; }
 public int brv() { return id; } public String bwk() { return name; }
}
""",
    "net/badlion/a/aDp.java": """
package net.badlion.a;
import java.util.*;
public class aDp {
 public final List<aCV> active = new ArrayList<>();
 public aDp(List<aCV> all) { for(aCV c:all) if(c.active) active.add(c); }
}
""",
    "net/badlion/clientcommon/type/cosmetics/response/a.java": """
package net.badlion.clientcommon.type.cosmetics.response;
import java.util.*;
import java.util.concurrent.atomic.AtomicBoolean;
import net.badlion.a.*;
public class a {
 public final List<aCV> cosmetics=new ArrayList<>();
 public final AtomicBoolean needsUpdate=new AtomicBoolean(true);
 public aDp userCosmetics;
 public Map<aNI,List<aCV>> ownedCosmeticCache=new HashMap<>();
 public List<aCV> buL() { return cosmetics; }
 public List<aCV> i(aNI t) { return ownedCosmeticCache.get(t); }
}
""",
    "net/badlion/a/aFM.java": """
package net.badlion.a;
import java.util.*;
import net.badlion.clientcommon.type.cosmetics.response.a;
public class aFM {
 public final Map<Integer,aFK> entries=new LinkedHashMap<>();
 public static a response;
 public Map<Integer,aFK> bwK() { return entries; }
 public static boolean X(int id) {
  if(id==40) return true;
  for(aCV c:response.buL()) if(c.cosmeticType==aNI.SPRAY && c.cosmeticId==id) return true;
  return false;
 }
}
""",
    "net/badlion/a/aCY.java": """
package net.badlion.a;
import net.badlion.clientcommon.type.cosmetics.response.a;
public class aCY {
 public final aFM sprays=new aFM(); public final a response=new a();
 public aFM bsF() { return sprays; } public a bsd() { return response; }
}
""",
    "SprayFixture.java": """
import java.util.*;
import net.badlion.a.*;
import net.badlion.clientcommon.type.cosmetics.response.a;
import com.google.gson.JsonObject;
public class SprayFixture {
 static native List<aCV> merge(aCY m,List<aCV> store);
 static native boolean install(a response,List<aCV> all,aDp active);
 static native void verify(aCY m);
 static int checks;
 static void check(boolean ok,String label) {
  if(!ok) throw new AssertionError(label);
  ++checks; System.out.println("PASS "+label);
 }
 static aCV spray(int id) { return new aCV(id,aNI.SPRAY,new JsonObject()); }
 static aCV find(List<aCV> all,int id) {
  return all.stream().filter(c->c.cosmeticType==aNI.SPRAY && c.cosmeticId==id).findFirst().orElseThrow();
 }
 public static void main(String[] args) {
  System.load(args[0]);
  aCY m=new aCY();
  aCV cape=new aCV(0,aNI.CAPE,new JsonObject()); cape.active=true;
  aCV storeSpray=spray(185),owned=spray(40); owned.cosmeticData.marker="owned metadata";
  m.response.cosmetics.add(owned); m.response.cosmetics.add(owned);
  m.sprays.entries.put(0,new aFK(0,"Black Cat"));
  m.sprays.entries.put(40,new aFK(40,"Badlion"));
  m.sprays.entries.put(185,new aFK(185,"Store resource"));
  m.sprays.entries.put(252,new aFK(252,"Last"));
  m.sprays.entries.put(-9,new aFK(-9,"Invalid"));
  List<aCV> store=new ArrayList<>(List.of(cape,storeSpray));
  List<aCV> all=merge(m,store);
  check(all!=null && all.size()==5,"union of store, owned, registered IDs");
  check(store.size()==2 && m.response.cosmetics.size()==2,"merge does not mutate input lists");
  check(all.get(0)==cape && find(all,185)==storeSpray,"reuse original store objects and order");
  check(find(all,40)==owned && owned.cosmeticData.marker.equals("owned metadata"),"retain missing owned object metadata");
  check(all.stream().filter(c->c.cosmeticType==aNI.SPRAY && c.cosmeticId==40).count()==1,"deduplicate repeated owned entries");
  check(find(all,0).name.equals("Black Cat") && !find(all,0).active,"registry name and inactive default");
  check(all.stream().noneMatch(c->c.cosmeticId<0),"skip invalid registry ID");
  aDp wrapper=new aDp(all); List<aCV> old=m.response.cosmetics;
  check(install(m.response,all,wrapper),"publish complete snapshot");
  check(m.response.cosmetics!=all && m.response.cosmetics!=old && old.size()==2,"publish uses non-aliasing list copy");
  check(m.response.i(aNI.SPRAY).size()==4 && m.response.i(aNI.CAPE).size()==1,"owned cache includes inactive sprays");
  check(!m.response.needsUpdate.get() && m.response.userCosmetics==wrapper,"snapshot state consistent");
  check(wrapper.active.size()==1 && wrapper.active.get(0)==cape,"active-only wrapper remains active-only");
  aFM.response=m.response;
  check(aFM.X(0) && aFM.X(252) && !aFM.X(-1) && !aFM.X(1000000),"positive and negative ownership controls");
  verify(m);
  check(merge(m,all).size()==5,"repeated merge does not add duplicate IDs");
  List<aCV> installed=m.response.cosmetics; Object cache=m.response.ownedCosmeticCache;
  List<aCV> invalid=new ArrayList<>(all); invalid.add(null);
  check(!install(m.response,invalid,wrapper) && m.response.cosmetics==installed && m.response.ownedCosmeticCache==cache,"bad catalog fails before publishing");
  check(merge(new aCY(),store)==null,"empty registry waits without installing");
  check(merge(null,store)==null && merge(m,null)==null,"null merge inputs fail closed");
  check(!install(m.response,null,wrapper) && !install(null,all,wrapper),"null install inputs fail closed");
  aCY large=new aCY();
  for(int i=0;i<4096;i++) large.sprays.entries.put(i,new aFK(i,"Spray "+i));
  List<aCV> many=merge(large,new ArrayList<>());
  check(many.size()==4096 && install(large.response,many,new aDp(many)) && large.response.i(aNI.SPRAY).size()==4096,"large registry local-reference/cache regression");
  System.out.println("FIXTURE_OK checks="+checks+"; not a game rendering test");
 }
}
""",
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jdk", default="C:/Program Files/Eclipse Adoptium/jdk-17.0.19.10-hotspot")
    parser.add_argument("--vs", default="C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/VsDevCmd.bat")
    args = parser.parse_args()
    out = HERE / "fixture-out"
    classes = out / "classes"
    classes.mkdir(parents=True, exist_ok=True)
    paths = []
    for name, source in SOURCES.items():
        p = out / "src" / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(source, encoding="utf-8")
        paths.append(str(p))
    log = []
    def run(command):
        result = subprocess.run(command, shell=isinstance(command, str), cwd=out, text=True, encoding="utf-8", errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end="")
        log.append(result.stdout)
        (HERE / "fixture-results.txt").write_text("".join(log), encoding="utf-8")
        if result.returncode:
            raise SystemExit(result.returncode)
        return result.stdout
    jdk = Path(args.jdk)
    run([str(jdk / "bin/javac.exe"), "-encoding", "UTF-8", "-d", str(classes), *paths])
    dll = out / "spray_fixture.dll"
    command = (f'call "{args.vs}" -arch=x64 && cl /nologo /utf-8 /std:c++17 /EHsc /LD '
               f'"{HERE / "fixture_bridge.cpp"}" /I"{jdk / "include"}" /I"{jdk / "include/win32"}" '
               f'/Fo"{out / "fixture_bridge.obj"}" /link /OUT:"{dll}"')
    run(command)
    result = run([str(jdk / "bin/java.exe"), "-cp", str(classes), "SprayFixture", str(dll)])
    if "FIXTURE_OK checks=19" not in result:
        raise SystemExit("Missing fixture success marker")
    (HERE / "fixture-result.json").write_text(json.dumps({
        "passed": True, "checks": 19,
        "scope": "actual production include in a synthetic JNI fixture; not Minecraft rendering",
    }, indent=2) + "\n", encoding="utf-8")

if __name__ == "__main__":
    main()
