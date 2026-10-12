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

struct ProjectState(Mutex<Option<PathBuf>>);

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
fn read_tileset(res: &Path) -> Option<Value> {
    for file in list_files(res, "tiles") {
        if !file.ends_with(".png") {
            continue;
        }
        let (width, height) = png_size(&res.join("tiles").join(&file))?;
        let columns = (width / 32) as u64;
        if columns < 1 {
            continue;
        }
        return Some(json!({
            "file": file,
            "image": format!("resources/tiles/{}", file),
            "columns": columns,
            "tileCount": columns * (height / 32) as u64,
            "tileWidth": 32,
            "tileHeight": 32,
        }));
    }
    None
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

    Ok(json!({
        "project": root.to_string_lossy(),
        "platform": platform,
        "tileset": read_tileset(&res),
        "dialogues": dialogues,
        "characters": characters,
        "rooms": rooms,
        "music": list_files(&res, "music"),
        "audio": list_files(&res, "audio"),
        "ui": list_files(&res, "ui"),
    }))
}

#[tauri::command]
fn save(state: State<ProjectState>, path: String, content: String) -> Result<Value, String> {
    let root = project_root(&state)?;
    let target = resolve_resource(&root, &path)?;
    match target.extension().and_then(|e| e.to_str()) {
        Some("json") => {}
        _ => return Err("Only .json files can be saved".to_string()),
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
            let doc = json!({
                "width": 4,
                "stand": Value::Object(empty_directions()),
                "move": Value::Object(empty_directions()),
            });
            fs::write(&target, pretty4(&doc)).map_err(|e| e.to_string())?;
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

fn run_build(root: PathBuf) -> Result<Value, String> {
    if !cfg!(target_os = "macos") {
        return Ok(json!({ "ok": false, "error": "build-mac.sh only runs on Mac" }));
    }
    let mut child = std::process::Command::new("/bin/bash")
        .arg("./build-mac.sh")
        .current_dir(&root)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;

    let output: Arc<Mutex<String>> = Arc::new(Mutex::new(String::new()));
    spawn_pipe_reader(child.stdout.take(), output.clone());
    spawn_pipe_reader(child.stderr.take(), output.clone());

    let start = Instant::now();
    let code = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status.code(),
            Ok(None) => {
                if start.elapsed() >= BUILD_TIMEOUT {
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
    tauri::async_runtime::spawn_blocking(move || run_build(root))
        .await
        .map_err(|e| e.to_string())?
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
    let dir = dir.canonicalize().unwrap_or(dir);
    if let Err(e) = persist_config(&app, &dir) {
        eprintln!("ltop-editor: could not persist project choice: {}", e);
    }
    *state.0.lock().unwrap() = Some(dir);
    Ok(json!({ "ok": true }))
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
            read_resource,
            set_project
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}