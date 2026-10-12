import { invoke } from '@tauri-apps/api/core';

// Maps the editor's old HTTP API onto Tauri IPC commands.
const COMMANDS = {
  '/api/scan': () => ['scan', {}],
  '/api/save': b => ['save', { path: b.path, content: b.content }],
  '/api/create': b => ['create', { kind: b.type, name: b.name, width: b.width, height: b.height }],
  '/api/delete': b => ['delete', { path: b.path }],
  '/api/upload': b => ['upload', { dir: b.dir, name: b.name, data: b.data }],
  '/api/build': () => ['build', {}],
  '/api/set_project': b => ['set_project', { path: b.path }]
};

export async function api(url, body) {
  const entry = COMMANDS[url];
  if (!entry) throw new Error('Unknown API endpoint: ' + url);
  const [command, args] = entry(body || {});
  try {
    return await invoke(command, args);
  } catch (err) {
    return { error: typeof err === 'string' ? err : String(err) };
  }
}

// Fetch a resource file (relative to the project root, e.g.
// "resources/tiles/texture.png") as a data URL the webview can display.
export async function resourceUrl(relPath) {
  const { mime, data } = await invoke('read_resource', { path: relPath });
  return 'data:' + mime + ';base64,' + data;
}