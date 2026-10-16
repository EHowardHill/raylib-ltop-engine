// LttP engine resource editor — Tauri backend.
//
// Mirrors the API the old Node server exposed (scan/save/create/delete/
// upload/build) as Tauri IPC commands. All reads and writes are confined to
// `resources/` inside the open project (the engine repo root); JSON files
// are written with 4-space indentation to match the previous
// JSON.stringify(…, null, 4) output byte for byte.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use base64::engine::general_purpose::STANDARD as BASE64;
use base64::Engine;
use serde_json::{json, Map, Value};
use std::fs;
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Manager, State};

const DIRECTIONS: [&str; 4] = ["down", "right", "left", "up"];
const BUILD_TIMEOUT: Duration = Duration::from_secs(300);
// First-run toolchain builds (raylib for emcc, the KOS docker image) run
// long; the shipped builds just compile.
const WEB_BUILD_TIMEOUT: Duration = Duration::from_secs(900);
const DREAMCAST_BUILD_TIMEOUT: Duration = Duration::from_secs(1800);

struct ProjectState(Mutex<Option<PathBuf>>);

// The launched game (Build & Play / Play from here / the enemy sandbox):
// one tracked child so the editor can quit it, and a new launch replaces
// the previous session instead of stacking windows.
static GAME_CHILD: Mutex<Option<std::process::Child>> = Mutex::new(None);
// The web-export static server (build/web over http; browsers refuse
// file:// wasm). One server at a time; a new export re-hosts.
static WEB_SERVER: Mutex<Option<std::process::Child>> = Mutex::new(None);

// ---- project root ---------------------------------------------------------

fn looks_like_root(dir: &Path) -> bool {
    dir.join("resources").is_dir()
}

fn find_root(start: Option<&Path>) -> Option<PathBuf> {
    start?
        .ancestors()
        .find(|dir| looks_like_root(dir))
        .map(Path::to_path_buf)
}

fn project_root(state: &ProjectState) -> Result<PathBuf, String> {
    state
        .0
        .lock()
        .unwrap()
        .clone()
        .ok_or_else(|| "No project open".to_string())
}

// The engine repo — where the build scripts and the generator live.
// Usually the repo the app was launched from (dev builds: the executable
// sits deep inside it); the open project itself when it is the repo.
fn engine_root(state: &ProjectState) -> Result<PathBuf, String> {
    if let Ok(exe) = std::env::current_exe() {
        for dir in exe.ancestors().skip(1) {
            if dir.join("build-mac.sh").is_file() && dir.join("scripts").is_dir() {
                return Ok(dir.to_path_buf());
            }
        }
    }
    let root = project_root(state)?;
    if root.join("build-mac.sh").is_file() && root.join("scripts").is_dir() {
        return Ok(root);
    }
    Err("Engine not found (no build-mac.sh beside the app or the project)".to_string())
}

// A game other than the engine repo's own bakes via the LTOP_GAME env:
// the generator reads it and bakes that resources/ folder instead.
fn build_env(engine: &Path, root: &Path) -> Vec<(std::ffi::OsString, std::ffi::OsString)> {
    let mut env: Vec<(std::ffi::OsString, std::ffi::OsString)> = std::env::vars_os().collect();
    if root != engine {
        env.push(("LTOP_GAME".into(), root.join("resources").into()));
    }
    env
}

fn config_file(app: &AppHandle) -> Option<PathBuf> {
    let dir = app.path().app_config_dir().ok()?;
    fs::create_dir_all(&dir).ok()?;
    Some(dir.join("config.json"))
}

fn load_config(app: &AppHandle) -> Option<PathBuf> {
    let file = config_file(app)?;
    let text = fs::read_to_string(file).ok()?;
    let value: Value = serde_json::from_str(&text).ok()?;
    let dir = PathBuf::from(value.get("project")?.as_str()?);
    if looks_like_root(&dir) { Some(dir) } else { None }
}

fn persist_config(app: &AppHandle, dir: &Path) -> Result<(), String> {
    let file = config_file(app).ok_or("No config directory")?;
    let doc = json!({ "project": dir.to_string_lossy() });
    fs::write(&file, serde_json::to_string_pretty(&doc).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())
}

// ---- new game template ------------------------------------------------------

// Starter tileset (8x2 32px tiles: floor, wall, grass, water, pit, path,
// bush, rock + variations) — a small placeholder so a fresh game renders
// something recognizable before any art exists.
const STARTER_TILESET_B64: &str = include_str!("starter_tileset.b64");

fn slugify(name: &str) -> String {
    let slug: String = name
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() { c.to_ascii_lowercase() } else { '-' })
        .collect();
    let slug = slug.trim_matches('-').to_string();
    if slug.is_empty() { "newgame".to_string() } else { slug }
}

fn create_new_game(parent: &Path, name: &str) -> Result<PathBuf, String> {
    let name = name.trim();
    if !valid_name(name) {
        return Err("Invalid name (letters, numbers, spaces, _ and - only)".to_string());
    }
    let root = parent.join(name);
    if root.exists() {
        return Err(format!("{} already exists", root.display()));
    }
    let res = root.join("resources");

    fs::create_dir_all(res.join("rooms")).map_err(|e| e.to_string())?;
    fs::create_dir_all(res.join("characters")).map_err(|e| e.to_string())?;
    fs::create_dir_all(res.join("tiles")).map_err(|e| e.to_string())?;

    // Game identity.
    let game = json!({ "title": name.to_uppercase(), "slug": slugify(name) });
    fs::write(res.join("game.json"), pretty4(&game)).map_err(|e| e.to_string())?;

    // Starter tileset + (empty) behavior metadata.
    let bytes = BASE64.decode(STARTER_TILESET_B64.trim()).map_err(|e| e.to_string())?;
    fs::write(res.join("tiles").join("texture.png"), bytes).map_err(|e| e.to_string())?;
    fs::write(res.join("tiles").join("tiles.json"), pretty4(&json!({ "tiles": {} })))
        .map_err(|e| e.to_string())?;

    // The hero: placeholder art synthesizes from the sheet layout at bake.
    let hero = json!({
        "width": 4,
        "stand": {
            "down": [0], "right": [1], "left": [2], "up": [3]
        },
        "move": {
            "down": [0, 4], "right": [1, 5], "left": [2, 6], "up": [3, 7]
        }
    });
    fs::write(res.join("characters").join("hero.json"), pretty4(&hero)).map_err(|e| e.to_string())?;

    // A first room: walled field with a spawn, wired to the hero.
    let width = 20usize;
    let height = 14usize;
    let decor = vec![Value::from(1); width * height];           // floor
    let mut colliders = vec![Value::from(0); width * height];
    for x in 0..width {
        colliders[x] = Value::from(2);                          // wall
        colliders[(height - 1) * width + x] = Value::from(2);
    }
    for y in 0..height {
        colliders[y * width] = Value::from(2);
        colliders[y * width + width - 1] = Value::from(2);
    }
    let room = json!({
        "width": width,
        "height": height,
        "default": { "hero": "hero", "spawn": [9, 7] },
        "decor": decor,
        "colliders": colliders
    });
    fs::write(res.join("rooms").join("01.json"), pretty4(&room)).map_err(|e| e.to_string())?;

    Ok(root)
}

