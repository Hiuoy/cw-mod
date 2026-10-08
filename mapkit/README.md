# mapkit

Tools for building custom maps for Black Ops Cold War (build 1.34.0.15931218). Status and what's
next: [docs/ROADMAP.md](../docs/ROADMAP.md#5-custom-maps-mapkit). Each step's findings:
[docs/mapkit-plan.md](../docs/mapkit-plan.md). The reversed zone formats:
[docs/mapkit-roadmap.md](../docs/mapkit-roadmap.md).

- `godot/`: the level builder, a Godot 4 project with the mapkit plugin. Lay out a map, place
  the Zombies objects, walk it, export the `.mkmap` source. See [godot/README.md](godot/README.md).
- [mkmap-format.md](mkmap-format.md): the `.mkmap` map source that the builder writes and cwlink
  reads.
- `zonekit/`: fastfile library. Reads `.ff` zones, applies their `.fd` patches, writes `.ff` zones.
- `ffinfo/`: command-line inspector built on zonekit.
- `mkasset/`: reads the game's models out of your install for the level builder (game data: it
  only ever writes to a local cache).
- `cwlink/`: builds a map from its `.mkmap` source into `cw-mod/maps/<id>`: the map's own zone
  (`<id>.ff`) and level scripts. The builder's Build button runs it.

mapkit uses the game's own `oo2core_8_win64.dll` from your install at runtime. Nothing from the
game is shipped with mapkit or belongs in this repo.

## Build

`generate.bat`, then build `mapkit\ffinfo` in `t9_vs2022.sln` (same toolset override as the client:
`/p:PlatformToolset=v145`). Output: `build\t9_vs2022\x64\ffinfo\ffinfo.exe`.

## ffinfo

```
set MAPKIT_GAME_DIR=E:\Games\...\Call of Duty Black Ops Cold War (1.34.0.15931218)

ffinfo zm_silver                     header, xblock sizes, asset counts by type
ffinfo zm_silver --assets            every asset in the table
ffinfo zm_silver --stream out.bin    the current (patched) xfile stream
ffinfo zm_silver --repack out.ff     a standalone .ff (no .fd), checked by reading it back
ffinfo --scan                        decode every zone in the install
ffinfo zm_silver --walk              walk the stream asset by asset with mapkit's loaders
ffinfo --walk-scan                   walk every zone: complete ones, and the missing loaders that stop the rest
ffinfo zm_silver --entities          the map's entities (entity list + trigger list), counted by classname
ffinfo zm_silver --entities-json f   every entity with all its keys, as JSON
```

## cwlink

```
cwlink build zm_silver <map>.mkmap --trace <game>\cw-mod\mapkit\trace\zm_silver.mktrace
                                     a map from its source, into cw-mod/maps/<id> (what Build runs);
                                     --overlay writes the older override-zone form instead
cwlink clone zm_silver zm_mapkit     a renamed copy of a map's whole zone set, into cw-mod/maps (tests only)
cwlink replace zm_silver             zm_silver.ff repacked without its .fd, into cw-mod/maps (tests only)
cwlink replace zm_silver --move script_noteworthy=talent_speedcola@1080,11,80,253
                                     the same, with every matching entity moved first
```

`clone` and `replace` write a copy of your own game data into your own game folder. Never share the
result.

## mkasset

```
mkasset catalog out.json             every model the traced zones hold: zone, bounds, LODs, materials
mkasset export <model> out.glb       one model as glTF binary, in Godot's space (Y up, meters)
        [--lod n]                    0 = full detail (the default), counting down
mkasset serve                        stay loaded; one JSON request per stdin line (the level builder)
        --zone <name>                (any command) read only these zones; default: every traced zone
```

`<model>` is an xmodel's name or hash. A zone is read through its trace
(`cw-mod/mapkit/trace/<zone>.mktrace`), since level zones cannot be walked yet; a model a zone only
references (it lives in another zone) comes from that other zone's trace. Indexing Die Maschine takes
about 1 s, and each model a few milliseconds after that. The output is game geometry: keep it local.
An xmodel's LOD slots are not in detail order (on Die Maschine the full model is usually the LAST
slot), so mkasset ranks them by triangle count.

The zones store model hashes, not names. `python mapkit/mkasset/model_names.py catalog.json
mapkit/godot/addons/mapkit/game_model_names.txt` names a catalog's models through ACTS
(`acts -t lookup`, keeping a name only when it hashes back): the level builder's model picker list.

A walk is complete when it ends on the stream's last byte and every XBlock size it implies matches
the zone header. The stream holds no sizes, so a wrong loader desynchronises everything after it.

`--game <dir>` overrides `MAPKIT_GAME_DIR`. A path to a `.ff` works in place of a zone name.
Repacked zones keep the retail signature, which no longer matches their blocks, so the game only
loads them once the client loader skips the signature check for custom zones (roadmap M3).
