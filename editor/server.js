// LttP engine resource editor server. Zero dependencies: Node stdlib only.
// Serves the React frontend from editor/public and the game's resources
// (so images render), plus a small JSON API for editing them.

const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const RES = path.join(ROOT, 'resources');
const PUBLIC = path.join(__dirname, 'public');
const PORT = Number(process.env.PORT || 5173);
const DIRECTIONS = ['down', 'right', 'left', 'up'];

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript',
    '.css': 'text/css',
    '.png': 'image/png',
    '.json': 'application/json',
    '.wav': 'audio/wav',
    '.otf': 'font/otf',
    '.xm': 'audio/xm',
    '.mod': 'audio/mod',
    '.tsx': 'text/xml',
    '.tmx': 'text/xml'
};

const UPLOAD_DIRS = {
    music: { dir: path.join(RES, 'music'), exts: ['.xm', '.mod', '.it', '.s3m', '.wav', '.mp3', '.ogg', '.flac'] },
    audio: { dir: path.join(RES, 'audio'), exts: ['.wav', '.mp3', '.ogg', '.flac', '.xm', '.mod'] },
    ui: { dir: path.join(RES, 'ui'), exts: ['.png'] },
    tiles: { dir: path.join(RES, 'tiles'), exts: ['.png', '.tsx'] },
    characters: { dir: path.join(RES, 'characters'), exts: ['.png', '.json'] }
};

function send(res, code, payload) {
    const body = typeof payload === 'string' ? payload : JSON.stringify(payload);
    res.writeHead(code, {
        'Content-Type': typeof payload === 'string' ? 'text/plain; charset=utf-8' : 'application/json',
        'Content-Length': Buffer.byteLength(body)
    });
    res.end(body);
}

function resourcePath(urlPath) {
    if (typeof urlPath !== 'string' || urlPath.includes('..')) return null;
    const full = path.resolve(ROOT, urlPath);
    if (full !== RES && !full.startsWith(RES + path.sep)) return null;
    return full;
}

function readBody(req) {
    return new Promise((resolve, reject) => {
        let size = 0;
        const chunks = [];
        req.on('data', chunk => {
            size += chunk.length;
            if (size > 20 * 1024 * 1024) { reject(new Error('Body too large')); req.destroy(); return; }
            chunks.push(chunk);
        });
        req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
        req.on('error', reject);
    });
}