// ---- path safety ----------------------------------------------------------

// Resolve `rel` (like "resources/rooms/01.json") against the project root,
// rejecting absolute paths and any traversal, and requiring the result to
// sit under resources/.
fn resolve_resource(root: &Path, rel: &str) -> Result<PathBuf, String> {
    let rel_path = Path::new(rel);
    if rel_path.is_absolute() {
        return Err("Invalid path".to_string());
    }
    let mut full = root.to_path_buf();
    for part in rel_path.components() {
        match part {
            Component::Normal(segment) => full.push(segment),
            Component::CurDir => {}
            _ => return Err("Invalid path".to_string()),
        }
    }
    let mut components = full.strip_prefix(root).map_err(|_| "Invalid path".to_string())?.components();
    match components.next() {
        Some(first) if first.as_os_str() == std::ffi::OsStr::new("resources") => {}
        _ => return Err("Invalid path".to_string()),
    }
    Ok(full)
}

// ---- helpers ---------------------------------------------------------------

fn list_files(res: &Path, sub: &str) -> Vec<String> {
    let mut out = Vec::new();
    let entries = match fs::read_dir(res.join(sub)) {
        Ok(entries) => entries,
        Err(_) => return out,
    };
    for entry in entries.flatten() {
        if !entry.file_type().map(|t| t.is_file()).unwrap_or(false) {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if name == ".DS_Store" {
            continue;
        }
        out.push(name);
    }
    out.sort();
    out
}

fn read_json(path: &Path) -> Result<Value, String> {
    let text = fs::read_to_string(path).map_err(|e| format!("{}: {}", path.display(), e))?;
    serde_json::from_str(&text).map_err(|e| format!("{}: {}", path.display(), e))
}

fn png_size(path: &Path) -> Option<(u32, u32)> {
    let mut file = fs::File::open(path).ok()?;
    let mut head = [0u8; 24];
    file.read_exact(&mut head).ok()?;
    if head[..8] != [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A] {
        return None;
    }
    Some((
        u32::from_be_bytes(head[16..20].try_into().ok()?),
        u32::from_be_bytes(head[20..24].try_into().ok()?),
    ))
}

// The tileset is a single png of 32x32 tiles; derive all metadata from it.
// Tilesets for the scan: every .png in resources/tiles with its derived
// grid shape, plus the metadata file the generator reads for it
// (<stem>.json when present; the legacy tiles.json for the default
// tileset; None when neither exists) and its parsed contents when found.
fn read_tilesets(res: &Path) -> (Vec<Value>, usize) {
    let files: Vec<String> = list_files(res, "tiles")
        .into_iter()
        .filter(|f| f.ends_with(".png"))
        .collect();
    let default_index = files
        .iter()
        .position(|f| f == "texture.png")
        .unwrap_or(0);

    let mut out = Vec::new();
    for (i, file) in files.into_iter().enumerate() {
        let Some((width, height)) = png_size(&res.join("tiles").join(&file)) else {
            continue;
        };
        let columns = (width / 32) as u64;
        let stem = file.trim_end_matches(".png");
        // Mirror of the generator's tileset_meta_file rule.
        let meta_path = if res.join("tiles").join(format!("{}.json", stem)).is_file() {
            Some(format!("{}.json", stem))
        } else if i == default_index && res.join("tiles").join("tiles.json").is_file() {
            Some("tiles.json".to_string())
        } else {
            None
        };
        let meta = meta_path
            .as_ref()
            .and_then(|m| read_json(&res.join("tiles").join(m)).ok());
        out.push(json!({
            "file": file,
            "image": format!("resources/tiles/{}", file),
            "columns": columns.max(1),
            "tileCount": columns.max(1) * (height / 32) as u64,
            "tileWidth": 32,
            "tileHeight": 32,
            "metaFile": meta_path,
            "meta": meta,
        }));
    }
    (out, default_index)
}

fn mime_for(path: &Path) -> &'static str {
    match path.extension().and_then(|e| e.to_str()).unwrap_or("").to_lowercase().as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        "svg" => "image/svg+xml",
        "bmp" => "image/bmp",
        "wav" => "audio/wav",
        "mp3" => "audio/mpeg",
        "ogg" => "audio/ogg",
        "flac" => "audio/flac",
        "xm" => "audio/xm",
        "mod" => "audio/mod",
        "it" => "audio/it",
        "s3m" => "audio/s3m",
        "otf" => "font/otf",
        "ttf" => "font/ttf",
        "json" => "application/json",
        _ => "application/octet-stream",
    }
}

// JSON.stringify(value, null, 4) equivalent, so generated files match what
// the old editor wrote (and what's already committed in resources/).
fn pretty4(value: &Value) -> String {
    let mut out = String::new();
    write_pretty(value, 0, &mut out);
    out
}

fn write_pretty(value: &Value, indent: usize, out: &mut String) {
    match value {
        Value::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return;
            }
            let pad = "    ".repeat(indent);
            let inner = "    ".repeat(indent + 1);
            out.push_str("[\n");
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push_str(",\n");
                }
                out.push_str(&inner);
                write_pretty(item, indent + 1, out);
            }
            out.push('\n');
            out.push_str(&pad);
            out.push(']');
        }
        Value::Object(map) => {
            if map.is_empty() {
                out.push_str("{}");
                return;
            }
            let pad = "    ".repeat(indent);
            let inner = "    ".repeat(indent + 1);
            out.push_str("{\n");
            for (i, (key, val)) in map.iter().enumerate() {
                if i > 0 {
                    out.push_str(",\n");
                }
                out.push_str(&inner);
                out.push_str(&serde_json::to_string(key).unwrap_or_default());
                out.push_str(": ");
                write_pretty(val, indent + 1, out);
            }
            out.push('\n');
            out.push_str(&pad);
            out.push('}');
        }
        other => out.push_str(&other.to_string()),
    }
}

fn valid_name(name: &str) -> bool {
    let mut chars = name.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphanumeric() => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == ' ' || c == '_' || c == '-')
}

fn empty_directions() -> Map<String, Value> {
    let mut map = Map::new();
    for dir in DIRECTIONS {
        map.insert(dir.to_string(), Value::Array(Vec::new()));
    }
    map
}

// ---- commands ----------------------------------------------------------------

#[tauri::command]
fn scan(state: State<ProjectState>) -> Result<Value, String> {
    let root = match state.0.lock().unwrap().clone() {
        Some(root) => root,
        None => return Ok(json!({ "project": Value::Null })),
    };
    let res = root.join("resources");

    let platform = match std::env::consts::OS {
        "macos" => "darwin",
        "windows" => "win32",
        other => other,
    };

    let mut dialogues = Vec::new();
    let (tilesets, default_tileset) = read_tilesets(&res);
    for file in list_files(&res, "dialogue").into_iter().filter(|f| f.ends_with(".json")) {
        dialogues.push(json!({
            "file": file,
            "data": read_json(&res.join("dialogue").join(&file))?,
        }));
    }

    let mut characters = Vec::new();
    for file in list_files(&res, "characters").into_iter().filter(|f| f.ends_with(".json")) {
        let png = res.join("characters").join(file.trim_end_matches(".json").to_string() + ".png");
        characters.push(json!({
            "file": file,
            "data": read_json(&res.join("characters").join(&file))?,
            "png": png.is_file(),
        }));
    }

    let mut rooms = Vec::new();
    for file in list_files(&res, "rooms").into_iter().filter(|f| f.ends_with(".json")) {
        let stem = file.trim_end_matches(".json");
        let data = read_json(&res.join("rooms").join(&file))?;
        let dim = |key: &str, default: u64| -> u64 {
            let raw = data.get(key).and_then(Value::as_u64).unwrap_or(default);
            if raw == 0 { default } else { raw }
        };
        let width = dim("width", 30);
        let height = dim("height", 20);
        let layer = |name: &str| -> Vec<i64> {
            let values = data.get(name).and_then(Value::as_array);
            (0..width * height)
                .map(|i| {
                    values
                        .and_then(|v| v.get(i as usize))
                        .and_then(Value::as_i64)
                        .unwrap_or(0)
                })
                .collect()
        };
        rooms.push(json!({
            "file": file,
            "legacyTmx": res.join("rooms").join(stem.to_string() + ".tmx").is_file(),
            "mapWidth": width,
            "mapHeight": height,
            "default": data.get("default").filter(|v| !v.is_null()).cloned().unwrap_or_else(|| json!({})),
            "encounters": data.get("encounters").filter(|v| !v.is_null()).cloned().unwrap_or_else(|| json!({})),
            "decor": layer("decor"),
            "colliders": layer("colliders"),
            "upper": layer("upper"),
            // Full parsed room JSON, so editor saves can preserve keys the
            // UI doesn't edit (enemies, objects, ...).
            "raw": data,
        }));
    }

    // Entity placement support: the object type compositions, the enemy
    // roster, and the item list (chest grant pickers). Types and enemies
    // carry their sprite's existence so the Files tab can audit what art
    // is still a bake-time placeholder.
    let object_types = read_json(&res.join("objects").join("types.json"))
        .ok()
        .and_then(|v| v.get("types").cloned())
        .map(|types| {
            types
                .as_array()
                .cloned()
                .unwrap_or_default()
                .into_iter()
                .map(|t| {
                    let image = t
                        .get("image")
                        .and_then(Value::as_str)
                        .map(str::to_string)
                        .or_else(|| t.get("name").and_then(Value::as_str).map(|n| format!("{}.png", n)))
                        .unwrap_or_default();
                    let mut out = t.clone();
                    out["png"] = json!(res.join("objects").join(&image).is_file());
                    out
                })
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();

    let mut enemies = Vec::new();
    for file in list_files(&res, "enemies").into_iter().filter(|f| f.ends_with(".json")) {
        let data = read_json(&res.join("enemies").join(&file))?;
        let image = data
            .get("image")
            .and_then(Value::as_str)
            .map(str::to_string)
            .unwrap_or_else(|| file.trim_end_matches(".json").to_string() + ".png");
        enemies.push(json!({
            "name": file.trim_end_matches(".json"),
            "file": file,
            "png": res.join("enemies").join(&image).is_file(),
            "data": data,
        }));
    }

    let mut items = Vec::new();
    for file in list_files(&res, "items").into_iter().filter(|f| f.ends_with(".json")) {
        let data = read_json(&res.join("items").join(&file))?;
        items.push(json!({
            "name": file.trim_end_matches(".json"),
            "label": data.get("label").cloned().unwrap_or_else(|| json!(file)),
            "data": data,
        }));
    }

    // Tile behavior metadata (tiles.json) and the game-code behavior
    // roster (code/manifest.json) for the definition editors.
    let tile_meta = read_json(&res.join("tiles").join("tiles.json")).ok();
    let behaviors = read_json(&res.join("code").join("manifest.json"))
        .ok()
        .and_then(|v| v.get("behaviors").cloned())
        .unwrap_or_else(|| json!([]));
    // Game-code sources (editable in the Code tab).
    let code_files: Vec<String> = list_files(&res, "code")
        .into_iter()
        .filter(|f| f.ends_with(".cpp"))
        .collect();

    Ok(json!({
        "project": root.to_string_lossy(),
        "platform": platform,
        "tilesets": tilesets,
        "defaultTileset": default_tileset,
        "dialogues": dialogues,
        "characters": characters,
        "rooms": rooms,
        "music": list_files(&res, "music"),
        "audio": list_files(&res, "audio"),
        "ui": list_files(&res, "ui"),
        "pickups": list_files(&res, "pickups"),
        "effects": list_files(&res, "effects"),
        "objects": list_files(&res, "objects"),
        "fonts": list_files(&res, "fonts"),
        // Scene pictures (the dialogue "image" cue): the picker and the
        // asset audit read this list.
        "scenes": list_files(&res, "scenes")
            .into_iter()
            .filter(|f| f.ends_with(".png"))
            .collect::<Vec<_>>(),
        "objectTypes": object_types,
        "enemies": enemies,
        "items": items,
        "tileMeta": tile_meta,
        "behaviors": behaviors,
        "codeFiles": code_files,
        "game": read_json(&res.join("game.json")).ok(),
    }))
}

#[tauri::command]
fn save(state: State<ProjectState>, path: String, content: String) -> Result<Value, String> {
    let root = project_root(&state)?;
    let target = resolve_resource(&root, &path)?;
    match target.extension().and_then(|e| e.to_str()) {
        Some("json") => {}
        // Game-code behavior sources are editable too, confined to
        // resources/code/ (resolve_resource already bounds everything to
        // the project's resources/ tree).
        Some("cpp") => {
            let in_code_dir = target
                .parent()
                .and_then(|p| p.file_name())
                .map(|n| n == std::ffi::OsStr::new("code"))
                .unwrap_or(false);
            if !in_code_dir {
                return Err(".cpp files can only be saved under resources/code/".to_string());
            }
        }
        _ => return Err("Only .json files (and .cpp under resources/code/) can be saved".to_string()),
    }
    fs::write(&target, content).map_err(|e| e.to_string())?;
    Ok(json!({ "ok": true }))
}

#[tauri::command]
fn create(
    state: State<ProjectState>,
    kind: String,
    name: String,
    width: Option<i64>,
    height: Option<i64>,
) -> Result<Value, String> {
    let root = project_root(&state)?;
    let res = root.join("resources");
    let name = name.trim();
    if !valid_name(name) {
        return Err("Invalid name (letters, numbers, spaces, _ and - only)".to_string());
    }

    match kind.as_str() {
        "dialogue" => {
            let target = res.join("dialogue").join(format!("{}.json", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            let doc = json!({ "lines": [] });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        "character" => {
            let target = res.join("characters").join(format!("{}.json", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            // The optional state channels start empty: the engine falls
            // back to the stand/move pair until frames are filled in (or
            // duplicated from stand as placeholders).
            let doc = json!({
                "width": 4,
                "stand": Value::Object(empty_directions()),
                "move": Value::Object(empty_directions()),
                "attack": Value::Object(empty_directions()),
                "lift": Value::Object(empty_directions()),
                "swim": Value::Object(empty_directions()),
                "hurt": Value::Object(empty_directions()),
            });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        "enemy" => {
            let target = res.join("enemies").join(format!("{}.json", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            let doc = json!({
                "width": 4,
                "color": [200, 60, 60],
                "stand": Value::Object(empty_directions()),
                "move": Value::Object(empty_directions()),
                "stats": {
                    "hp": 2,
                    "speed": 40,
                    "damage": 1,
                    "behavior": ""
                }
            });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        "item" => {
            let target = res.join("items").join(format!("{}.json", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            let doc = json!({
                "label": name.to_uppercase(),
                "use": { "kind": "none" }
            });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        // A new tileset scaffolds the same starter sheet new games get
        // (placeholder art until a real one is uploaded) + empty behavior
        // metadata. Rooms reference it by this name.
        "tileset" => {
            if name == "texture" {
                return Err("texture is the default tileset's name; replace it instead".to_string());
            }
            let png = res.join("tiles").join(format!("{}.png", name));
            if png.exists() {
                return Err("Already exists".to_string());
            }
            let bytes = BASE64.decode(STARTER_TILESET_B64.trim()).map_err(|e| e.to_string())?;
            fs::create_dir_all(res.join("tiles")).map_err(|e| e.to_string())?;
            fs::write(&png, bytes).map_err(|e| e.to_string())?;
            let meta = res.join("tiles").join(format!("{}.json", name));
            if !meta.exists() {
                fs::write(&meta, pretty4(&json!({ "tiles": {} }))).map_err(|e| e.to_string())?;
            }
            Ok(json!({ "ok": true }))
        }
        // Custom-behavior scaffolding: a registered boilerplate unit in
        // resources/code/ plus the manifest entry the generator validates
        // against. Compile errors surface through the Build tab (or the
        // editor's compile check) after saving.
        "behavior" => {
            let target = res.join("code").join(format!("{}.cpp", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            fs::create_dir_all(res.join("code")).map_err(|e| e.to_string())?;
            let boiler = format!(
                "#include \"gamecode.h\"\n#include \"enemy.h\"\n#include \"room.h\"\n\n\
// {name}: a game-code custom behavior (layer 4). Registered at static init;\n\
// an enemy references it as \"custom:{name}\" in its stats.\n\
// Engine rules apply: frameDt() if per-frame timing is needed, the engine Rng\n\
// (passed in) for randomness, fixed arrays, no file I/O, no exceptions/RTTI\n\
// (the Dreamcast toolchain).\n\n\
static void {name}Behavior(Enemy& self, const Room& room, Vector2 heroCenter,\n\
                          Rng& rng, float dt)\n\
{{\n\
    // scratch(i) keeps private per-instance state (zeroed at spawn):\n\
    // headings, timers, sentinels.\n\
    switch (self.modeState())\n\
    {{\n\
    case Enemy::ENEMY_IDLE:\n\
        break;\n\
    case Enemy::ENEMY_WALK:\n\
        break;\n\
    case Enemy::ENEMY_PAUSE:\n\
        break;\n\
    default:\n\
        self.setMode(Enemy::ENEMY_IDLE);\n\
        break;\n\
    }}\n\
}}\n\n\
// Bake-time-linked registration (the gamecode seam).\n\
static const bool {name}Registered =\n\
    (gamecodeRegisterEnemyBehavior(\"{name}\", {name}Behavior), true);\n",
                name = name
            );
            fs::write(&target, boiler).map_err(|e| e.to_string())?;

            // Manifest: the generator validates custom: references against
            // this list, so the scaffold registers itself.
            let manifest_path = res.join("code").join("manifest.json");
            let mut manifest = read_json(&manifest_path).unwrap_or_else(|_| json!({}));
            if !manifest.get("behaviors").map(Value::is_array).unwrap_or(false) {
                manifest["behaviors"] = json!([]);
            }
            let list = manifest.get_mut("behaviors").unwrap();
            let exists = list.as_array().map(|a| a.iter().any(|b| b == &json!(name))).unwrap_or(false);
            if !exists {
                list.as_array_mut().unwrap().push(json!(name));
            }
            fs::write(&manifest_path, pretty4(&manifest)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        "room" => {
            let width = width.unwrap_or(30).clamp(1, 500);
            let height = height.unwrap_or(20).clamp(1, 500);
            let target = res.join("rooms").join(format!("{}.json", name));
            if target.exists() {
                return Err("Already exists".to_string());
            }
            let doc = json!({
                "width": width,
                "height": height,
                "default": {
                    "hero": "",
                    "spawn": [0, 0],
                    "music": "",
                    "walk_sfx": ""
                },
                "encounters": { "instant": [], "interactive": [] },
                "decor": Value::Array(vec![Value::from(0); (width * height) as usize]),
                "colliders": Value::Array(vec![Value::from(0); (width * height) as usize]),
            });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
            Ok(json!({ "ok": true }))
        }
        _ => Err("Unknown type".to_string()),
    }
}

#[tauri::command]
fn delete(state: State<ProjectState>, path: String) -> Result<Value, String> {
    let root = project_root(&state)?;
    let target = resolve_resource(&root, &path)?;
    match fs::metadata(&target) {
        Ok(meta) if meta.is_file() => {}
        _ => return Err("Not a file".to_string()),
    }
    fs::remove_file(&target).map_err(|e| e.to_string())?;
    Ok(json!({ "ok": true }))
}

#[tauri::command]
fn upload(state: State<ProjectState>, dir: String, name: String, data: String) -> Result<Value, String> {
    let upload_dirs: &[(&str, &[&str])] = &[
        ("music", &[".xm", ".mod", ".it", ".s3m", ".wav", ".mp3", ".ogg", ".flac"]),
        ("audio", &[".wav", ".mp3", ".ogg", ".flac", ".xm", ".mod"]),
        ("ui", &[".png"]),
        ("tiles", &[".png"]),
        ("characters", &[".png", ".json"]),
        ("enemies", &[".png"]),
        ("pickups", &[".png"]),
        ("effects", &[".png"]),
        ("objects", &[".png"]),
        ("fonts", &[".png", ".ttf", ".otf"]),
        ("scenes", &[".png"]),
    ];
    let root = project_root(&state)?;
    let res = root.join("resources");

    let allowed = upload_dirs
        .iter()
        .find(|(key, _)| *key == dir)
        .map(|(_, exts)| *exts)
        .ok_or_else(|| "Invalid directory".to_string())?;

    let name = Path::new(&name)
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or_else(|| "Invalid upload".to_string())?
        .to_string();
    let ext = format!(".{}", Path::new(&name).extension().and_then(|e| e.to_str()).unwrap_or("").to_lowercase());
    if !allowed.contains(&ext.as_str()) {
        return Err(format!("File type not allowed here ({})", ext));
    }

    let bytes = BASE64
        .decode(data.as_bytes())
        .map_err(|e| format!("Invalid upload: {}", e))?;
    fs::create_dir_all(res.join(&dir)).map_err(|e| e.to_string())?;
    fs::write(res.join(&dir).join(&name), bytes).map_err(|e| e.to_string())?;
    Ok(json!({ "ok": true }))
}

#[tauri::command]
fn read_resource(state: State<ProjectState>, path: String) -> Result<Value, String> {
    let root = project_root(&state)?;
    let target = resolve_resource(&root, &path)?;
    if !target.is_file() {
        return Err(format!("No such file: {}", path));
    }
    let bytes = fs::read(&target).map_err(|e| e.to_string())?;
    Ok(json!({
        "mime": mime_for(&target),
        "data": BASE64.encode(bytes),
    }))
}

fn run_script(
    engine: &Path,
    root: &Path,
    script: &str,
    timeout: Duration,
) -> Result<Value, String> {
    let mut child = std::process::Command::new("/bin/bash")
        .arg(script)
        .current_dir(engine)
        .envs(build_env(engine, root))
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("{}: {}", script, e))?;

    let output: Arc<Mutex<String>> = Arc::new(Mutex::new(String::new()));
    spawn_pipe_reader(child.stdout.take(), output.clone());
    spawn_pipe_reader(child.stderr.take(), output.clone());

    let start = Instant::now();
    let code = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status.code(),
            Ok(None) => {
                if start.elapsed() >= timeout {
                    let _ = child.kill();
                    let _ = child.wait();
                    break None;
                }
                std::thread::sleep(Duration::from_millis(50));
            }
            Err(_) => break None,
        }
    };
    std::thread::sleep(Duration::from_millis(100)); // let the readers drain
    Ok(json!({
        "ok": code == Some(0),
        "code": code,
        "output": output.lock().unwrap().clone(),
    }))
}

fn run_build(engine: PathBuf, root: PathBuf) -> Result<Value, String> {
    // Build button parity: the script follows the host (Mac: the native
    // build; everything else: the web build — the non-darwin path — with
    // emmc's own errors surfacing in the output). Builds run at the
    // engine root; a project that isn't the engine repo bakes its game
    // folder through LTOP_GAME.
    let script = if cfg!(target_os = "macos") {
        "./build-mac.sh"
    } else {
        "./build-web.sh"
    };
    run_script(&engine, &root, script, BUILD_TIMEOUT)
}

fn spawn_pipe_reader<R: Read + Send + 'static>(pipe: Option<R>, out: Arc<Mutex<String>>) {
    if let Some(mut pipe) = pipe {
        std::thread::spawn(move || {
            let mut buf = [0u8; 8192];
            loop {
                match pipe.read(&mut buf) {
                    Ok(0) | Err(_) => break,
                    Ok(n) => out.lock().unwrap().push_str(&String::from_utf8_lossy(&buf[..n])),
                }
            }
        });
    }
}

#[tauri::command]
async fn build(state: State<'_, ProjectState>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || run_build(engine, root))
        .await
        .map_err(|e| e.to_string())?
}

// ---- web export & dreamcast (the ship targets) ------------------------------

// An OS-assigned free localhost port (bound then released; the tiny race
// is fine for a dev server).
fn free_port() -> Option<u16> {
    std::net::TcpListener::bind("127.0.0.1:0")
        .ok()?
        .local_addr()
        .ok()?
        .port()
        .into()
}

fn open_in_browser(url: &str) {
    #[allow(unused_variables)]
    let command = if cfg!(target_os = "macos") {
        Some(("open", vec![url.to_string()]))
    } else if cfg!(target_os = "linux") {
        Some(("xdg-open", vec![url.to_string()]))
    } else if cfg!(target_os = "windows") {
        Some(("cmd", vec!["/C".to_string(), "start".to_string(), url.to_string()]))
    } else {
        None
    };
    if let Some((program, args)) = command {
        let _ = std::process::Command::new(program)
            .args(&args)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn();
    }
}

// Stop the web-export server (one server at a time; a new export re-hosts).
#[tauri::command]
fn stop_web() -> Result<Value, String> {
    if let Ok(mut guard) = WEB_SERVER.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let _ = child.wait();
            return Ok(json!({ "ok": true, "wasRunning": true }));
        }
    }
    Ok(json!({ "ok": true, "wasRunning": false }))
}

// Web build export: run build-web.sh (the editor is the whole toolchain —
// shipping shouldn't need a terminal). With `serve`, host build/web/ over
// http (browsers refuse file:// wasm) and open it in the default browser;
// a previous server is re-hosted on a fresh port.
#[tauri::command]
async fn build_web(
    state: State<'_, ProjectState>,
    serve: bool,
) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        let result = run_script(&engine, &root, "./build-web.sh", WEB_BUILD_TIMEOUT)?;
        if !result.get("ok").and_then(Value::as_bool).unwrap_or(false) || !serve {
            return Ok(result);
        }
        let web_dir = engine.join("build").join("web");
        if !web_dir.join("index.html").is_file() {
            let mut failed = result;
            failed["ok"] = json!(false);
            failed["error"] = json!("build-web.sh reported success but build/web/index.html is missing");
            return Ok(failed);
        }

        // Re-host: stop any previous server, then start a fresh one.
        if let Ok(mut guard) = WEB_SERVER.lock() {
            if let Some(mut old) = guard.take() {
                let _ = old.kill();
                let _ = old.wait();
            }
        }
        let port = free_port().ok_or("No free port for the web server")?;
        let child = std::process::Command::new(root_env_python(&root))
            .arg("-m").arg("http.server")
            .arg(port.to_string())
            .arg("--bind").arg("127.0.0.1")
            .arg("--directory").arg(&web_dir)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .map_err(|e| format!("http.server: {}", e))?;
        if let Ok(mut guard) = WEB_SERVER.lock() {
            *guard = Some(child);
        }

        let url = format!("http://127.0.0.1:{}/", port);
        open_in_browser(&url);
        let mut served = result;
        served["url"] = json!(url);
        Ok(served)
    })
    .await
    .map_err(|e| e.to_string())?
}

// Dreamcast disc image (build-dreamcast.sh): cross-compiles inside Docker
// (a working daemon is the only host requirement — its errors surface in
// the output). The first run builds the toolchain image, so the timeout
// is long.
#[tauri::command]
async fn build_dreamcast(state: State<'_, ProjectState>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        run_script(&engine, &root, "./build-dreamcast.sh", DREAMCAST_BUILD_TIMEOUT)
    })
    .await
    .map_err(|e| e.to_string())?
}

// ---- runbook smoke steps -----------------------------------------------------

// Generator run, warnings parsed out: the bake must be warning-free (the
// audio-budget notices on placeholder loops are known and fine).
#[tauri::command]
async fn run_generator(state: State<'_, ProjectState>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        let output = std::process::Command::new(root_env_python(&root))
            .arg("scripts/generate_assets.py")
            .current_dir(&engine)
            .envs(build_env(&engine, &root))
            .stdin(std::process::Stdio::null())
            .output()
            .map_err(|e| e.to_string())?;
        let stdout = String::from_utf8_lossy(&output.stdout).to_string();
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        let warnings: Vec<String> = stderr
            .lines()
            .filter(|l| l.starts_with("warning:"))
            .map(str::to_string)
            .collect();
        Ok(json!({
            "ok": output.status.success(),
            "warnings": warnings,
            "output": format!("{}{}", stdout, stderr),
        }))
    })
    .await
    .map_err(|e| e.to_string())?
}

// Headless boot + save soak: ./main --soak runs fresh game -> save ->
// reload -> re-save headlessly and exits by itself (exit 0 = every
// persisted byte round-tripped). WARNING/ERROR lines are surfaced.
#[tauri::command]
async fn headless_check(state: State<'_, ProjectState>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        let binary = engine.join("main");
        if !binary.is_file() {
            return Ok(json!({
                "ok": false,
                "output": "no ./main binary — run a native build first (Mac host)".to_string(),
            }));
        }
        let output = std::process::Command::new(&binary)
            .arg("--soak")
            .current_dir(&engine)
            .envs(build_env(&engine, &root))
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .output()
            .map_err(|e| e.to_string())?;
        let text = format!(
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        let problems: Vec<&str> = text
            .lines()
            .filter(|l| l.contains("WARNING: IMAGE") || l.contains("ERROR"))
            .collect();
        Ok(json!({
            "ok": output.status.success() && problems.is_empty(),
            "problems": problems,
            "output": text,
        }))
    })
    .await
    .map_err(|e| e.to_string())?
}

// Test-from-here: build (the game data is baked into the binary, so the
// build must run for edits to show up), then launch the built binary
// straight into a room at a tile — a throwaway session with saving
// suppressed (see the engine's --room/--tile handling). An optional enemy
// name spawns that enemy beside the drop tile (the tuning sandbox; the
// engine's --enemy flag). The launched game is tracked: a new session
// replaces the old one, and stop_game quits it.
#[tauri::command]
async fn play(
    state: State<'_, ProjectState>,
    room: String,
    tile: Option<String>,
    enemy: Option<String>,
) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        let build = run_build(engine.clone(), root)?;
        if !build.get("ok").and_then(Value::as_bool).unwrap_or(false) {
            return Ok(build);
        }
        let binary = engine.join("main");
        if !binary.is_file() {
            return Ok(json!({
                "ok": false,
                "error": "Build succeeded but no ./main exists — test-from-here needs the native binary (macOS host)"
            }));
        }
        let room = room.trim().to_string();
        let mut command = std::process::Command::new(&binary);
        if !room.is_empty() {
            command.arg("--room").arg(&room);
            if let Some(t) = &tile {
                command.arg("--tile").arg(t);
            }
            if let Some(e) = &enemy {
                if !e.trim().is_empty() {
                    command.arg("--enemy").arg(e.trim());
                }
            }
        }
        command.current_dir(&engine)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null());
        let child = command.spawn().map_err(|e| e.to_string())?;

        // Replace any previous session (restart semantics), then track.
        if let Ok(mut guard) = GAME_CHILD.lock() {
            if let Some(mut old) = guard.take() {
                let _ = old.kill();
                let _ = old.wait();
            }
            *guard = Some(child);
        }
        Ok(json!({ "ok": true }))
    })
    .await
    .map_err(|e| e.to_string())?
}