function readJson(file) {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function tagAttr(tag, name) {
    const match = tag.match(new RegExp(name + '="([^"]+)"'));
    return match ? match[1] : null;
}

function readTmx(file) {
    const text = fs.readFileSync(file, 'utf8');
    const mapTag = (text.match(/<map[^>]*>/) || [''])[0];
    const firstgidMatch = text.match(/<tileset[^>]*firstgid="(\d+)"/);
    const layers = {};
    const re = /<layer[^>]*name="([\w-]+)"[^>]*>\s*<data[^>]*>([\s\S]*?)<\/data>/g;
    let match;
    while ((match = re.exec(text))) {
        layers[match[1]] = match[2].split(',').map(v => parseInt(v.trim(), 10) || 0);
    }
    return {
        width: parseInt(tagAttr(mapTag, 'width'), 10) || 30,
        height: parseInt(tagAttr(mapTag, 'height'), 10) || 20,
        firstgid: firstgidMatch ? parseInt(firstgidMatch[1], 10) : 1,
        layers
    };
}

function tmxTemplate(width, height, decor, colliders) {
    const csv = values => {
        const rows = [];
        for (let y = 0; y < height; y++) {
            const row = [];
            for (let x = 0; x < width; x++) row.push(values[y * width + x] || 0);
            rows.push(row.join(','));
        }
        return rows.join(',\n');
    };
    const layerXml = [['decor', decor], ['colliders', colliders]].map(([name, values], index) => {
        return ` <layer id="${index + 1}" name="${name}" width="${width}" height="${height}">\n  <data encoding="csv">\n${csv(values)}\n</data>\n </layer>`;
    }).join('\n');
    return `<?xml version="1.0" encoding="UTF-8"?>\n<map version="1.10" tiledversion="1.12.2" orientation="orthogonal" renderorder="right-down" width="${width}" height="${height}" tilewidth="32" tileheight="32" infinite="0" nextlayerid="3" nextobjectid="1">\n <tileset firstgid="1" source="../tiles/texture.tsx"/>\n${layerXml}\n</map>\n`;
}

function readTileset() {
    const dir = path.join(RES, 'tiles');
    if (!fs.existsSync(dir)) return null;
    for (const file of fs.readdirSync(dir).sort()) {
        if (!file.endsWith('.tsx')) continue;
        const text = fs.readFileSync(path.join(dir, file), 'utf8');
        const rootTag = (text.match(/<tileset[^>]*>/) || [''])[0];
        const imageTag = (text.match(/<image[^>]*>/) || [''])[0];
        return {
            file,
            image: 'resources/tiles/' + (tagAttr(imageTag, 'source') || ''),
            columns: parseInt(tagAttr(rootTag, 'columns'), 10) || 1,
            tileCount: parseInt(tagAttr(rootTag, 'tilecount'), 10) || 0,
            tileWidth: parseInt(tagAttr(rootTag, 'tilewidth'), 10) || 32,
            tileHeight: parseInt(tagAttr(rootTag, 'tileheight'), 10) || 32
        };
    }
    return null;
}

function listFiles(dirName) {
    const dir = path.join(RES, dirName);
    if (!fs.existsSync(dir)) return [];
    return fs.readdirSync(dir)
        .filter(f => f !== '.DS_Store' && fs.statSync(path.join(dir, f)).isFile())
        .sort();
}

function scan() {
    const jsonFiles = dir => listFiles(dir).filter(f => f.endsWith('.json'));

    const dialogues = jsonFiles('dialogue').map(file => ({
        file,
        data: readJson(path.join(RES, 'dialogue', file))
    }));

    const characters = jsonFiles('characters').map(file => ({
        file,
        data: readJson(path.join(RES, 'characters', file)),
        png: fs.existsSync(path.join(RES, 'characters', file.replace(/\.json$/, '.png')))
    }));

    const rooms = jsonFiles('rooms').map(file => {
        const stem = file.replace(/\.json$/, '');
        const tmxPath = path.join(RES, 'rooms', stem + '.tmx');
        const hasTmx = fs.existsSync(tmxPath);
        const tmx = hasTmx ? readTmx(tmxPath) : null;
        const data = readJson(path.join(RES, 'rooms', file));
        return {
            file,
            tmx: hasTmx ? 'resources/rooms/' + stem + '.tmx' : null,
            firstgid: tmx ? tmx.firstgid : 1,
            mapWidth: tmx ? tmx.width : 30,
            mapHeight: tmx ? tmx.height : 20,
            default: data.default || {},
            encounters: data.encounters || {},
            decor: (tmx && tmx.layers.decor) || [],
            colliders: (tmx && tmx.layers.colliders) || []
        };
    });

    return {
        platform: process.platform,
        tileset: readTileset(),
        dialogues,
        characters,
        rooms,
        music: listFiles('music'),
        audio: listFiles('audio'),
        ui: listFiles('ui')
    };
}

function saveHandler(payload, res) {
    const target = resourcePath(payload.path);
    if (!target) return send(res, 400, { error: 'Invalid path' });
    if (!/\.(json|tmx)$/.test(target)) return send(res, 400, { error: 'Only .json and .tmx files can be saved' });
    fs.writeFileSync(target, payload.content || '');
    send(res, 200, { ok: true });
}

function validName(name) {
    return /^[A-Za-z0-9][A-Za-z0-9 _-]*$/.test(name);
}

function createHandler(payload, res) {
    const name = String(payload.name || '').trim();
    if (!validName(name)) return send(res, 400, { error: 'Invalid name (letters, numbers, spaces, _ and - only)' });

    if (payload.type === 'dialogue') {
        const file = name.endsWith('.json') ? name : name + '.json';
        const target = path.join(RES, 'dialogue', file);
        if (fs.existsSync(target)) return send(res, 400, { error: 'Already exists' });
        fs.writeFileSync(target, JSON.stringify({ lines: [] }, null, 4));
        return send(res, 200, { ok: true });
    }

    if (payload.type === 'character') {
        const file = name.endsWith('.json') ? name : name + '.json';
        const target = path.join(RES, 'characters', file);
        if (fs.existsSync(target)) return send(res, 400, { error: 'Already exists' });
        const skeleton = { width: 4 };
        for (const kind of ['stand', 'move']) {
            skeleton[kind] = {};
            for (const dir of DIRECTIONS) skeleton[kind][dir] = [];
        }
        fs.writeFileSync(target, JSON.stringify(skeleton, null, 4));
        return send(res, 200, { ok: true });
    }

    if (payload.type === 'room') {
        const stem = name.replace(/\.json$/, '');
        if (!validName(stem)) return send(res, 400, { error: 'Invalid name' });
        const width = Math.max(1, Math.min(500, parseInt(payload.width, 10) || 30));
        const height = Math.max(1, Math.min(500, parseInt(payload.height, 10) || 20));
        const jsonPath = path.join(RES, 'rooms', stem + '.json');
        const tmxPath = path.join(RES, 'rooms', stem + '.tmx');
        if (fs.existsSync(jsonPath) || fs.existsSync(tmxPath)) return send(res, 400, { error: 'Already exists' });
        const roomJson = {
            default: { hero: '', spawn: [0, 0], music: '', walk_sfx: '' },
            encounters: { instant: [], interactive: [] }
        };
        fs.writeFileSync(jsonPath, JSON.stringify(roomJson, null, 4));
        fs.writeFileSync(tmxPath, tmxTemplate(width, height, [], []));
        return send(res, 200, { ok: true });
    }

    send(res, 400, { error: 'Unknown type' });
}

function deleteHandler(payload, res) {
    const target = resourcePath(payload.path);
    if (!target) return send(res, 400, { error: 'Invalid path' });
    if (!fs.existsSync(target) || !fs.statSync(target).isFile()) return send(res, 400, { error: 'Not a file' });
    fs.unlinkSync(target);
    send(res, 200, { ok: true });
}

function uploadHandler(payload, res) {
    const entry = UPLOAD_DIRS[payload.dir];
    if (!entry) return send(res, 400, { error: 'Invalid directory' });
    if (!payload.name || typeof payload.data !== 'string') return send(res, 400, { error: 'Invalid upload' });
    const name = path.basename(String(payload.name));
    const ext = path.extname(name).toLowerCase();
    if (!entry.exts.includes(ext)) return send(res, 400, { error: 'File type not allowed here (' + ext + ')' });
    fs.mkdirSync(entry.dir, { recursive: true });
    fs.writeFileSync(path.join(entry.dir, name), Buffer.from(payload.data, 'base64'));
    send(res, 200, { ok: true });
}

function buildHandler(res) {
    if (process.platform !== 'darwin') {
        return send(res, 200, { ok: false, error: 'build-mac.sh only runs on Mac' });
    }
    const child = spawn('/bin/bash', ['./build-mac.sh'], { cwd: ROOT });
    let output = '';
    child.stdout.on('data', chunk => { output += chunk; });
    child.stderr.on('data', chunk => { output += chunk; });
    const timer = setTimeout(() => child.kill(), 300000);
    child.on('error', err => {
        clearTimeout(timer);
        send(res, 200, { ok: false, output: output + String(err) });
    });
    child.on('close', code => {
        clearTimeout(timer);
        send(res, 200, { ok: code === 0, code, output });
    });
}

function serveStatic(pathname, res) {
    if (pathname === '/api/scan') return send(res, 200, scan());

    let file = null;
    if (pathname === '/' || pathname === '/index.html') {
        file = path.join(PUBLIC, 'index.html');
    } else if (pathname.startsWith('/resources/')) {
        const target = resourcePath(pathname.slice(1));
        if (target && fs.existsSync(target) && fs.statSync(target).isFile()) file = target;
    } else if (!pathname.includes('..')) {
        const target = path.normalize(path.join(PUBLIC, pathname));
        if (target.startsWith(PUBLIC) && fs.existsSync(target) && fs.statSync(target).isFile()) file = target;
    }

    if (!file) return send(res, 404, 'Not found');
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
}

const server = http.createServer(async (req, res) => {
    try {
        const url = new URL(req.url, 'http://localhost');
        if (req.method === 'GET') return serveStatic(url.pathname, res);
        if (req.method !== 'POST') return send(res, 405, { error: 'Method not allowed' });

        const payload = JSON.parse((await readBody(req)) || '{}');
        if (url.pathname === '/api/save') return saveHandler(payload, res);
        if (url.pathname === '/api/create') return createHandler(payload, res);
        if (url.pathname === '/api/delete') return deleteHandler(payload, res);
        if (url.pathname === '/api/upload') return uploadHandler(payload, res);
        if (url.pathname === '/api/build') return buildHandler(res);
        send(res, 404, { error: 'Not found' });
    } catch (err) {
        send(res, 500, { error: String((err && err.message) || err) });
    }
});

server.listen(PORT, () => {
    console.log('LttP resource editor: http://localhost:' + PORT);
});