// Quit the running dev build (Build & Play / Play from here sessions).
#[tauri::command]
fn stop_game() -> Result<Value, String> {
    if let Ok(mut guard) = GAME_CHILD.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let _ = child.wait();
            return Ok(json!({ "ok": true, "wasRunning": true }));
        }
    }
    Ok(json!({ "ok": true, "wasRunning": false }))
}

// Compile-error surfacing for game code: a fast syntax-only pass over one
// resources/code unit (or the whole folder when none is given) with the
// engine's include paths, so behavior authors see compiler errors without
// running the full build. Non-Mac hosts fall back to the web build (the
// native compiler isn't clang++ there).
#[tauri::command]
async fn check_code(state: State<'_, ProjectState>, file: Option<String>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        if !cfg!(target_os = "macos") {
            let build = run_build(engine, root)?;
            if build.get("ok").and_then(Value::as_bool).unwrap_or(false) {
                return Ok(json!({ "ok": true, "output": "(web build compiled clean)" }));
            }
            return Ok(json!({ "ok": false, "output": build.get("output").cloned().unwrap_or(json!("")) }));
        }
        let mut command = std::process::Command::new("clang++");
        command.arg("-fsyntax-only")
            .arg("-std=c++11")
            .arg("-I").arg(engine.join("include"))
            .arg("-I").arg(engine.join("include_auto"))
            .current_dir(&root)
            .stdin(std::process::Stdio::null());
        // Raylib headers come via pkg-config on the mac build.
        if let Ok(cflags) = std::process::Command::new("pkg-config")
            .args(["--cflags", "raylib"])
            .output()
        {
            if cflags.status.success() {
                for flag in String::from_utf8_lossy(&cflags.stdout).split_whitespace() {
                    command.arg(flag);
                }
            }
        }
        if let Some(f) = &file {
            command.arg(root.join("resources").join("code").join(f));
        } else {
            let dir = root.join("resources").join("code");
            let mut any = false;
            if let Ok(entries) = fs::read_dir(&dir) {
                for entry in entries.flatten() {
                    if entry.path().extension().and_then(|e| e.to_str()) == Some("cpp") {
                        any = true;
                        command.arg(entry.path());
                    }
                }
            }
            if !any {
                return Ok(json!({ "ok": true, "output": "(no game code)" }));
            }
        }
        let output = command.output().map_err(|e| e.to_string())?;
        let text = format!(
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        Ok(json!({
            "ok": output.status.success(),
            "output": text,
        }))
    })
    .await
    .map_err(|e| e.to_string())?
}

// World flags: run the generator's tooling mode (--flags) and hand the
// JSON through. The bake is the single authority on flag ids, so the
// editor reads this rather than re-deriving the list.
#[tauri::command]
async fn list_flags(state: State<'_, ProjectState>) -> Result<Value, String> {
    let root = project_root(&state)?;
    let engine = engine_root(&state)?;
    tauri::async_runtime::spawn_blocking(move || {
        let game_env = build_env(&engine, &root);
        let output = std::process::Command::new(root_env_python(&root))
            .arg("scripts/generate_assets.py")
            .arg("--flags")
            .current_dir(&engine)
            .envs(game_env)
            .output()
            .map_err(|e| e.to_string())?;
        let text = String::from_utf8_lossy(&output.stdout).to_string();
        let value: Value = serde_json::from_str(&text)
            .map_err(|_| format!("Generator tooling output not JSON: {}", text))?;
        Ok(value)
    })
    .await
    .map_err(|e| e.to_string())?
}

// The project's venv python if present, else the system one.
fn root_env_python(root: &Path) -> String {
    let candidate = root.join(".venv/bin/python3");
    if candidate.is_file() { candidate.to_string_lossy().to_string() }
    else { "python3".to_string() }
}

#[tauri::command]
fn set_project(app: AppHandle, state: State<ProjectState>, path: String) -> Result<Value, String> {
    let dir = PathBuf::from(&path);
    let meta = fs::metadata(&dir).map_err(|_| format!("Cannot open {}", path))?;
    if !meta.is_dir() {
        return Err("Not a folder".to_string());
    }
    if !looks_like_root(&dir) {
        return Err("That folder has no resources/ directory".to_string());
    }
    persist_config(&app, &dir)?;
    *state.0.lock().unwrap() = Some(dir);
    Ok(json!({ "ok": true }))
}

// New game: create a starter folder next to the current project and
// switch to it — a new resources folder is a new game.
#[tauri::command]
fn new_game(app: AppHandle, state: State<ProjectState>, name: String) -> Result<Value, String> {
    let root = project_root(&state)?;
    let parent = root.parent().ok_or("The project has no parent folder")?;
    let created = create_new_game(parent, &name)?;
    let path = created.to_string_lossy().to_string();
    persist_config(&app, &created)?;
    *state.0.lock().unwrap() = Some(created);
    Ok(json!({ "ok": true, "path": path }))
}

// Save-slot tools (the engine writes save<slot>.dat beside the binary on
// desktop targets — see src/main.cpp saveFilePath).
#[tauri::command]
fn save_slots(state: State<'_, ProjectState>) -> Result<Value, String> {
    let engine = engine_root(&state)?;
    let slots: Vec<Value> = (0..3)
        .map(|slot| {
            let path = engine.join(format!("save{}.dat", slot));
            match fs::metadata(&path) {
                Ok(meta) => json!({ "slot": slot, "exists": true, "bytes": meta.len() }),
                Err(_) => json!({ "slot": slot, "exists": false, "bytes": 0 }),
            }
        })
        .collect();
    Ok(json!({ "ok": true, "slots": slots }))
}

#[tauri::command]
fn delete_save(state: State<'_, ProjectState>, slot: i64) -> Result<Value, String> {
    if slot < 0 || slot > 2 {
        return Err("Slot out of range".to_string());
    }
    let engine = engine_root(&state)?;
    let path = engine.join(format!("save{}.dat", slot));
    if path.is_file() {
        fs::remove_file(&path).map_err(|e| e.to_string())?;
    }
    Ok(json!({ "ok": true }))
}

// ---- replace-in-place with rename -------------------------------------------
//
// Renaming an asset (a GIMP/Tracker export landing under a new name, or a
// content rename) must never break a bake: the rename updates every JSON
// reference to the old name — enemy "image" overrides, sfx/music names in
// rooms and dialogue (and items' use sfx, game.json's font), room
// tileset/hero/npc/enemy stem references when a paired sheet+json renames
// together, and dialogue "image" scene cues.

// Recursively replace exact string values (the old file name) in a value.
fn replace_string_values(value: &mut Value, from: &str, to: &str) -> bool {
    let mut changed = false;
    match value {
        Value::String(s) => {
            if s == from {
                *s = to.to_string();
                changed = true;
            }
        }
        Value::Array(items) => {
            for item in items.iter_mut() {
                if replace_string_values(item, from, to) {
                    changed = true;
                }
            }
        }
        Value::Object(map) => {
            for (_, v) in map.iter_mut() {
                if replace_string_values(v, from, to) {
                    changed = true;
                }
            }
        }
        _ => {}
    }
    changed
}

// Bare-stem references, patched only inside the containers that carry
// them (a bare stem matched anywhere else would be a false positive).
fn patch_stem_refs(data: &mut Value, old: &str, fresh: &str, dir: &str) -> bool {
    let mut changed = false;
    if dir == "characters" {
        if let Some(default) = data.get_mut("default").and_then(Value::as_object_mut) {
            if default.get("hero").and_then(Value::as_str) == Some(old) {
                default.insert("hero".into(), json!(fresh));
                changed = true;
            }
        }
        if let Some(list) = data.get_mut("npcs").and_then(Value::as_array_mut) {
            for entry in list.iter_mut() {
                if entry.get("character").and_then(Value::as_str) == Some(old) {
                    entry["character"] = json!(fresh);
                    changed = true;
                }
            }
        }
    } else if dir == "enemies" {
        if let Some(list) = data.get_mut("enemies").and_then(Value::as_array_mut) {
            for entry in list.iter_mut() {
                if entry.get("name").and_then(Value::as_str) == Some(old) {
                    entry["name"] = json!(fresh);
                    changed = true;
                }
            }
        }
    } else if dir == "tiles" {
        if data.get("tileset").and_then(Value::as_str) == Some(old) {
            data["tileset"] = json!(fresh);
            changed = true;
        }
    }
    changed
}

// game.json's boot cards ("logo" / "title_card") take their image by
// file name or stem, like the dialogue image cues; patch the stem form
// right on the fields so renaming a splash never orphans it.
fn patch_game_card_stems(data: &mut Value, old: &str, fresh: &str) -> bool {
    let mut changed = false;
    for card in ["logo", "title_card"] {
        if let Some(image) = data.get_mut(card)
            .and_then(Value::as_object_mut)
            .and_then(|o| o.get_mut("image"))
        {
            if image.as_str() == Some(old) {
                *image = json!(fresh);
                changed = true;
            }
        }
    }
    changed
}

// Dialogue "image" cues accept the file name or the stem; the stem form
// only ever means a scene reference, so patch it right on the field.
fn patch_dialogue_scene_stems(data: &mut Value, old: &str, fresh: &str) -> bool {
    let mut changed = false;
    if let Some(lines) = data.get_mut("lines").and_then(Value::as_array_mut) {
        for line in lines.iter_mut() {
            if line.get("image").and_then(Value::as_str) == Some(old) {
                line["image"] = json!(fresh);
                changed = true;
            }
        }
    }
    changed
}

#[tauri::command]
fn rename_asset(
    state: State<ProjectState>,
    dir: String,
    from: String,
    to: String,
) -> Result<Value, String> {
    const ASSET_DIRS: &[&str] = &["music", "audio", "ui", "tiles", "characters",
        "enemies", "pickups", "effects", "objects", "fonts", "scenes"];
    if !ASSET_DIRS.contains(&dir.as_str()) {
        return Err("Invalid directory".to_string());
    }
    let root = project_root(&state)?;
    let res = root.join("resources");

    let plain_name = |n: &str| {
        Path::new(n).file_name().and_then(|f| f.to_str()).map(|f| f == n) == Some(true)
    };
    if !plain_name(&from) || !plain_name(&to) {
        return Err("Names must be plain file names".to_string());
    }

    let from_path = res.join(&dir).join(&from);
    let to_path = res.join(&dir).join(&to);
    if !from_path.is_file() {
        return Err(format!("{}/{} not found", dir, from));
    }
    if to_path.exists() {
        return Err(format!("{}/{} already exists", dir, to));
    }

    let ext_of = |n: &str| {
        Path::new(n)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("")
            .to_lowercase()
    };
    if ext_of(&from) != ext_of(&to) {
        return Err("Rename must keep the same file type".to_string());
    }
    let stem_of = |n: &str| {
        Path::new(n)
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("")
            .to_string()
    };
    let old_stem = stem_of(&from);
    let new_stem = stem_of(&to);
    if !valid_name(&new_stem) {
        return Err("Invalid name (letters, numbers, spaces, _ and - only)".to_string());
    }

    // Paired sheet+json (characters/enemies/tiles): the definition (or
    // the per-sheet tile metadata) rides along with the sheet, keeping
    // the canonical same-name pairing intact.
    let mut renamed_pair = false;
    if (dir == "characters" || dir == "enemies" || dir == "tiles")
        && ext_of(&from) == "png"
    {
        let json_from = res.join(&dir).join(format!("{}.json", old_stem));
        let json_to = res.join(&dir).join(format!("{}.json", new_stem));
        if json_from.is_file() && !json_to.exists() {
            fs::rename(&json_from, &json_to).map_err(|e| e.to_string())?;
            renamed_pair = true;
        }
    }

    fs::rename(&from_path, &to_path).map_err(|e| e.to_string())?;

    // Reference updates, across the families that can point at the asset.
    let mut updated: Vec<String> = Vec::new();

    for file in list_files(&res, "rooms")
        .into_iter()
        .filter(|f| f.ends_with(".json"))
    {
        let path = res.join("rooms").join(&file);
        let mut data = read_json(&path)?;
        let mut changed = replace_string_values(&mut data, &from, &to);
        changed |= patch_stem_refs(&mut data, &old_stem, &new_stem, &dir);
        if changed {
            fs::write(&path, pretty4(&data)).map_err(|e| e.to_string())?;
            updated.push(format!("rooms/{}", file));
        }
    }

    for file in list_files(&res, "dialogue")
        .into_iter()
        .filter(|f| f.ends_with(".json"))
    {
        let path = res.join("dialogue").join(&file);
        let mut data = read_json(&path)?;
        let mut changed = replace_string_values(&mut data, &from, &to);
        if dir == "scenes" {
            changed |= patch_dialogue_scene_stems(&mut data, &old_stem, &new_stem);
        }
        if changed {
            fs::write(&path, pretty4(&data)).map_err(|e| e.to_string())?;
            updated.push(format!("dialogue/{}", file));
        }
    }

    for (sub, name) in [("items", None::<String>), ("enemies", None), ("objects", Some("types.json".to_string())), ("", Some("game.json".to_string()))] {
        // items/enemies: every json in the folder; objects/game: the one file.
        let files: Vec<String> = match &name {
            Some(single) => {
                let path = if sub.is_empty() { res.join(single) } else { res.join(sub).join(single) };
                if path.is_file() {
                    vec![single.clone()]
                } else {
                    vec![]
                }
            }
            None => list_files(&res, sub)
                .into_iter()
                .filter(|f| f.ends_with(".json"))
                .collect(),
        };
        for file in files {
            let path = if sub.is_empty() { res.join(&file) } else { res.join(sub).join(&file) };
            let Ok(mut data) = read_json(&path) else { continue };
            let mut changed = replace_string_values(&mut data, &from, &to);
            if sub.is_empty() && dir == "scenes" {
                // game.json's boot cards, stem form (the file-name form
                // is covered by the exact match above).
                changed |= patch_game_card_stems(&mut data, &old_stem, &new_stem);
            }
            if changed {
                fs::write(&path, pretty4(&data)).map_err(|e| e.to_string())?;
                updated.push(if sub.is_empty() { file.clone() } else { format!("{}/{}", sub, file) });
            }
        }
    }

    Ok(json!({ "ok": true, "updated": updated, "renamedPair": renamed_pair }))
}

// ---- boot -----------------------------------------------------------------

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let mut root: Option<PathBuf> = None;

            if let Ok(env_path) = std::env::var("LTOP_EDITOR_PROJECT") {
                let dir = PathBuf::from(env_path);
                if looks_like_root(&dir) {
                    root = Some(dir);
                }
            }
            if root.is_none() {
                root = load_config(app.handle());
            }
            if root.is_none() {
                root = find_root(std::env::current_exe().ok().as_deref());
            }
            if root.is_none() {
                root = find_root(std::env::current_dir().ok().as_deref());
            }
            if root.is_none() {
                eprintln!("ltop-editor: no project found; pick one in the app");
            }

            app.manage(ProjectState(Mutex::new(root)));
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            scan,
            save,
            create,
            delete,
            upload,
            build,
            build_web,
            stop_web,
            build_dreamcast,
            play,
            stop_game,
            read_resource,
            set_project,
            new_game,
            save_slots,
            delete_save,
            list_flags,
            check_code,
            run_generator,
            headless_check,
            rename_asset
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}