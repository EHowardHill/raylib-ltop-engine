import { useState, useEffect, useRef, useCallback, useContext, createContext } from 'react';
import { createRoot } from 'react-dom/client';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import { api, resourceUrl, resourceText } from './api.js';
import './styles.css';

const stem = file => file.replace(/\.json$/, '');
const DIRECTIONS = ['down', 'right', 'left', 'up'];

// ---- confirm dialog (no native window.confirm inside Tauri) ------------

const ConfirmContext = createContext(() => {});

function ConfirmProvider({ children }) {
    const [req, setReq] = useState(null);
    const confirm = useCallback(message => new Promise(resolve => setReq({ message, resolve })), []);
    return (
        <ConfirmContext.Provider value={confirm}>
            {children}
            {req && (
                <div className="modalOverlay" onClick={() => { setReq(null); req.resolve(false); }}>
                    <div className="modal" onClick={e => e.stopPropagation()}>
                        <h1>{req.message}</h1>
                        <div className="row">
                            <button className="btn primary" onClick={() => { setReq(null); req.resolve(true); }}>Delete</button>
                            <button className="btn" onClick={() => { setReq(null); req.resolve(false); }}>Cancel</button>
                        </div>
                    </div>
                </div>
            )}
        </ConfirmContext.Provider>
    );
}

const useConfirm = () => useContext(ConfirmContext);

// ---- project picker ------------------------------------------------------

function useProject() {
    const [error, setError] = useState('');
    const pick = async onPicked => {
        const path = await openDialog({ directory: true, title: 'Choose the engine repository' });
        if (!path) return;
        const result = await api('/api/set_project', { path });
        if (result.error) return setError(result.error);
        setError('');
        onPicked();
    };
    return { pick, error };
}

function ProjectPicker({ onPicked }) {
    const { pick, error } = useProject();
    return (
        <div style={{ padding: 40 }}>
            <h1>No project open</h1>
            <p className="dim">Choose the engine repository folder — the one that contains <code>resources/</code>.</p>
            <div className="row" style={{ marginTop: 10 }}>
                <button className="btn primary" onClick={() => pick(onPicked)}>Open Project Folder…</button>
            </div>
            {error && <p className="fail">{error}</p>}
        </div>
    );
}

function App() {
    const [scan, setScan] = useState(null);
    const [tab, setTab] = useState('rooms');
    const refresh = useCallback(() => api('/api/scan').then(setScan), []);
    useEffect(() => { refresh(); }, []);

    if (!scan) return <p className="dim">Loading…</p>;
    if (scan.error) return (
        <div style={{ padding: 40 }}>
            <h1>Scan failed</h1>
            <p className="fail">{scan.error}</p>
            <button className="btn" onClick={refresh}>Retry</button>
        </div>
    );
    if (!scan.project) return <ProjectPicker onPicked={refresh} />;

    const tabs = [
        ['file', 'File'],
        ['rooms', 'Rooms'], ['enemies', 'Enemies'], ['items', 'Items'],
        ['tiles', 'Tiles'], ['objects', 'Object Types'], ['dialogue', 'Dialogue'], ['characters', 'Characters'],
        ['code', 'Code'], ['flags', 'Flags'], ['files', 'Music & Files'], ['build', 'Build']
    ];

    return (
        <ConfirmProvider>
            <div id="wrap" style={{ display: 'flex', width: '100%' }}>
                <nav>
                    <div style={{ color: '#fff', fontWeight: 'bold', padding: '6px 10px 14px' }}>LttP Editor</div>
                    {tabs.map(([key, label]) =>
                        <button key={key}
                            className={(tab === key ? 'active ' : '') + (key === 'file' ? 'file' : '')}
                            onClick={() => setTab(key)}>{label}</button>)}
                </nav>
                    <main>
                    {tab === 'file' && <FileTab scan={scan} refresh={refresh} />}
                    {tab === 'rooms' && <RoomsTab scan={scan} refresh={refresh} />}
                    {tab === 'enemies' && <EnemiesTab scan={scan} refresh={refresh} />}
                    {tab === 'items' && <ItemsTab scan={scan} refresh={refresh} />}
                    {tab === 'tiles' && <TilesTab scan={scan} refresh={refresh} />}
                    {tab === 'objects' && <ObjectTypesTab scan={scan} refresh={refresh} />}
                    {tab === 'dialogue' && <DialogueTab scan={scan} refresh={refresh} />}
                    {tab === 'flags' && <FlagsTab />}
                    {tab === 'characters' && <CharactersTab scan={scan} refresh={refresh} />}
                    {tab === 'code' && <CodeTab scan={scan} refresh={refresh} />}
                    {tab === 'files' && <FilesTab scan={scan} refresh={refresh} />}
                    {tab === 'build' && <BuildTab scan={scan} refresh={refresh} />}
                    </main>
            </div>
        </ConfirmProvider>
    );
}

// The File tab: every project-level meta action lives here — open/switch
// game, new game, game identity, save-slot tools, and one-click
// build-and-play. The engine's working tabs stay below it.
function FileTab({ scan, refresh }) {
    const { pick, error: pickError } = useProject();
    const confirm = useConfirm();
    const [newName, setNewName] = useState('');
    const [newError, setNewError] = useState('');
    const [slots, setSlots] = useState(null);
    const [slotStatus, setSlotStatus] = useState('');
    const [playing, setPlaying] = useState(false);
    const [playStatus, setPlayStatus] = useState('');

    const reloadSlots = useCallback(
        () => api('/api/save_slots').then(r => setSlots(r.slots || [])),
        []);
    useEffect(() => { reloadSlots(); }, [reloadSlots]);

    const createGame = async () => {
        if (!newName.trim()) return;
        const result = await api('/api/new_game', { name: newName.trim() });
        if (result.error) return setNewError(result.error);
        setNewName('');
        setNewError('');
        refresh();
    };

    const deleteSlot = async slot => {
        if (!await confirm('Delete save slot ' + slot + '? This save is gone for good.')) return;
        await api('/api/delete_save', { slot });
        setSlotStatus('Deleted slot ' + slot);
        reloadSlots();
    };

    const makeAndPlay = async () => {
        setPlaying(true);
        setPlayStatus('Building…');
        const result = await api('/api/play', { room: '' });
        setPlaying(false);
        setPlayStatus(result.error ? result.error
            : result.ok ? 'Running — enjoy (saves are live).'
            : (result.output || 'Build failed.'));
    };

    return (
        <div>
            <h1>File</h1>
            <div className="panel" style={{ maxWidth: 520 }}>
                <h2>Current Game</h2>
                <div className="dim" style={{ wordBreak: 'break-all' }}>{scan.project}</div>
                <div className="row" style={{ marginTop: 8 }}>
                    <button className="btn" onClick={() => pick(refresh)}>Open / Switch Game…</button>
                </div>
                {pickError && <p className="fail">{pickError}</p>}
                <div style={{ marginTop: 14 }}>
                    <div className="dim">New game (a folder beside this one):</div>
                    <div className="row" style={{ marginTop: 4 }}>
                        <input placeholder="name" value={newName} style={{ width: 120 }}
                            onChange={e => setNewName(e.target.value)} />
                        <button className="btn" onClick={createGame}>Create</button>
                    </div>
                    {newError && <p className="fail">{newError}</p>}
                </div>
            </div>
            <div style={{ marginTop: 20 }}>
                <GameIdentity scan={scan} onSaved={refresh} />
            </div>
            <div className="panel" style={{ maxWidth: 520, marginTop: 20 }}>
                <h2>Save Slots</h2>
                <p className="dim">The engine writes save&lt;slot&gt;.dat beside the binary (the title screen picks a slot).</p>
                {!slots && <p className="dim">(loading…)</p>}
                {slots && slots.map(s => (
                    <div className="row" key={s.slot} style={{ marginTop: 4 }}>
                        <span style={{ minWidth: 120 }}>Slot {s.slot}</span>
                        {s.exists
                            ? <span className="dim">used ({s.bytes} bytes)</span>
                            : <span className="dim">empty</span>}
                        {s.exists && <button className="delete" onClick={() => deleteSlot(s.slot)}>×</button>}
                    </div>
                ))}
                {slotStatus && <p className="dim" style={{ marginTop: 6 }}>{slotStatus}</p>}
            </div>
            <div className="panel" style={{ maxWidth: 520, marginTop: 20 }}>
                <h2>Make &amp; Play</h2>
                <p className="dim">Builds the game and runs it from the title screen (full game, real saves).</p>
                <div className="row">
                    <button className="btn primary" onClick={makeAndPlay} disabled={playing}>
                        {playing ? 'Building…' : 'Build & Play'}
                    </button>
                </div>
                {playStatus && <p className="dim" style={{ marginTop: 6, whiteSpace: 'pre-wrap' }}>{playStatus}</p>}
            </div>
        </div>
    );
}

const TILE_TOOLS = [
    ['paint', 'Paint', 'B'],
    ['erase', 'Erase', 'E'],
    ['fill', 'Fill', 'G'],
    ['rect', 'Rect', 'R'],
    ['pick', 'Pick', 'I']
];
const MARKER_TOOLS = [
    ['select', 'Select', 'V'],
    ['spawn', 'Spawn', 'S'],
    ['instant', 'Place Instant', 'N'],
    ['interactive', 'Place Interactive', 'M'],
    ['object', 'Place Object', 'O'],
    ['enemy', 'Place Enemy', 'K'],
    ['npc', 'Place NPC', 'P'],
    ['pickup', 'Place Pickup', 'U']
];
const TOOL_HINTS = {
    paint: 'Left-drag paints the active layer. Right-drag erases.',
    erase: 'Left-drag erases tiles from the active layer.',
    fill: 'Click to flood-fill a region of the active layer. Right-click fills with empty.',
    rect: 'Drag a rectangle to fill it on the active layer. Right-drag fills with empty.',
    pick: 'Click a tile to grab it into the palette.',
    select: 'Click or drag the spawn / encounter / entity markers to move them. Del removes the selection.',
    spawn: 'Click a tile to place the hero spawn.',
    instant: 'Click a tile to add an instant encounter.',
    interactive: 'Click a tile to add an interactive (action-button) encounter.',
    object: 'Click a tile to place the chosen object type (chest, block, warp...).',
    enemy: 'Click a tile to place the chosen enemy.',
    npc: 'Click a tile to place the chosen character as an NPC.',
    pickup: 'Click a tile to lay the chosen pickup (toggle "buried" for shovel-revealed items).'
};
const ENCOUNTER_COLORS = { instant: '#f97316', interactive: '#3b82f6' };
const ENTITY_COLORS = { object: '#a855f7', enemy: '#ef4444', npc: '#22d3ee', pickup: '#84cc16' };
const ENTITY_KINDS = ['object', 'enemy', 'npc', 'pickup'];
const ENTITY_LABELS = { object: 'Objects', enemy: 'Enemies', npc: 'NPCs', pickup: 'Pickups' };
// Chest grant vocabulary (mirrors the generator's GRANT_KINDS; the editor
// can't run the bake, so the stable engine list lives here).
const GRANT_KINDS = ['item', 'rupees', 'arrows', 'bombs', 'magic', 'heart_container',
    'flag', 'key', 'big_key', 'bottle', 'potion_red', 'potion_green', 'potion_blue'];

function RoomEditor({ room, scan, onSaved }) {
    // The room's tileset (shared sheets): its named reference, else the
    // game's default. The map and palette render from this sheet, and the
    // picker swaps it live so the layers' ids re-preview immediately.
    const tilesets = scan.tilesets || [];
    const defaultTileset = tilesets[scan.defaultTileset || 0];
    const [tilesetChoice, setTilesetChoice] = useState(room.raw.tileset || '');
    const tileset = tilesets.find(t => t.file === (tilesetChoice || (defaultTileset || {}).file))
        || defaultTileset || { image: '', columns: 1, tileCount: 0, tileWidth: 32, tileHeight: 32 };
    const cols = tileset.columns || 1;
    const ts = tileset.tileWidth || 32;

    const [size, setSize] = useState({ w: room.mapWidth, h: room.mapHeight });
    const [def, setDef] = useState(room.default || {});
    const [enc, setEnc] = useState({ instant: [], interactive: [], ...(room.encounters || {}) });
    // Entities (objects, enemies, NPCs): placed, dragged, and edited on
    // the map like the encounter markers; saved into the room's lists.
    const [objects, setObjects] = useState((room.raw.objects || []).slice());
    const [enemies, setEnemies] = useState((room.raw.enemies || []).slice());
    const [npcs, setNpcs] = useState((room.raw.npcs || []).slice());
    // Pickups: one editing list carrying the shovel flag; saved split into
    // the room's "pickups" and "buried" lists (same entry shape).
    const [pickups, setPickups] = useState(
        (room.raw.pickups || []).map(e => ({ ...e }))
            .concat((room.raw.buried || []).map(e => ({ ...e, buried: true }))));
    const [placePickup, setPlacePickup] = useState(PICKUP_KINDS[0]);
    const [placeBuried, setPlaceBuried] = useState(false);
    // World links: edge doors, map-graph exits, the mirror pair, and the
    // pit-landing — the room's connections to everything else.
    const [doors, setDoors] = useState({ ...(room.raw.doors || {}) });
    const [exits, setExits] = useState({ ...(room.raw.exits || {}) });
    const [mirror, setMirror] = useState(room.raw.mirror || '');
    const [pit, setPit] = useState({ ...(room.raw.pit || {}) });
    const [placeType, setPlaceType] = useState((scan.objectTypes && scan.objectTypes[0]) ? scan.objectTypes[0].name : 'pot');
    const [placeEnemy, setPlaceEnemy] = useState((scan.enemies && scan.enemies[0]) ? scan.enemies[0].name : '');
    const [placeCharacter, setPlaceCharacter] = useState(scan.characters[0] ? stem(scan.characters[0].file) : '');
    const [layers, setLayers] = useState({
        decor: room.decor.slice(),
        colliders: room.colliders.slice(),
        upper: (room.upper || []).slice()
    });
    const [layer, setLayer] = useState('decor');
    const [vis, setVis] = useState({ decor: true, colliders: true, upper: true });
    const [gid, setGid] = useState(1);
    const [tool, setTool] = useState('paint');
    const [showGrid, setShowGrid] = useState(true);
    const [zoom, setZoom] = useState(1);
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);
    const [hover, setHover] = useState(null);
    const [rectStart, setRectStart] = useState(null);
    const [selected, setSelected] = useState(null);
    const [img, setImg] = useState(null);
    const [newW, setNewW] = useState(room.mapWidth);
    const [newH, setNewH] = useState(room.mapHeight);
    const canvasRef = useRef(null);
    const scrollRef = useRef(null);
    const drag = useRef(null);

    const W = size.w, H = size.h;

    useEffect(() => {
        let alive = true;
        if (!tileset.image) return () => { alive = false; };
        resourceUrl(tileset.image).then(src => {
            if (!alive) return;
            const image = new Image();
            image.onload = () => setImg(image);
            image.src = src;
        }).catch(() => {});
        return () => { alive = false; };
    }, [tileset.image]);

    // Fit the map into the viewport once on mount.
    useEffect(() => {
        const el = scrollRef.current;
        if (!el) return;
        const fit = Math.min((el.clientWidth - 24) / (W * ts), (el.clientHeight - 24) / (H * ts), 1);
        setZoom(Math.max(0.25, Math.round(fit * 4) / 4));
    }, []);

    // Ctrl/cmd + wheel zooms the map; a plain wheel keeps scrolling it.
    // (Native listener: React's wheel handler is passive and can't block
    // the browser's pinch-zoom gesture.)
    useEffect(() => {
        const el = scrollRef.current;
        if (!el) return;
        const onWheel = e => {
            if (!(e.ctrlKey || e.metaKey)) return;
            e.preventDefault();
            const factor = e.deltaY < 0 ? 1.25 : 0.8;
            setZoom(z => Math.max(0.25, Math.min(4, Math.round(z * factor * 4) / 4)));
        };
        el.addEventListener('wheel', onWheel, { passive: false });
        return () => el.removeEventListener('wheel', onWheel);
    }, []);

    // Keyboard shortcuts (ignored while typing in a field).
    useEffect(() => {
        const onKey = e => {
            const tag = (e.target && e.target.tagName) || '';
            if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
            const key = e.key.toLowerCase();
            const keys = { b: 'paint', e: 'erase', g: 'fill', r: 'rect', i: 'pick', v: 'select', s: 'spawn', n: 'instant', m: 'interactive', o: 'object', k: 'enemy', p: 'npc', u: 'pickup' };
            if (keys[key]) { setTool(keys[key]); return; }
            if (key === 'escape') { setSelected(null); setRectStart(null); return; }
            if ((key === 'delete' || key === 'backspace') && selected && selected !== 'spawn') {
                removeEntity(selected);
                setSelected(null);
            }
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [selected]);

    // ---- rendering -------------------------------------------------------

    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#3a3a44';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        const drawLayer = (values, isCollider) => {
            for (let i = 0; i < values.length; i++) {
                const id = values[i];
                if (!id) continue;
                const x = (i % W) * ts, y = Math.floor(i / W) * ts;
                const frame = id - 1;
                if (frame < 0 || frame >= tileset.tileCount) {
                    ctx.fillStyle = '#e11d48';
                    ctx.fillRect(x, y, ts, ts);
                    continue;
                }
                ctx.drawImage(img, (frame % cols) * ts, Math.floor(frame / cols) * ts, ts, ts, x, y, ts, ts);
                if (isCollider) {
                    ctx.fillStyle = 'rgba(239,68,68,0.38)';
                    ctx.fillRect(x, y, ts, ts);
                    ctx.strokeStyle = 'rgba(239,68,68,0.9)';
                    ctx.lineWidth = 2;
                    ctx.strokeRect(x + 1, y + 1, ts - 2, ts - 2);
                }
            }
        };

        if (vis.decor) drawLayer(layers.decor, false);
        if (vis.colliders) drawLayer(layers.colliders, true);
        // Upper layer paints last: over the hero when walking under it.
        if (vis.upper) drawLayer(layers.upper, false);

        if (showGrid && zoom >= 0.5) {
            ctx.strokeStyle = 'rgba(255,255,255,0.10)';
            ctx.lineWidth = 1;
            for (let x = 0; x <= W; x++) { ctx.beginPath(); ctx.moveTo(x * ts + 0.5, 0); ctx.lineTo(x * ts + 0.5, H * ts); ctx.stroke(); }
            for (let y = 0; y <= H; y++) { ctx.beginPath(); ctx.moveTo(0, y * ts + 0.5); ctx.lineTo(W * ts, y * ts + 0.5); ctx.stroke(); }
        }

        if (rectStart && hover) {
            const x = Math.min(rectStart[0], hover[0]) * ts, y = Math.min(rectStart[1], hover[1]) * ts;
            const w = (Math.abs(hover[0] - rectStart[0]) + 1) * ts, h = (Math.abs(hover[1] - rectStart[1]) + 1) * ts;
            ctx.fillStyle = 'rgba(250,204,21,0.25)';
            ctx.fillRect(x, y, w, h);
            ctx.strokeStyle = '#facc15';
            ctx.lineWidth = 2;
            ctx.strokeRect(x + 1, y + 1, w - 2, h - 2);
        }

        if (hover && TILE_TOOLS.some(t => t[0] === tool)) {
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 2;
            ctx.strokeRect(hover[0] * ts + 1, hover[1] * ts + 1, ts - 2, ts - 2);
        }

        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';

        if (def.spawn) {
            const x = def.spawn[0] * ts, y = def.spawn[1] * ts;
            ctx.fillStyle = 'rgba(34,197,94,0.8)';
            ctx.fillRect(x + 4, y + 4, ts - 8, ts - 8);
            if (selected === 'spawn') { ctx.strokeStyle = '#fff'; ctx.lineWidth = 3; ctx.strokeRect(x + 2, y + 2, ts - 4, ts - 4); }
            ctx.fillStyle = '#fff';
            ctx.font = 'bold 15px sans-serif';
            ctx.fillText('S', x + ts / 2, y + ts / 2 + 1);
        }

        for (const kind of ['instant', 'interactive']) {
            (enc[kind] || []).forEach((entry, i) => {
                if (!entry.location) return;
                const cx = entry.location[0] * ts + ts / 2, cy = entry.location[1] * ts + ts / 2;
                ctx.beginPath();
                ctx.arc(cx, cy, 10, 0, Math.PI * 2);
                ctx.fillStyle = ENCOUNTER_COLORS[kind];
                ctx.fill();
                ctx.lineWidth = 2;
                const isSelected = selected && selected !== 'spawn' && selected.kind === kind && selected.index === i;
                ctx.strokeStyle = isSelected ? '#fff' : 'rgba(0,0,0,0.45)';
                ctx.stroke();
                ctx.fillStyle = '#fff';
                ctx.font = 'bold 12px sans-serif';
                ctx.fillText(kind === 'instant' ? '!' : 'A', cx, cy + 1);
            });
        }

        // Entity markers (objects, enemies, NPCs, pickups): colored squares
        // with two-letter type initials (buried pickups draw dashed).
        const entityList = kind => kind === 'object' ? objects : kind === 'enemy' ? enemies
            : kind === 'npc' ? npcs : pickups;
        for (const kind of ENTITY_KINDS) {
            entityList(kind).forEach((entry, i) => {
                if (!entry.location) return;
                const x = entry.location[0] * ts, y = entry.location[1] * ts;
                ctx.fillStyle = ENTITY_COLORS[kind];
                ctx.fillRect(x + 3, y + 3, ts - 6, ts - 6);
                const isSelected = selected && selected !== 'spawn' && selected.kind === kind && selected.index === i;
                ctx.lineWidth = 2;
                ctx.strokeStyle = isSelected ? '#fff' : 'rgba(0,0,0,0.5)';
                ctx.strokeRect(x + 3, y + 3, ts - 6, ts - 6);
                ctx.fillStyle = '#fff';
                ctx.font = 'bold 12px sans-serif';
                const label = kind === 'object'
                    ? (entry.type || '??').slice(0, 2)
                    : kind === 'enemy' ? (entry.name || '?').slice(0, 2)
                    : kind === 'pickup' ? ((entry.buried ? 'b:' : '') + (entry.kind || '?')).slice(0, 3)
                    : (entry.character || '?').slice(0, 2);
                ctx.fillText(label.toUpperCase(), x + ts / 2, y + ts / 2 + 1);
            });
        }
    }, [img, layers, vis, def, enc, objects, enemies, npcs, pickups, hover, tool, rectStart, selected, showGrid, zoom, W, H]);

    // ---- map interaction -------------------------------------------------

    const tileFromEvent = e => {
        const canvas = canvasRef.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / W));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / H));
        return (x >= 0 && y >= 0 && x < W && y < H) ? [x, y] : null;
    };

    const setTile = (index, value) => {
        setLayers(prev => {
            if (prev[layer][index] === value) return prev;
            const copy = prev[layer].slice();
            copy[index] = value;
            return { ...prev, [layer]: copy };
        });
        setDirty(true);
    };

    const floodFill = (index, value) => {
        setLayers(prev => {
            const src = prev[layer];
            const target = src[index];
            if (target === value) return prev;
            const copy = src.slice();
            const stack = [index];
            while (stack.length) {
                const i = stack.pop();
                if (i < 0 || i >= copy.length || copy[i] !== target) continue;
                copy[i] = value;
                const x = i % W;
                if (x > 0) stack.push(i - 1);
                if (x < W - 1) stack.push(i + 1);
                stack.push(i - W, i + W);
            }
            return { ...prev, [layer]: copy };
        });
        setDirty(true);
    };

    const applyRect = (a, b, value) => {
        setLayers(prev => {
            const copy = prev[layer].slice();
            for (let y = Math.min(a[1], b[1]); y <= Math.max(a[1], b[1]); y++)
                for (let x = Math.min(a[0], b[0]); x <= Math.max(a[0], b[0]); x++)
                    copy[y * W + x] = value;
            return { ...prev, [layer]: copy };
        });
        setDirty(true);
    };

    const entityList = kind => kind === 'object' ? objects : kind === 'enemy' ? enemies
        : kind === 'npc' ? npcs : pickups;
    const setEntityList = (kind, value) => {
        if (kind === 'object') setObjects(value);
        else if (kind === 'enemy') setEnemies(value);
        else if (kind === 'npc') setNpcs(value);
        else setPickups(value);
    };

    const removeEntity = marker => {
        // Functional updates: the keyboard shortcut's effect may hold an
        // older render's lists, so deletes never race fresh placements.
        if (marker.kind === 'object') setObjects(prev => prev.filter((_, j) => j !== marker.index));
        else if (marker.kind === 'enemy') setEnemies(prev => prev.filter((_, j) => j !== marker.index));
        else if (marker.kind === 'npc') setNpcs(prev => prev.filter((_, j) => j !== marker.index));
        else setPickups(prev => prev.filter((_, j) => j !== marker.index));
        setDirty(true);
    };

    const markerAt = tile => {
        if (!tile) return null;
        for (const kind of ['instant', 'interactive']) {
            const list = enc[kind] || [];
            for (let i = list.length - 1; i >= 0; i--) {
                const loc = list[i].location;
                if (loc && loc[0] === tile[0] && loc[1] === tile[1]) return { kind, index: i };
            }
        }
        for (const kind of ENTITY_KINDS) {
            const list = entityList(kind);
            for (let i = list.length - 1; i >= 0; i--) {
                const loc = list[i].location;
                if (loc && loc[0] === tile[0] && loc[1] === tile[1]) return { kind, index: i };
            }
        }
        if (def.spawn && def.spawn[0] === tile[0] && def.spawn[1] === tile[1]) return 'spawn';
        return null;
    };

    const moveMarker = (marker, tile) => {
        if (marker === 'spawn') {
            setDef(prev => (!prev.spawn || prev.spawn[0] !== tile[0] || prev.spawn[1] !== tile[1])
                ? { ...prev, spawn: tile.slice() } : prev);
        } else if (marker.kind === 'instant' || marker.kind === 'interactive') {
            setEnc(prev => {
                const list = prev[marker.kind] || [];
                const entry = list[marker.index];
                if (!entry || (entry.location && entry.location[0] === tile[0] && entry.location[1] === tile[1])) return prev;
                const copy = list.slice();
                copy[marker.index] = { ...entry, location: tile.slice() };
                return { ...prev, [marker.kind]: copy };
            });
        } else {
            setEntityList(marker.kind, entityList(marker.kind).map((entry, j) =>
                j === marker.index ? { ...entry, location: tile.slice() } : entry));
        }
        setDirty(true);
    };

    const down = e => {
        if (e.button === 1) {
            e.preventDefault();
            drag.current = { pan: true, x: e.clientX, y: e.clientY, left: scrollRef.current.scrollLeft, top: scrollRef.current.scrollTop };
            return;
        }
        if (e.button !== 0) return;
        const tile = tileFromEvent(e);
        if (!tile) return;
        const index = tile[1] * W + tile[0];

        if (tool === 'paint' || tool === 'erase') {
            drag.current = { type: 'stroke', value: tool === 'erase' ? 0 : gid };
            setTile(index, drag.current.value);
        } else if (tool === 'fill') {
            floodFill(index, gid);
        } else if (tool === 'rect') {
            drag.current = { type: 'rect', value: gid };
            setRectStart(tile);
        } else if (tool === 'pick') {
            const v = layers[layer][index];
            if (v) setGid(v);
            setTool('paint');
        } else if (tool === 'spawn') {
            setDef(prev => ({ ...prev, spawn: tile.slice() }));
            setSelected('spawn');
            setDirty(true);
        } else if (tool === 'instant' || tool === 'interactive') {
            const entry = {
                location: tile.slice(),
                dialogue: scan.dialogues[0] ? scan.dialogues[0].file : '',
                repeat: tool === 'interactive'
            };
            setEnc(prev => ({ ...prev, [tool]: (prev[tool] || []).concat(entry) }));
            setSelected({ kind: tool, index: (enc[tool] || []).length });
            setDirty(true);
        } else if (tool === 'object') {
            const entry = { type: placeType, location: tile.slice() };
            setObjects(prev => prev.concat(entry));
            setSelected({ kind: 'object', index: objects.length });
            setDirty(true);
        } else if (tool === 'enemy') {
            const entry = { name: placeEnemy, location: tile.slice() };
            setEnemies(prev => prev.concat(entry));
            setSelected({ kind: 'enemy', index: enemies.length });
            setDirty(true);
        } else if (tool === 'npc') {
            const entry = { character: placeCharacter, location: tile.slice() };
            setNpcs(prev => prev.concat(entry));
            setSelected({ kind: 'npc', index: npcs.length });
            setDirty(true);
        } else if (tool === 'pickup') {
            const entry = { kind: placePickup, location: tile.slice() };
            if (placeBuried) entry.buried = true;
            setPickups(prev => prev.concat(entry));
            setSelected({ kind: 'pickup', index: pickups.length });
            setDirty(true);
        } else if (tool === 'select') {
            const marker = markerAt(tile);
            setSelected(marker);
            if (marker) drag.current = { type: 'marker', marker };
        }
    };

    const move = e => {
        if (drag.current && drag.current.pan) {
            const el = scrollRef.current;
            el.scrollLeft = drag.current.left - (e.clientX - drag.current.x);
            el.scrollTop = drag.current.top - (e.clientY - drag.current.y);
            return;
        }
        const tile = tileFromEvent(e);
        if (!tile) { if (hover) setHover(null); return; }
        if (!hover || hover[0] !== tile[0] || hover[1] !== tile[1]) setHover(tile);
        if (!drag.current) return;
        if (drag.current.type === 'stroke') setTile(tile[1] * W + tile[0], drag.current.value);
        else if (drag.current.type === 'marker') moveMarker(drag.current.marker, tile);
    };

    const up = () => {
        const d = drag.current;
        drag.current = null;
        if (d && d.type === 'rect') {
            if (rectStart && hover) applyRect(rectStart, hover, d.value);
            setRectStart(null);
        }
    };

    // ---- panels ----------------------------------------------------------

    const patchDef = (key, value) => { setDef(prev => ({ ...prev, [key]: value })); setDirty(true); };
    const patchEnc = (kind, i, key, value) => {
        setEnc(prev => ({ ...prev, [kind]: prev[kind].map((x, j) => j === i ? { ...x, [key]: value } : x) }));
        setDirty(true);
    };
    const patchEntity = (kind, i, key, value) => {
        setEntityList(kind, entityList(kind).map((x, j) => j === i ? { ...x, [key]: value } : x));
        setDirty(true);
    };

    const resize = () => {
        const w = Math.max(1, Math.min(500, parseInt(newW, 10) || W));
        const h = Math.max(1, Math.min(500, parseInt(newH, 10) || H));
        if (w === W && h === H) return;
        const remap = values => {
            const out = new Array(w * h).fill(0);
            for (let y = 0; y < Math.min(h, H); y++)
                for (let x = 0; x < Math.min(w, W); x++)
                    out[y * w + x] = values[y * W + x] || 0;
            return out;
        };
        setLayers(prev => ({
            decor: remap(prev.decor),
            colliders: remap(prev.colliders),
            upper: remap(prev.upper || new Array(w * h).fill(0))
        }));
        const clampLoc = loc => [Math.min(loc[0] || 0, w - 1), Math.min(loc[1] || 0, h - 1)];
        if (def.spawn) patchDef('spawn', clampLoc(def.spawn));
        setEnc(prev => ({
            instant: (prev.instant || []).map(e => ({ ...e, location: clampLoc(e.location || [0, 0]) })),
            interactive: (prev.interactive || []).map(e => ({ ...e, location: clampLoc(e.location || [0, 0]) }))
        }));
        for (const kind of ENTITY_KINDS) {
            setEntityList(kind, entityList(kind).map(e => ({ ...e, location: clampLoc(e.location || [0, 0]) })));
        }
        setSize({ w, h });
        setDirty(true);
    };

    const playFromHere = async () => {
        setStatus('Building + launching…');
        const result = await api('/api/play', {
            room: stem(room.file),
            tile: def.spawn ? def.spawn.join(',') : undefined
        });
        if (result && result.error) setStatus(result.error);
        else if (result && result.ok) setStatus('Launched (saving off in test session)');
        else setStatus((result && result.error) || 'Launch failed — see Build output');
    };

    const save = async () => {
        // Doors/exits/mirror/pit with empty values drop their keys so the
        // baked config sees "no link" rather than a blank one.
        const cleanExits = Object.fromEntries(Object.entries(exits).filter(([, v]) => v));
        const cleanDoors = Object.fromEntries(Object.entries(doors).filter(([, v]) => v && v.kind));
        const cleanPit = pit.room ? { room: pit.room, ...(pit.tile ? { tile: pit.tile } : {}) } : undefined;
        await api('/api/save', {
            path: 'resources/rooms/' + room.file,
            content: JSON.stringify({
                // Preserve keys the editor doesn't edit (buried, ...).
                ...(room.raw || {}),
                width: W, height: H,
                tileset: tilesetChoice || undefined,
                default: def,
                encounters: enc,
                objects: objects,
                enemies: enemies,
                npcs: npcs,
                pickups: pickups.some(p => !p.buried) ? pickups.filter(p => !p.buried).map(({ buried, ...rest }) => rest) : undefined,
                buried: pickups.some(p => p.buried) ? pickups.filter(p => p.buried).map(({ buried, ...rest }) => rest) : undefined,
                doors: Object.keys(cleanDoors).length ? cleanDoors : undefined,
                exits: Object.keys(cleanExits).length ? cleanExits : undefined,
                mirror: mirror || undefined,
                pit: cleanPit,
                decor: layers.decor,
                colliders: layers.colliders,
                upper: layers.upper
            }, (key, value) => value === undefined ? undefined : value, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    const deleteLegacy = async () => {
        await api('/api/delete', { path: 'resources/rooms/' + room.file.replace(/\.json$/, '.tmx') });
        onSaved();
    };

    const zoomBy = f => setZoom(z => Math.max(0.25, Math.min(4, Math.round(z * f * 4) / 4)));

    const encounterKinds = ['instant', 'interactive'];
    const kindLabels = { instant: 'Instant (on entry)', interactive: 'Interactive (action button)' };

    return (
        <div style={{ flex: 1, minWidth: 0 }}>
            <h1>{room.file} <span className="dim">{W} × {H}</span>
                {dirty && <span className="fail"> (unsaved changes)</span>}</h1>
            {room.legacyTmx && (
                <p className="fail">A legacy Tiled .tmx file sits next to this room and is ignored by the build.
                    <button className="btn" style={{ marginLeft: 8 }} onClick={deleteLegacy}>Delete .tmx</button>
                </p>
            )}

            <div className="mapLayout">
                <div className="mapSide">
                    <div className="toolbar">
                        {TILE_TOOLS.map(([key, label, hotkey]) =>
                            <button key={key} className={'tool' + (tool === key ? ' active' : '')}
                                title={TOOL_HINTS[key] + ' (' + hotkey + ')'}
                                onClick={() => setTool(key)}>{label}</button>)}
                        <span className="toolbarSep" />
                        {MARKER_TOOLS.map(([key, label, hotkey]) =>
                            <button key={key} className={'tool marker' + (tool === key ? ' active' : '')}
                                title={TOOL_HINTS[key] + ' (' + hotkey + ')'}
                                onClick={() => setTool(key)}>{label}</button>)}
                    </div>
                    {(tool === 'object' || tool === 'enemy' || tool === 'npc' || tool === 'pickup') && (
                        <div className="toolbar">
                            <span className="dim">placing:</span>
                            {tool === 'object' && (
                                <select value={placeType} onChange={e => setPlaceType(e.target.value)}
                                    title="Object type (composed from resources/objects/types.json)">
                                    {(scan.objectTypes || []).map(t =>
                                        <option key={t.name} value={t.name}>{t.name}</option>)}
                                </select>
                            )}
                            {tool === 'enemy' && (
                                <select value={placeEnemy} onChange={e => setPlaceEnemy(e.target.value)}>
                                    {(scan.enemies || []).map(en =>
                                        <option key={en.name} value={en.name}>{en.name}</option>)}
                                </select>
                            )}
                            {tool === 'npc' && (
                                <select value={placeCharacter} onChange={e => setPlaceCharacter(e.target.value)}>
                                    {scan.characters.map(c =>
                                        <option key={c.file} value={stem(c.file)}>{stem(c.file)}</option>)}
                                </select>
                            )}
                            {tool === 'pickup' && (
                                <select value={placePickup} onChange={e => setPlacePickup(e.target.value)}
                                    title="Pickup kind">
                                    {PICKUP_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                                </select>
                            )}
                            {tool === 'pickup' && (
                                <label title="Shovel reveals buried items once per visit">
                                    <input type="checkbox" checked={placeBuried}
                                        onChange={e => setPlaceBuried(e.target.checked)} /> buried
                                </label>
                            )}
                        </div>
                    )}
                    <div className="toolbar">
                        {['decor', 'colliders', 'upper'].map(l =>
                            <span key={l} className="layerToggle">
                                <button className={'tool' + (layer === l ? ' active' : '')}
                                    onClick={() => setLayer(l)}>{l}</button>
                                <label title="Toggle visibility">
                                    <input type="checkbox" checked={vis[l]} onChange={e => setVis(prev => ({ ...prev, [l]: e.target.checked }))} />
                                </label>
                            </span>)}
                        <span className="toolbarSep" />
                        <label className="dim"><input type="checkbox" checked={showGrid} onChange={e => setShowGrid(e.target.checked)} /> grid</label>
                        <span className="toolbarSep" />
                        <button className="tool" onClick={() => zoomBy(1 / 1.25)}>-</button>
                        <span className="dim" style={{ width: 40, textAlign: 'center' }}>{Math.round(zoom * 100)}%</span>
                        <button className="tool" onClick={() => zoomBy(1.25)}>+</button>
                    </div>

                    <div className="scrollWrap" ref={scrollRef}>
                        <canvas ref={canvasRef} width={W * ts} height={H * ts}
                            style={{ width: W * ts * zoom, imageRendering: 'pixelated', cursor: 'crosshair', display: 'block' }}
                            onMouseDown={down} onMouseMove={move} onMouseUp={up}
                            onMouseLeave={() => { up(); if (hover) setHover(null); }}
                            onContextMenu={e => e.preventDefault()} />
                    </div>

                    <div className="statusBar">
                        <span>{hover ? 'Tile ' + hover[0] + ',' + hover[1] : '\u00a0'}</span>
                        <span className="chip">Hero: {def.hero || '(none)'}</span>
                        <span className="chip">Music: {def.music || '(none)'}</span>
                        <span className="chip">Sfx: {def.walk_sfx || '(none)'}</span>
                    </div>
                    <p className="dim">{TOOL_HINTS[tool]}</p>

                    <h2>Palette <span className="dim">(tile {gid} selected)</span></h2>
                    <Palette tileset={tileset} img={img} selected={gid} onSelect={setGid} />
                </div>

                <div className="panel">
                    <h2>Room Defaults</h2>
                    <div className="field"><label>Hero</label>
                        <select value={def.hero || ''} onChange={e => patchDef('hero', e.target.value)}>
                            <option value="">(none)</option>
                            {scan.characters.map(c => <option key={c.file} value={stem(c.file)}>{stem(c.file)}</option>)}
                        </select>
                    </div>
                    <div className="field"><label>Music</label>
                        <select value={def.music || ''} onChange={e => patchDef('music', e.target.value)}>
                            <option value="">(none)</option>
                            {scan.music.map(m => <option key={m} value={m}>{m}</option>)}
                        </select>
                    </div>
                    <div className="field"><label>Walk sfx</label>
                        <select value={def.walk_sfx || ''} onChange={e => patchDef('walk_sfx', e.target.value)}>
                            <option value="">(none)</option>
                            {scan.audio.map(a => <option key={a} value={a}>{a}</option>)}
                        </select>
                    </div>
                    <div className="field"><label>Spawn</label>
                        <label><input type="checkbox" checked={!!def.spawn}
                            onChange={e => patchDef('spawn', e.target.checked ? [0, 0] : undefined)} /> enable</label>
                        {def.spawn && <span>
                            <input type="number" value={def.spawn[0]} onChange={e => patchDef('spawn', [+e.target.value, def.spawn[1]])} />
                            <input type="number" value={def.spawn[1]} onChange={e => patchDef('spawn', [def.spawn[0], +e.target.value])} />
                        </span>}
                    </div>

                    <h2>Encounters</h2>
                    {encounterKinds.map(kind => (
                        <div key={kind} style={{ marginBottom: 10 }}>
                            <h2>{kindLabels[kind]}</h2>
                            {(enc[kind] || []).length === 0 && <p className="dim">(none — use the "{kindLabels[kind].split(' ')[0]}" tool on the map)</p>}
                            {(enc[kind] || []).map((entry, i) => {
                                const isSelected = selected && selected !== 'spawn' && selected.kind === kind && selected.index === i;
                                return (
                                    <div key={i}>
                                    <div className={'row encRow' + (isSelected ? ' selected' : '')}
                                        onClick={() => setSelected({ kind, index: i })}>
                                        <span className="encDot" style={{ background: ENCOUNTER_COLORS[kind] }} />
                                        <input type="number" value={entry.location ? entry.location[0] : 0}
                                            onChange={e => patchEnc(kind, i, 'location', [+e.target.value, (entry.location || [0, 0])[1]])} />
                                        <input type="number" value={entry.location ? entry.location[1] : 0}
                                            onChange={e => patchEnc(kind, i, 'location', [(entry.location || [0, 0])[0], +e.target.value])} />
                                        <select value={entry.dialogue || ''}
                                            onChange={e => patchEnc(kind, i, 'dialogue', e.target.value)}>
                                            <option value="">(no dialogue)</option>
                                            {scan.dialogues.map(d => <option key={d.file} value={d.file}>{d.file}</option>)}
                                        </select>
                                        <label title="Fire more than once"><input type="checkbox" checked={!!entry.repeat}
                                            onChange={e => patchEnc(kind, i, 'repeat', e.target.checked)} /> rep</label>
                                        <button className="delete" onClick={e => {
                                            e.stopPropagation();
                                            setEnc(prev => ({ ...prev, [kind]: prev[kind].filter((_, j) => j !== i) }));
                                            setDirty(true);
                                        }}>×</button>
                                    </div>
                                    {isSelected && (
                                        <div className="row" style={{ marginLeft: 22, marginBottom: 6 }}>
                                            <input placeholder="if flag" title="Fire only while this world flag is set"
                                                value={entry.if || ''}
                                                onChange={e => patchEnc(kind, i, 'if', e.target.value || undefined)} />
                                            <input placeholder="unless flag" title="Never fire while this world flag is set"
                                                value={entry.unless || ''}
                                                onChange={e => patchEnc(kind, i, 'unless', e.target.value || undefined)} />
                                        </div>
                                    )}
                                    </div>
                                );
                            })}
                        </div>
                    ))}

                    <h2>Entities</h2>
                    {ENTITY_KINDS.map(kind => {
                        const list = entityList(kind);
                        return (
                            <div key={kind} style={{ marginBottom: 10 }}>
                                <h2>{ENTITY_LABELS[kind]}</h2>
                                {list.length === 0 && <p className="dim">(none — use the "Place {ENTITY_LABELS[kind].replace(/s$/, '')}" tool on the map)</p>}
                                {list.map((entry, i) => {
                                    const isSelected = selected && selected !== 'spawn' && selected.kind === kind && selected.index === i;
                                    const label = kind === 'object' ? (entry.type || '?')
                                        : kind === 'enemy' ? (entry.name || '?')
                                        : kind === 'pickup' ? ((entry.buried ? '(buried) ' : '') + (entry.kind || '?'))
                                        : (entry.character || '?');
                                    return (
                                        <div className={'row encRow' + (isSelected ? ' selected' : '')} key={i}
                                            onClick={() => setSelected({ kind, index: i })}>
                                            <span className="encDot" style={{ background: ENTITY_COLORS[kind] }} />
                                            <span style={{ flex: 1 }}>{label}</span>
                                            <span className="dim">{entry.location ? entry.location[0] + ',' + entry.location[1] : '?'}</span>
                                            <button className="delete" onClick={e => {
                                                e.stopPropagation();
                                                removeEntity({ kind, index: i });
                                                if (isSelected) setSelected(null);
                                            }}>×</button>
                                        </div>
                                    );
                                })}
                            </div>
                        );
                    })}
                    {selected && selected !== 'spawn' && ENTITY_KINDS.includes(selected.kind) && (
                        <EntityFields
                            kind={selected.kind}
                            entry={entityList(selected.kind)[selected.index]}
                            scan={scan}
                            patch={(key, value) => patchEntity(selected.kind, selected.index, key, value)} />
                    )}

                    <h2>Doors & Exits</h2>
                    {['north', 'south', 'east', 'west'].map(edge => (
                        <div className="row" key={edge} style={{ marginBottom: 4 }}>
                            <span className="dim" style={{ width: 44 }}>{edge}</span>
                            <select value={(doors[edge] || {}).kind || ''} title="Door kind on this edge"
                                onChange={e => {
                                    const kind = e.target.value;
                                    setDoors(prev => {
                                        const copy = { ...prev };
                                        if (kind) copy[edge] = { ...(copy[edge] || {}), kind };
                                        else delete copy[edge];
                                        return copy;
                                    });
                                    setDirty(true);
                                }}>
                                <option value="">no door</option>
                                {['open', 'closed', 'locked', 'big', 'bombable', 'shutter'].map(k =>
                                    <option key={k} value={k}>{k}</option>)}
                            </select>
                            <select value={exits[edge] || ''} title="The room this edge leads to (the map graph)"
                                onChange={e => {
                                    const target = e.target.value;
                                    setExits(prev => {
                                        const copy = { ...prev };
                                        if (target) copy[edge] = target;
                                        else delete copy[edge];
                                        return copy;
                                    });
                                    setDirty(true);
                                }}>
                                <option value="">walled</option>
                                {scan.rooms.filter(r => r.file !== room.file).map(r =>
                                    <option key={r.file} value={r.file}>{stem(r.file)}</option>)}
                            </select>
                        </div>
                    ))}
                    <p className="dim">Exits draw on the world graph; a shutter stays shut until the room is cleared, a locked one takes a small key.</p>

                    <h2>World Links</h2>
                    <div className="field"><label>Mirror pair <span className="dim">(the dark world)</span></label>
                        <select value={mirror || ''} onChange={e => { setMirror(e.target.value); setDirty(true); }}>
                            <option value="">(none)</option>
                            {scan.rooms.filter(r => r.file !== room.file).map(r =>
                                <option key={r.file} value={r.file}>{stem(r.file)}</option>)}
                        </select>
                    </div>
                    <div className="field"><label>Pit landing <span className="dim">(falling into a pit tile warps here)</span></label>
                        <select value={pit.room || ''}
                            onChange={e => { setPit(prev => ({ ...prev, room: e.target.value })); setDirty(true); }}>
                            <option value="">(respawn at the safe tile)</option>
                            {scan.rooms.filter(r => r.file !== room.file).map(r =>
                                <option key={r.file} value={r.file}>{stem(r.file)}</option>)}
                        </select>
                    </div>
                    {pit.room && (
                        <div className="row">
                            <div className="field"><label>Landing tile X</label>
                                <input type="number" value={(pit.tile || [])[0] || 0}
                                    onChange={e => { setPit(prev => ({ ...prev, tile: [+e.target.value, (prev.tile || [])[1] || 0] })); setDirty(true); }} />
                            </div>
                            <div className="field"><label>Y</label>
                                <input type="number" value={(pit.tile || [])[1] || 0}
                                    onChange={e => { setPit(prev => ({ ...prev, tile: [(prev.tile || [])[0] || 0, +e.target.value] })); setDirty(true); }} />
                            </div>
                        </div>
                    )}
                    <p className="dim">Warp pads and stairs carry their targets on the map (place + edit them there); all links preview on the world graph.</p>

                    <h2>Tileset</h2>
                    <div className="field"><label>Sheet</label>
                        <select value={tilesetChoice}
                            onChange={e => { setTilesetChoice(e.target.value); setDirty(true); }}>
                            <option value="">(default: {(defaultTileset || {}).file || 'none'})</option>
                            {tilesets.map(t =>
                                <option key={t.file} value={t.file}>{t.file} ({t.tileCount} tiles)</option>)}
                        </select>
                    </div>
                    <p className="dim">The shared sheet this room's layers paint from — multiple rooms can use the same one (manage them in the Tiles tab).</p>

                    <h2>Resize Room</h2>
                    <div className="row">
                        <input type="number" value={newW} onChange={e => setNewW(+e.target.value)} />
                        <span className="dim">×</span>
                        <input type="number" value={newH} onChange={e => setNewH(+e.target.value)} />
                        <button className="btn" onClick={resize}>Apply</button>
                    </div>
                    <p className="dim">Content stays anchored to the top-left; tiles outside the new bounds are dropped and markers are clamped inside.</p>

                    <div className="row" style={{ marginTop: 16 }}>
                        <button className="btn primary" onClick={save}>Save Room</button>
                        <button className="btn" onClick={playFromHere}
                            title="Build, then launch the game straight into this room at the spawn tile (a throwaway test session — nothing saves)">
                            Play from here</button>
                        <span className="dim">{status}</span>
                    </div>

                    <h2>Legend</h2>
                    <p className="dim">
                        <span className="encDot" style={{ background: '#22c55e' }} /> hero spawn (S)<br />
                        <span className="encDot" style={{ background: ENCOUNTER_COLORS.instant }} /> instant (!)<br />
                        <span className="encDot" style={{ background: ENCOUNTER_COLORS.interactive }} /> interactive (A)<br />
                        red tint = collider tile (blocks movement, still drawn)
                    </p>
                </div>
            </div>
        </div>
    );
}

// Entity property form: type-specific fields for the selected object,
// enemy, or NPC (the type's interactions decide what shows — the editor
// reads the same composition data the engine interprets).
function EntityFields({ kind, entry, scan, patch }) {
    if (!entry) return null;

    const numberField = (label, value, onSet, props) => (
        <div className="field"><label>{label}</label>
            <input type="number" value={value == null ? 0 : value}
                onChange={e => onSet(e.target.value === '' ? undefined : +e.target.value)} {...(props || {})} />
        </div>
    );

    if (kind === 'enemy') {
        return (
            <div className="entityFields">
                <div className="field"><label>Enemy</label>
                    <select value={entry.name || ''} onChange={e => patch('name', e.target.value)}>
                        {(scan.enemies || []).map(en => <option key={en.name} value={en.name}>{en.name}</option>)}
                    </select>
                </div>
            </div>
        );
    }

    if (kind === 'npc') {
        return (
            <div className="entityFields">
                <div className="field"><label>Character</label>
                    <select value={entry.character || ''} onChange={e => patch('character', e.target.value)}>
                        {scan.characters.map(c => <option key={c.file} value={stem(c.file)}>{stem(c.file)}</option>)}
                    </select>
                </div>
                <div className="field"><label>Dialogue</label>
                    <select value={entry.dialogue || ''} onChange={e => patch('dialogue', e.target.value || undefined)}>
                        <option value="">(silent)</option>
                        {scan.dialogues.map(d => <option key={d.file} value={d.file}>{d.file}</option>)}
                    </select>
                </div>
                <div className="field"><label>Behavior</label>
                    <label><input type="checkbox" checked={!!entry.wander}
                        onChange={e => patch('wander', e.target.checked || undefined)} /> wander</label>
                </div>
                <div className="field"><label>Present if flag</label>
                    <input placeholder="(always)" value={entry.if || ''}
                        onChange={e => patch('if', e.target.value || undefined)} />
                </div>
                <div className="field"><label>Present unless flag</label>
                    <input placeholder="(never hidden)" value={entry.unless || ''}
                        onChange={e => patch('unless', e.target.value || undefined)} />
                </div>
            </div>
        );
    }

    if (kind === 'pickup') {
        return (
            <div className="entityFields">
                <div className="field"><label>Kind</label>
                    <select value={entry.kind || 'heart'} onChange={e => patch('kind', e.target.value)}>
                        {PICKUP_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                    </select>
                </div>
                <div className="field"><label>Shovel</label>
                    <label title="Invisible until the shovel turns the tile over (once per visit)">
                        <input type="checkbox" checked={!!entry.buried}
                            onChange={e => patch('buried', e.target.checked || undefined)} /> buried
                    </label>
                </div>
            </div>
        );
    }

    // Object: the type's interaction composition drives the form.
    const type = entry.type || 'pot';
    const typeDef = (scan.objectTypes || []).find(t => t.name === type);
    const interactions = (typeDef && typeDef.interactions) || [];
    const has = name => interactions.includes(name);
    const grant = entry.grant || {};
    const warp = entry.warp || {};

    return (
        <div className="entityFields">
            <div className="field"><label>Type</label>
                <select value={type} onChange={e => patch('type', e.target.value)}>
                    {(scan.objectTypes || []).map(t => <option key={t.name} value={t.name}>{t.name}</option>)}
                </select>
            </div>

            {has('chest') && (
                <div>
                    <div className="field"><label>Chest kind</label>
                        <select value={entry.kind || 'normal'} onChange={e => patch('kind', e.target.value)}>
                            <option value="normal">normal</option>
                            <option value="locked">locked (small key)</option>
                            <option value="big">big (big key)</option>
                        </select>
                    </div>
                    <div className="field"><label>Grant</label>
                        <select value={grant.kind || 'rupees'}
                            onChange={e => patch('grant', { kind: e.target.value })}>
                            {GRANT_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                        </select>
                    </div>
                    {grant.kind === 'item' && (
                        <div className="field"><label>Item</label>
                            <select value={grant.item || ''}
                                onChange={e => patch('grant', { ...grant, item: e.target.value })}>
                                <option value="">(pick)</option>
                                {(scan.items || []).map(it => <option key={it.name} value={it.name}>{it.name}</option>)}
                            </select>
                        </div>
                    )}
                    {grant.kind === 'flag' && (
                        <div className="field"><label>Flag name</label>
                            <input placeholder="world flag" value={grant.flag || ''}
                                onChange={e => patch('grant', { ...grant, flag: e.target.value })} />
                        </div>
                    )}
                    {grant.kind !== 'item' && grant.kind !== 'flag' && (
                        numberField('Amount', grant.amount, v => patch('grant', { ...grant, amount: v }))
                    )}
                </div>
            )}

            {(has('warp') || has('stair')) && (
                <div>
                    <div className="field"><label>{has('stair') ? 'Stair' : 'Warp'} target</label>
                        <select value={warp.room || ''}
                            onChange={e => patch('warp', { room: e.target.value, tile: warp.tile || [0, 0] })}>
                            <option value="">(pick room)</option>
                            {scan.rooms.map(r => <option key={r.file} value={r.file}>{stem(r.file)}</option>)}
                        </select>
                    </div>
                    {numberField('Target tile X', (warp.tile || [])[0],
                        v => patch('warp', { ...(warp.room ? { room: warp.room } : {}), tile: [v, (warp.tile || [])[1] || 0] }))}
                    {numberField('Target tile Y', (warp.tile || [])[1],
                        v => patch('warp', { ...(warp.room ? { room: warp.room } : {}), tile: [(warp.tile || [])[0] || 0, v] }))}
                </div>
            )}

            {has('switch') && (
                <div>
                    <div className="field"><label>Mode</label>
                        <select value={entry.mode || 'hold'} onChange={e => patch('mode', e.target.value)}>
                            <option value="hold">hold (weight)</option>
                            <option value="latch">latch (forever)</option>
                            <option value="timed">timed (~3s)</option>
                        </select>
                    </div>
                    {numberField('Link group', entry.link, v => patch('link', v))}
                </div>
            )}

            {(has('torch') || has('lever')) && (
                numberField('Link group', entry.link, v => patch('link', v))
            )}

            {has('barrier') && (
                <div>
                    <div className="field"><label>Color</label>
                        <select value={entry.color || 'linked'} onChange={e => patch('color', e.target.value)}>
                            <option value="linked">linked (follows its group)</option>
                            <option value="blue">blue (crystal)</option>
                            <option value="red">red (crystal)</option>
                        </select>
                    </div>
                    {entry.color !== 'blue' && entry.color !== 'red' && numberField('Link group', entry.link, v => patch('link', v))}
                </div>
            )}

            {has('readable') && (
                <div className="field"><label>Dialogue</label>
                    <select value={entry.dialogue || ''} onChange={e => patch('dialogue', e.target.value)}>
                        <option value="">(pick)</option>
                        {scan.dialogues.map(d => <option key={d.file} value={d.file}>{d.file}</option>)}
                    </select>
                </div>
            )}

            {has('hides') && (
                <div className="field"><label>Hides enemy</label>
                    <select value={entry.hides || ''} onChange={e => patch('hides', e.target.value || undefined)}>
                        <option value="">(nothing)</option>
                        {(scan.enemies || []).map(en => <option key={en.name} value={en.name}>{en.name}</option>)}
                    </select>
                </div>
            )}

            {has('platform') && (
                <div className="field"><label>Rail path</label>
                    <input placeholder="x,y x,y …" title="Tile waypoints, glided in a loop"
                        value={(entry.path || []).map(p => (p || []).join(',')).join(' ')}
                        onChange={e => {
                            const text = e.target.value.trim();
                            patch('path', text
                                ? text.split(/\s+/).map(pair => pair.split(',').map(Number))
                                : undefined);
                        }} />
                </div>
            )}
        </div>
    );
}

// World graph preview: rooms as boxes laid out like the in-game map (BFS
// over exits from the first room; rooms off the graph shelf bottom-right),
// with exits, mirror pairs, pit-landings, and warp/stair links drawn.
function WorldGraph({ rooms, current, onSelect }) {
    const pitchX = 150, pitchY = 110, boxW = 116, boxH = 58;

    const resolve = name => {
        if (name == null) return -1;
        const text = String(name);
        return rooms.findIndex(r => r.file === text || stem(r.file) === stem(text));
    };

    const { xs, ys } = useMemo(() => {
        const xs = rooms.map(() => Infinity);
        const ys = rooms.map(() => Infinity);
        if (rooms.length) { xs[0] = 0; ys[0] = 0; }
        let changed = true;
        while (changed) {
            changed = false;
            for (let i = 0; i < rooms.length; i++) {
                if (xs[i] === Infinity) continue;
                const exits = (rooms[i].raw && rooms[i].raw.exits) || {};
                const edges = [['north', 0, -1], ['south', 0, 1], ['east', 1, 0], ['west', -1, 0]];
                for (const [dir, ox, oy] of edges) {
                    const target = resolve(exits[dir]);
                    if (target < 0 || xs[target] !== Infinity) continue;
                    xs[target] = xs[i] + ox;
                    ys[target] = ys[i] + oy;
                    changed = true;
                }
            }
        }
        let maxX = 0, maxY = 0;
        for (let i = 0; i < rooms.length; i++) {
            if (xs[i] === Infinity) continue;
            maxX = Math.max(maxX, xs[i]);
            maxY = Math.max(maxY, ys[i]);
        }
        let shelf = 0;
        for (let i = 0; i < rooms.length; i++) {
            if (xs[i] !== Infinity) continue;
            xs[i] = maxX + 1 + shelf;
            ys[i] = maxY + 1;
            shelf++;
        }
        return { xs, ys };
    }, [rooms]);

    let width = 0, height = 0;
    for (let i = 0; i < rooms.length; i++) {
        width = Math.max(width, (xs[i] + 1) * pitchX + boxW);
        height = Math.max(height, (ys[i] + 1) * pitchY + boxH);
    }

    const center = i => [xs[i] * pitchX + boxW / 2, ys[i] * pitchY + boxH / 2];
    const link = (a, b) => {
        if (a < 0 || b < 0) return null;
        const [x0, y0] = center(a), [x1, y1] = center(b);
        return <line key={'l' + a + '-' + b} x1={x0} y1={y0} x2={x1} y2={y1} strokeWidth={2} />;
    };

    const links = [];
    rooms.forEach((r, i) => {
        const raw = r.raw || {};
        const exits = raw.exits || {};
        // Exit edges (gray) — drawn once per pair via the lower index.
        for (const dir of ['north', 'south', 'east', 'west']) {
            const target = resolve(exits[dir]);
            if (target >= 0 && target > i) links.push(
                <line key={'e' + i + dir} x1={center(i)[0]} y1={center(i)[1]}
                    x2={center(target)[0]} y2={center(target)[1]} stroke="#64748b" strokeWidth={2} />);
        }
        // Mirror pairs (dashed purple) — once per pair.
        const mirror = resolve(raw.mirror);
        if (mirror >= 0 && mirror > i) links.push(
            <line key={'m' + i} x1={center(i)[0]} y1={center(i)[1]} x2={center(mirror)[0]} y2={center(mirror)[1]}
                stroke="#a855f7" strokeWidth={2} strokeDasharray="6 4" />);
        // Pit landings (dotted amber).
        const pit = resolve((raw.pit || {}).room);
        if (pit >= 0) links.push(
            <line key={'p' + i} x1={center(i)[0]} y1={center(i)[1]} x2={center(pit)[0]} y2={center(pit)[1]}
                stroke="#f59e0b" strokeWidth={2} strokeDasharray="2 5" />);
        // Warp pads + stairs (dashed teal).
        (raw.objects || []).forEach((o, j) => {
            if (o.type !== 'warp' && o.type !== 'stair') return;
            const target = resolve((o.warp || {}).room);
            if (target < 0) return;
            links.push(
                <line key={'w' + i + '-' + j} x1={center(i)[0]} y1={center(i)[1]} x2={center(target)[0]} y2={center(target)[1]}
                    stroke={o.type === 'stair' ? '#14b8a6' : '#0ea5e9'} strokeWidth={2} strokeDasharray="4 4" />);
        });
    });

    return (
        <div className="graphWrap">
            <svg width={width} height={height} style={{ display: 'block' }}>
                {links}
                {rooms.map((r, i) => (
                    <g key={r.file} onClick={() => onSelect(i)} style={{ cursor: 'pointer' }}>
                        <rect x={xs[i] * pitchX} y={ys[i] * pitchY} width={boxW} height={boxH} rx={6}
                            fill={i === current ? '#1d4ed8' : '#334155'}
                            stroke={i === current ? '#fff' : '#64748b'} strokeWidth={2} />
                        <text x={xs[i] * pitchX + boxW / 2} y={ys[i] * pitchY + 22} textAnchor="middle"
                            fill="#fff" fontSize={13} fontWeight="bold">{stem(r.file)}</text>
                        <text x={xs[i] * pitchX + boxW / 2} y={ys[i] * pitchY + 42} textAnchor="middle"
                            fill="#cbd5e1" fontSize={11}>{r.mapWidth}×{r.mapHeight}</text>
                    </g>
                ))}
            </svg>
        </div>
    );
}

function Palette({ tileset, img, selected, onSelect }) {
    const ref = useRef(null);
    const [hoverTile, setHoverTile] = useState(null);
    const cols = tileset.columns || 1;
    const rows = Math.max(1, Math.ceil((tileset.tileCount || 1) / cols));
    const tw = tileset.tileWidth || 32;

    useEffect(() => {
        const canvas = ref.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0);
        const outline = (frame, color) => {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.strokeRect((frame % cols) * tw + 1, Math.floor(frame / cols) * tw + 1, tw - 2, tw - 2);
        };
        if (selected > 0) outline(selected - 1, '#f00');
        if (hoverTile != null) outline(hoverTile, '#fff');
    }, [img, selected, hoverTile]);

    const tileFromEvent = e => {
        const canvas = ref.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / cols));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / rows));
        return (x >= 0 && y >= 0 && x < cols && y < rows) ? y * cols + x : null;
    };

    return (
        <div>
            <div className="scrollWrap paletteWrap">
                <canvas ref={ref} width={cols * tw} height={rows * tw}
                    style={{ width: cols * tw, imageRendering: 'pixelated', cursor: 'pointer', display: 'block' }}
                    onMouseMove={e => setHoverTile(tileFromEvent(e))}
                    onMouseLeave={() => setHoverTile(null)}
                    onClick={e => {
                        const tile = tileFromEvent(e);
                        if (tile != null) onSelect(tile + 1);
                    }} />
            </div>
            <p className="dim">Tile {hoverTile != null ? hoverTile : selected - 1} — frame index into the sheet (0-based). 0 = empty.</p>
        </div>
    );
}

function RoomsTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [w, setW] = useState(30);
    const [h, setH] = useState(20);
    const [error, setError] = useState('');
    const [showGraph, setShowGraph] = useState(false);
    const room = scan.rooms[sel];

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'room', name: name.trim(), width: w, height: h });
        if (result.error) return setError(result.error);
        setName(''); setError(''); refresh();
    };

    const remove = async r => {
        if (!await confirm('Delete ' + r.file + '?')) return;
        await api('/api/delete', { path: 'resources/rooms/' + r.file });
        setSel(0); refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Rooms</h2>
                <button className={'btn' + (showGraph ? ' primary' : '')} style={{ width: '100%', marginBottom: 8 }}
                    onClick={() => setShowGraph(!showGraph)}>
                    {showGraph ? 'Back to the editor' : 'World graph'}
                </button>
                {scan.rooms.map((r, i) => (
                    <div className="row" key={r.file}>
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => setSel(i)}>{r.file}</button>
                        <button className="delete" onClick={() => remove(r)}>×</button>
                    </div>
                ))}
                <h2>New Room</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <input type="number" value={w} onChange={e => setW(+e.target.value)} />
                    <input type="number" value={h} onChange={e => setH(+e.target.value)} />
                </div>
                <button className="btn" onClick={create}>Create Room</button>
                {error && <p className="fail">{error}</p>}
            </div>
            {room
                ? (showGraph
                    ? <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minWidth: 0 }}>
                        <h1>World Graph</h1>
                        <p className="dim">Exits solid gray · mirror pairs dashed purple · pit-landings dotted amber · warp pads dashed sky · stairs dashed teal. Click a room to edit it. Off-graph rooms shelf bottom-right, like the in-game map.</p>
                        <WorldGraph rooms={scan.rooms} current={sel}
                            onSelect={i => { setSel(i); setShowGraph(false); }} />
                    </div>
                    : <RoomEditor key={room.file + ':' + scan.rooms.length} room={room} scan={scan} onSaved={refresh} />)
                : <p className="dim">No rooms. Create one on the left.</p>}
        </div>
    );
}

// ---- data vocabulary lists (mirrored from the generator's tables; the
// editor can't run the bake, so the stable engine vocabularies live here) --
const USE_KINDS = ['none', 'boomerang', 'arrow', 'bomb', 'bolt', 'hammer', 'shovel',
    'hookshot', 'medallion', 'byrna', 'cape', 'somaria', 'powder', 'flute', 'net',
    'bottle', 'mirror'];
const ELEMENT_KINDS = ['', 'fire', 'ice'];
const BEHAVIOR_PRESETS = ['', 'soldier', 'octorok', 'boss'];
const PICKUP_KINDS = ['heart', 'rupee', 'rupee5', 'arrow', 'bomb', 'magic', 'fairy', 'key'];
// The engine's interaction vocabulary (OBJINT_* in include/engine.h; the
// generator's OBJINT_NAMES table is the authority — mirrored here).
const OBJINT_KINDS = ['solid', 'floor', 'liftable', 'cut', 'dash_smash', 'pushable',
    'sinks', 'weight', 'switch', 'barrier', 'readable', 'chest', 'torch', 'warp',
    'crystal', 'leaves', 'two_frame', 'hides', 'pullable', 'hammer_smash', 'heavy',
    'lever', 'platform', 'hookable', 'powder', 'stair'];
const DIRECTIONS_4 = ['down', 'right', 'left', 'up'];

// Game-code custom behaviors (resources/code/): scaffolding, the
// manifest-driven list, and compile-error surfacing (a fast syntax-only
// check with the engine's include paths; full builds surface the same
// output on the Build tab).
function BehaviorSection({ scan, refresh }) {
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const [check, setCheck] = useState(null);   // {ok, output}

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'behavior', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError('');
        const verified = await api('/api/check_code', { file: name.trim() + '.cpp' });
        setCheck(verified && verified.ok !== undefined ? verified : null);
        refresh();
    };

    const checkAll = async () => {
        setCheck(null);
        const result = await api('/api/check_code', {});
        setCheck(result);
    };

    return (
        <div style={{ borderTop: '1px solid #334155', marginTop: 14, paddingTop: 10 }}>
            <h2>Game-code behaviors</h2>
            <p className="dim">resources/code/*.cpp — compiled into the binary; enemies reference them as "custom:&lt;name&gt;".</p>
            {(scan.behaviors || []).map(b => (
                <div className="row" key={b}>
                    <code style={{ flex: 1 }}>custom:{b}</code>
                    <button className="tool" title="Syntax-check this unit"
                        onClick={async () => setCheck(await api('/api/check_code', { file: b + '.cpp' }))}>check</button>
                </div>
            ))}
            <div className="row" style={{ marginTop: 6 }}>
                <input placeholder="name" value={name} style={{ width: 96 }} onChange={e => setName(e.target.value)} />
                <button className="btn" onClick={create}>Scaffold</button>
            </div>
            <button className="btn" style={{ marginTop: 6 }} onClick={checkAll}>Compile check all</button>
            {error && <p className="fail">{error}</p>}
            {check && (
                <div style={{ marginTop: 8 }}>
                    {check.ok
                        ? <p className="dim" style={{ color: '#22c55e' }}>✓ Compiles clean.</p>
                        : <p className="fail">✗ Compile errors:</p>}
                    {check.output && check.output.trim() && check.output !== '(web build compiled clean)' && (
                        <pre className="build" style={{ maxHeight: 200 }}>{check.output.slice(-4000)}</pre>
                    )}
                </div>
            )}
        </div>
    );
}

function EnemiesTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const enemy = scan.enemies[sel];

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'enemy', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError(''); refresh();
    };

    const remove = async e => {
        if (!await confirm('Delete ' + e.file + '?')) return;
        await api('/api/delete', { path: 'resources/enemies/' + e.file });
        setSel(0); refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Enemies</h2>
                {scan.enemies.map((e, i) => (
                    <div className="row" key={e.file}>
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => setSel(i)}>{e.name}</button>
                        <button className="delete" onClick={() => remove(e)}>×</button>
                    </div>
                ))}
                <h2>New</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Create</button>
                </div>
                {error && <p className="fail">{error}</p>}
                <BehaviorSection scan={scan} refresh={refresh} />
            </div>
            {enemy
                ? <EnemyEditor key={enemy.file} enemy={enemy} scan={scan} onSaved={refresh} />
                : <p className="dim">No enemies. Create one, then upload its sheet (same-named .png) right in the editor.</p>}
        </div>
    );
}

// Sheet upload-and-replace for an entity's png: renames whatever the user
// picks to the canonical same-name pairing (resources/<dir>/<stem>.png), so
// the json and its art stay linked without leaving the editor.
function SheetUpload({ dir, file, onUploaded }) {
    const inputRef = useRef(null);
    const [status, setStatus] = useState('');
    const [busy, setBusy] = useState(false);

    const upload = e => {
        const picked = e.target.files[0];
        if (!picked) return;
        setBusy(true);
        const reader = new FileReader();
        reader.onload = async () => {
            // Whatever the picked file is called, it lands under the
            // entity's canonical name (json stem + .png, or the tileset's
            // own file when replacing a sheet).
            const name = file.replace(/\.[^.]+$/, '') + '.png';
            const result = await api('/api/upload', { dir, name, data: String(reader.result).split(',')[1] });
            setBusy(false);
            setStatus(result.error || 'Replaced ' + name);
            e.target.value = '';
            if (!result.error) onUploaded();
        };
        reader.readAsDataURL(picked);
    };

    return (
        <span className="row" style={{ margin: 0 }}>
            <input ref={inputRef} type="file" accept="image/png" style={{ display: 'none' }} onChange={upload} />
            <button className="btn" disabled={busy} onClick={() => inputRef.current.click()}>
                {busy ? 'Uploading…' : 'Upload ' + file.replace(/\.[^.]+$/, '') + '.png'}
            </button>
            {status && <span className={status.indexOf('Replaced') === 0 ? 'dim' : 'fail'}>{status}</span>}
        </span>
    );
}

function EnemyEditor({ enemy, scan, onSaved }) {
    const [data, setData] = useState(enemy.data);
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);
    const [sheet, setSheet] = useState(null);
    const [sheetVersion, setSheetVersion] = useState(0);

    // Sheet preview: resources/enemies/<stem>.png, reloaded after a replace.
    const sheetPath = 'resources/enemies/' + stem(enemy.file) + '.png';
    useEffect(() => {
        let alive = true;
        resourceUrl(sheetPath).then(src => { if (alive) setSheet(src); })
            .catch(() => { if (alive) setSheet(null); });
        return () => { alive = false; };
    }, [sheetPath, sheetVersion]);

    const stats = data.stats || {};
    const components = stats.components || {};
    const drops = stats.drops || {};

    const patch = (key, value) => { setData(prev => ({ ...prev, [key]: value })); setDirty(true); };
    const patchStats = (key, value) => {
        setData(prev => ({ ...prev, stats: { ...(prev.stats || {}), [key]: value } }));
        setDirty(true);
    };
    const patchComponent = (key, value) => {
        setData(prev => ({
            ...prev,
            stats: {
                ...(prev.stats || {}),
                components: { ...(prev.stats || {}).components || {}, [key]: value }
            }
        }));
        setDirty(true);
    };
    const patchDrop = (kind, weight) => {
        setData(prev => ({
            ...prev,
            stats: {
                ...(prev.stats || {}),
                drops: { ...((prev.stats || {}).drops || {}), [kind]: weight }
            }
        }));
        setDirty(true);
    };
    const removeDrop = kind => {
        const copy = { ...drops };
        delete copy[kind];
        patchStats('drops', copy);
    };
    const addDrop = () => patchDrop('heart', 10);

    // Stand/move animation frame lists, per direction ("0,4" notation).
    const animValue = (block, dir) => ((data[block] || {})[dir] || []).join(',');
    const patchAnim = (block, dir, text) => {
        const frames = text.split(',').map(s => parseInt(s.trim(), 10))
            .filter(n => !isNaN(n));
        patch(block, { ...(data[block] || {}), [dir]: frames });
    };

    const save = async () => {
        await api('/api/save', {
            path: 'resources/enemies/' + enemy.file,
            content: JSON.stringify(data, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    const numberField = (label, value, onSet) => (
        <div className="field"><label>{label}</label>
            <input type="number" value={value == null ? 0 : value}
                onChange={e => onSet(e.target.value === '' ? undefined : +e.target.value)} />
        </div>
    );

    return (
        <div className="panel" style={{ maxWidth: 560 }}>
            <h1>{enemy.name} {dirty && <span className="fail">(unsaved)</span>}</h1>

            <h2>Sheet & Stats</h2>
            <div className="row">
                <SheetUpload dir="enemies" file={enemy.file} onUploaded={() => setSheetVersion(v => v + 1)} />
                {sheet
                    ? <img className="sheet" src={sheet} style={{ maxHeight: 56 }} alt="" />
                    : <span className="dim">no sheet yet — the bake draws a placeholder from the color</span>}
            </div>
            <div className="row">
                {numberField('Sheet columns', data.width, v => patch('width', v))}
                <div className="field"><label>Color (rgb)</label>
                    <span>
                        <input type="number" style={{ width: 52 }} value={(data.color || [200, 60, 60])[0]}
                            onChange={e => patch('color', [+e.target.value, (data.color || [])[1] || 0, (data.color || [])[2] || 0])} />
                        <input type="number" style={{ width: 52 }} value={(data.color || [])[1] || 0}
                            onChange={e => patch('color', [(data.color || [])[0] || 0, +e.target.value, (data.color || [])[2] || 0])} />
                        <input type="number" style={{ width: 52 }} value={(data.color || [])[2] || 0}
                            onChange={e => patch('color', [(data.color || [])[0] || 0, (data.color || [])[1] || 0, +e.target.value])} />
                    </span>
                </div>
            </div>
            <div className="row">
                {numberField('HP', stats.hp, v => patchStats('hp', v))}
                {numberField('Speed', stats.speed, v => patchStats('speed', v))}
                {numberField('Damage (half-hearts)', stats.damage, v => patchStats('damage', v))}
            </div>
            <div className="field"><label>Behavior</label>
                <select value={stats.behavior || ''}
                    onChange={e => patchStats('behavior', e.target.value)}>
                    {BEHAVIOR_PRESETS.map(b =>
                        <option key={b} value={b}>{b === '' ? '(plain wanderer)' : b}</option>)}
                    {(scan.behaviors || []).map(b =>
                        <option key={'c' + b} value={'custom:' + b}>custom:{b}</option>)}
                </select>
            </div>
            <div className="row">
                {numberField('Sight (charge radius)', stats.sight, v => patchStats('sight', v))}
                {numberField('Shoot period', stats.shoot_period, v => patchStats('shoot_period', v))}
            </div>

            <h2>Components</h2>
            <div className="checkGrid">
                <label title="Arena boss: seals the room, health bar, banked defeat">
                    <input type="checkbox" checked={!!components.boss} onChange={e => patchComponent('boss', e.target.checked || undefined)} /> boss</label>
                <label title="Mid-fight: seals + health bar, room-local defeat">
                    <input type="checkbox" checked={!!components.mini_boss} onChange={e => patchComponent('mini_boss', e.target.checked || undefined)} /> mini_boss</label>
                <label title="Only projectile hits land (reflectable magic)">
                    <input type="checkbox" checked={!!components.strike_immune} onChange={e => patchComponent('strike_immune', e.target.checked || undefined)} /> strike_immune</label>
                <label title="The bug net bottles it">
                    <input type="checkbox" checked={!!components.catchable} onChange={e => patchComponent('catchable', e.target.checked || undefined)} /> catchable</label>
            </div>
            <div className="row">
                {numberField('Mover walk time (s)', (components.mover || {}).walk_time,
                    v => patchComponent('mover', v == null ? undefined : { ...(components.mover || {}), walk_time: v }))}
                <label title="Occasional random course change while walking">
                    <input type="checkbox" checked={!!(components.mover || {}).turn_chance}
                        onChange={e => patchComponent('mover', e.target.checked
                            ? { ...(components.mover || {}), turn_chance: true }
                            : (components.mover || {}).walk_time != null ? { walk_time: components.mover.walk_time } : undefined)} /> turn_chance</label>
            </div>

            <h2>Animations <span className="dim">(frame numbers, comma-separated)</span></h2>
            {['stand', 'move'].map(block => (
                <div key={block} style={{ marginBottom: 8 }}>
                    <div className="dim">{block}</div>
                    {DIRECTIONS_4.map(dir => (
                        <div className="field" key={dir}>
                            <label>{dir}</label>
                            <input value={animValue(block, dir)}
                                onChange={e => patchAnim(block, dir, e.target.value)} />
                        </div>
                    ))}
                </div>
            ))}

            <h2>Drops <span className="dim">(kind → weight)</span></h2>
            {Object.keys(drops).length === 0 && <p className="dim">(engine default roll)</p>}
            {Object.entries(drops).map(([kind, weight]) => (
                <div className="row" key={kind}>
                    <select value={kind} onChange={e => {
                        const copy = { ...drops };
                        delete copy[kind];
                        copy[e.target.value] = weight;
                        patchStats('drops', copy);
                    }}>
                        {PICKUP_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                    </select>
                    <input type="number" style={{ width: 70 }} value={weight}
                        onChange={e => patchDrop(kind, +e.target.value)} />
                    <button className="delete" onClick={() => removeDrop(kind)}>×</button>
                </div>
            ))}
            <button className="btn" onClick={addDrop}>+ Add drop</button>

            <div className="row" style={{ marginTop: 16 }}>
                <button className="btn primary" onClick={save}>Save Enemy</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

// Item subscreen icons: resources/ui/items.png holds one 16x16 frame
// per item, frame = item id (sorted-filename order — the same order this
// list is in). The URL loads once and every preview crops its frame.
let itemIconsPromise = null;
const itemIconSheetUrl = () => {
    if (!itemIconsPromise) itemIconsPromise = resourceUrl('resources/ui/items.png');
    return itemIconsPromise;
};

function ItemIcon({ index, count, size = 32 }) {
    const [url, setUrl] = useState(null);
    useEffect(() => {
        let alive = true;
        itemIconSheetUrl().then(u => { if (alive) setUrl(u); });
        return () => { alive = false; };
    }, []);
    if (url == null || index == null || index < 0 || count <= 0)
        return <div style={{ width: size, height: size, flex: 'none' }} />;
    return <div title="subscreen icon (resources/ui/items.png)"
        style={{
            width: size, height: size, flex: 'none',
            backgroundImage: `url(${url})`,
            backgroundSize: `${count * size}px ${size}px`,
            backgroundPosition: `${-index * size}px 0`,
            imageRendering: 'pixelated',
        }} />;
}

function ItemsTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const item = scan.items[sel];

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'item', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError(''); refresh();
    };

    const remove = async it => {
        if (!await confirm('Delete ' + it.name + '?')) return;
        await api('/api/delete', { path: 'resources/items/' + it.name + '.json' });
        setSel(0); refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Items</h2>
                {scan.items.map((it, i) => (
                    <div className="row" key={it.name}>
                        <ItemIcon index={i} count={scan.items.length} size={24} />
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => setSel(i)}>{it.name}</button>
                        <button className="delete" onClick={() => remove(it)}>×</button>
                    </div>
                ))}
                <h2>New</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Create</button>
                </div>
                {error && <p className="fail">{error}</p>}
            </div>
            {item
                ? <ItemEditor key={item.name} item={item} index={sel} scan={scan} onSaved={refresh} />
                : <p className="dim">No items. Create one on the left.</p>}
        </div>
    );
}

function ItemEditor({ item, index, scan, onSaved }) {
    const [data, setData] = useState(item.data);
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);

    const use = data.use || {};
    const passive = data.passive || {};
    const upgrade = data.upgrade || {};
    const cost = use.cost || {};
    const ammoCost = cost.ammo === 'arrow' || cost.ammo === 'bomb';

    const patch = (key, value) => { setData(prev => ({ ...prev, [key]: value })); setDirty(true); };
    const patchUse = (key, value) => { setData(prev => ({ ...prev, use: { ...(prev.use || {}), [key]: value } })); setDirty(true); };
    const patchPassive = (key, value) => { setData(prev => ({ ...prev, passive: { ...(prev.passive || {}), [key]: value } })); setDirty(true); };
    const patchUpgrade = (key, value) => { setData(prev => ({ ...prev, upgrade: { ...(prev.upgrade || {}), [key]: value } })); setDirty(true); };
    const patchCost = (key, value) => { setData(prev => ({ ...prev, use: { ...(prev.use || {}), cost: { ...(prev.use || {}).cost || {}, [key]: value } } })); setDirty(true); };

    const save = async () => {
        await api('/api/save', {
            path: 'resources/items/' + item.name + '.json',
            content: JSON.stringify(data, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    const numberField = (label, value, onSet) => (
        <div className="field"><label>{label}</label>
            <input type="number" value={value == null ? 0 : value}
                onChange={e => onSet(e.target.value === '' ? undefined : +e.target.value)} />
        </div>
    );

    return (
        <div className="panel" style={{ maxWidth: 560 }}>
            <h1><ItemIcon index={index} count={scan.items.length} size={28} /> {item.name} {dirty && <span className="fail">(unsaved)</span>}</h1>

            <div className="field"><label>Label (subscreen)</label>
                <input value={data.label || ''} onChange={e => patch('label', e.target.value)} />
            </div>

            <h2>Use-Action</h2>
            <div className="field"><label>Kind</label>
                <select value={use.kind || 'none'} onChange={e => patchUse('kind', e.target.value)}>
                    {USE_KINDS.map(k => <option key={k} value={k}>{k}</option>)}
                </select>
            </div>
            <div className="field"><label>Sfx</label>
                <select value={use.sfx || ''} onChange={e => patchUse('sfx', e.target.value || undefined)}>
                    <option value="">(none)</option>
                    {scan.audio.map(a => <option key={a} value={a}>{a}</option>)}
                </select>
            </div>
            {(use.kind === 'bolt' || use.kind === 'medallion') && (
                <div className="field"><label>Element</label>
                    <select value={use.element || ''} onChange={e => patchUse('element', e.target.value || undefined)}>
                        {ELEMENT_KINDS.filter(k => k !== '').map(k => <option key={k} value={k}>{k}</option>)}
                    </select>
                </div>
            )}
            {use.kind !== 'none' && (
                <div>
                    <h2>Cost</h2>
                    <div className="row">
                        {numberField('Magic (half-units)', cost.magic, v => patchCost('magic', v))}
                        <label title="Ammo cost instead of magic">
                            <input type="checkbox" checked={ammoCost}
                                onChange={e => patchCost('ammo', e.target.checked ? 'arrow' : undefined)} /> ammo</label>
                    </div>
                    {ammoCost && (
                        <div className="row">
                            <div className="field"><label>Ammo</label>
                                <select value={cost.ammo || 'arrow'} onChange={e => patchCost('ammo', e.target.value)}>
                                    <option value="arrow">arrow</option>
                                    <option value="bomb">bomb</option>
                                </select>
                            </div>
                            {numberField('Amount', cost.amount, v => patchCost('amount', v))}
                            <label title="Spend a rupee when the quiver is dry">
                                <input type="checkbox" checked={!!cost.rupee_fallback}
                                    onChange={e => patchCost('rupee_fallback', e.target.checked || undefined)} /> rupee fallback</label>
                        </div>
                    )}
                </div>
            )}
            {use.kind === 'somaria' && (
                <div className="field"><label>Spawned block type</label>
                    <select value={data.spawn || ''} onChange={e => patch('spawn', e.target.value || undefined)}>
                        <option value="">(none)</option>
                        {(scan.objectTypes || []).map(t => <option key={t.name} value={t.name}>{t.name}</option>)}
                    </select>
                </div>
            )}

            <h2>Passives</h2>
            <div className="row">
                {numberField('Light radius (lantern)', passive.light, v => patchPassive('light', v))}
                <label title="Deep water becomes swimmable"><input type="checkbox" checked={!!passive.swim}
                    onChange={e => patchPassive('swim', e.target.checked || undefined)} /> swim</label>
                <label title="Hoists heavy objects"><input type="checkbox" checked={!!passive.lift}
                    onChange={e => patchPassive('lift', e.target.checked || undefined)} /> lift</label>
                <label title="Map marks chests/bosses"><input type="checkbox" checked={!!passive.compass}
                    onChange={e => patchPassive('compass', e.target.checked || undefined)} /> compass</label>
            </div>

            <h2>Upgrade <span className="dim">(flag-gated stats)</span></h2>
            <div className="field"><label>Flag</label>
                <input placeholder="world flag" value={upgrade.flag || ''}
                    onChange={e => patchUpgrade('flag', e.target.value || undefined)} />
            </div>
            <div className="row">
                {numberField('Speed', upgrade.speed, v => patchUpgrade('speed', v))}
                {numberField('Damage', upgrade.damage, v => patchUpgrade('damage', v))}
            </div>

            <div className="row" style={{ marginTop: 16 }}>
                <button className="btn primary" onClick={save}>Save Item</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

// Object type definitions: resources/objects/types.json — each type is a
// name, a sprite image, and an interaction composition over the engine's
// OBJINT_* vocabulary. Rooms reference types by name; the generator bakes
// the masks. The save preserves keys the editor doesn't edit.
function ObjectTypesTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [types, setTypes] = useState(null);
    const [raw, setRaw] = useState(null);   // full parsed types.json
    const [sel, setSel] = useState(null);
    const [newName, setNewName] = useState('');
    const [status, setStatus] = useState('');
    const [error, setError] = useState('');

    const reload = useCallback(async () => {
        try {
            const text = await resourceText('resources/objects/types.json');
            const data = JSON.parse(text);
            setRaw(data);
            setTypes(((data.types || []).filter(t => t && t.name)).slice());
            setSel(null);
        } catch (e) {
            // No types.json: the generator's baseline set applies (baked when
            // the file is missing). Start from what the scan saw of it.
            setRaw(null);
            setTypes((scan.objectTypes || []).map(t => ({ ...t })));
        }
    }, [scan]);
    useEffect(() => { reload(); }, [reload]);

    if (!types) return <p className="dim">Loading…</p>;
    const current = sel != null ? types[sel] : null;

    const roomsUsing = name => (scan.rooms || [])
        .filter(r => ((r.raw || {}).objects || []).some(o => o.type === name))
        .map(r => r.file);

    const patch = (i, key, value) => setTypes(prev => prev.map((t, j) => j === i ? { ...t, [key]: value } : t));

    const patchInteractions = (i, name, on) => setTypes(prev => prev.map((t, j) => {
        if (j !== i) return t;
        const list = (t.interactions || []).slice();
        const at = list.indexOf(name);
        if (on && at < 0) list.push(name);
        if (!on && at >= 0) list.splice(at, 1);
        return { ...t, interactions: list };
    }));

    const add = async () => {
        const name = newName.trim().toLowerCase().replace(/\s+/g, '_');
        if (!name) return;
        if (types.some(t => t.name === name)) return setError('"' + name + '" already exists');
        setTypes(prev => prev.concat({ name, image: name + '.png', interactions: ['solid'] }));
        setSel(types.length);
        setNewName('');
        setError('');
        setStatus('');
    };

    const remove = async i => {
        const name = types[i].name;
        const rooms = roomsUsing(name);
        if (rooms.length) {
            setError('"' + name + '" is placed in ' + rooms.join(', ') + ' — remove those placements first.');
            return;
        }
        if (!await confirm('Delete object type "' + name + '"?')) return;
        setTypes(prev => prev.filter((_, j) => j !== i));
        setSel(null);
        setError('');
    };

    const save = async () => {
        const payload = { ...(raw || {}), types };
        await api('/api/save', {
            path: 'resources/objects/types.json',
            content: JSON.stringify(payload, (key, value) => value === undefined ? undefined : value, 4)
        });
        setStatus('Saved — rebuild to bake the masks.');
        refresh();
    };

    return (
        <div style={{ display: 'flex', gap: 24 }}>
            <div className="sidebar" style={{ maxWidth: 240 }}>
                <h1>Object Types</h1>
                {types.map((t, i) => (
                    <button key={t.name + i} className={'item' + (sel === i ? ' active' : '')}
                        onClick={() => { setSel(i); setError(''); }}>{t.name}</button>
                ))}
                <div className="row" style={{ marginTop: 10 }}>
                    <input placeholder="name" value={newName} style={{ width: 100 }} onChange={e => setNewName(e.target.value)} />
                    <button className="btn" onClick={add}>Add</button>
                </div>
                {error && <p className="fail">{error}</p>}
                <p className="dim" style={{ marginTop: 10 }}>
                    Types compose the engine's interaction vocabulary; rooms place them by name.
                    The sprite lives in resources/objects/ (upload on the Music &amp; Files tab).
                </p>
            </div>
            <div style={{ flex: 1, maxWidth: 640 }}>
                {!current && <p className="dim">Select a type to edit its composition.</p>}
                {current && (
                    <div>
                        <h1>{current.name}</h1>
                        <div className="field"><label>Name <span className="dim">(rooms reference it)</span></label>
                            <input value={current.name || ''} onChange={e => patch(sel, 'name', e.target.value)} />
                        </div>
                        <div className="field"><label>Image <span className="dim">(resources/objects/, placeholder if missing)</span></label>
                            <input value={current.image || ''} placeholder={current.name + '.png'}
                                onChange={e => patch(sel, 'image', e.target.value || undefined)} />
                        </div>
                        <h2>Interactions</h2>
                        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: '4px 18px', maxWidth: 560 }}>
                            {OBJINT_KINDS.map(name => (
                                <label key={name} title={name}>
                                    <input type="checkbox"
                                        checked={(current.interactions || []).includes(name)}
                                        onChange={e => patchInteractions(sel, name, e.target.checked)} />
                                    {name}
                                </label>
                            ))}
                        </div>
                        <p className="dim" style={{ marginTop: 8 }}>
                            Extras are consumed by interaction (not name): a "switch" takes mode/link, a "chest" takes
                            kind/grant, a "warp"/"stair" takes warp, and so on — set them on each placed object in Rooms.
                        </p>
                        <div className="row" style={{ marginTop: 14 }}>
                            <button className="btn primary" onClick={save}>Save</button>
                            <button className="btn delete" onClick={() => remove(sel)}>Delete</button>
                            <span className="dim">{status}</span>
                        </div>
                    </div>
                )}
            </div>
        </div>
    );
}

function TilesTab({ scan, refresh }) {
    const confirm = useConfirm();
    const tilesets = scan.tilesets || [];
    const [selSet, setSelSet] = useState(Math.min(scan.defaultTileset || 0, Math.max(tilesets.length - 1, 0)));
    const tileset = tilesets[selSet]
        || { file: '', image: '', columns: 1, tileCount: 0, tileWidth: 32, tileHeight: 32, meta: null, metaFile: null };
    const cols = tileset.columns || 1;
    const ts = tileset.tileWidth || 32;
    const rows = Math.max(1, Math.ceil((tileset.tileCount || 1) / cols));

    const [sel, setSel] = useState(1);   // 1-based tile id
    const [meta, setMeta] = useState((tileset.meta && tileset.meta.tiles) || {});
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const [img, setImg] = useState(null);
    const [imgVersion, setImgVersion] = useState(0);
    const [pending, setPending] = useState(null);   // freshly created tileset to select
    const canvasRef = useRef(null);

    // Switching sheets swaps in that tileset's metadata (discarding unsaved
    // tile edits, like the other tabs' select guards).
    const selectSet = async i => {
        if (i === selSet) return;
        if (dirty && !await confirm('Discard unsaved tile changes?')) return;
        setSelSet(i);
    };
    useEffect(() => {
        setMeta((tileset.meta && tileset.meta.tiles) || {});
        setSel(1);
        setDirty(false);
    }, [tileset.file]);

    // Select a freshly created tileset once the refreshed scan carries it.
    useEffect(() => {
        if (pending == null) return;
        const i = tilesets.findIndex(t => t.file === pending);
        if (i >= 0) setSelSet(i);
        setPending(null);
    }, [tilesets, pending]);

    useEffect(() => {
        if (!tileset.image) return;
        let alive = true;
        resourceUrl(tileset.image).then(src => {
            if (!alive) return;
            const image = new Image();
            image.onload = () => setImg(image);
            image.src = src;
        }).catch(() => {});
        return () => { alive = false; };
    }, [tileset.image, imgVersion]);

    const entry = meta[String(sel)] || {};

    const patch = (key, value) => {
        setMeta(prev => ({ ...prev, [String(sel)]: { ...(prev[String(sel)] || {}), [key]: value } }));
        setDirty(true);
    };

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'tileset', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError('');
        setPending(name.trim() + '.png');
        await refresh();
    };

    const remove = async t => {
        const refs = scan.rooms.filter(r => r.raw.tileset === t.file).length;
        const isDefault = ((tilesets[scan.defaultTileset] || {}).file || '') === t.file;
        const implicit = isDefault ? scan.rooms.filter(r => !r.raw.tileset).length : 0;
        let msg = 'Delete ' + t.file + '?';
        if (refs) msg += ' ' + refs + ' room(s) reference it and will fail the bake until repointed.';
        else if (implicit) msg += ' It is the default sheet for ' + implicit + ' room(s) without an explicit tileset.';
        if (!await confirm(msg)) return;
        await api('/api/delete', { path: 'resources/tiles/' + t.file });
        if (t.metaFile && t.metaFile !== 'tiles.json') {
            await api('/api/delete', { path: 'resources/tiles/' + t.metaFile });
        }
        setSelSet(0);
        refresh();
    };

    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0);
        // Tiles carrying metadata get an amber outline.
        for (const key of Object.keys(meta)) {
            const frame = parseInt(key, 10) - 1;
            if (isNaN(frame) || frame < 0 || frame >= tileset.tileCount) continue;
            ctx.strokeStyle = '#f59e0b';
            ctx.lineWidth = 2;
            ctx.strokeRect((frame % cols) * ts + 1, Math.floor(frame / cols) * ts + 1, ts - 2, ts - 2);
        }
        // The selection gets a white outline.
        if (sel > 0 && sel <= tileset.tileCount) {
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 3;
            ctx.strokeRect(((sel - 1) % cols) * ts + 1, Math.floor((sel - 1) / cols) * ts + 1, ts - 2, ts - 2);
        }
    }, [img, meta, sel]);

    const tileFromEvent = e => {
        const canvas = canvasRef.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / cols));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / rows));
        return (x >= 0 && y >= 0 && x < cols && y < rows) ? y * cols + x + 1 : null;
    };

    const save = async () => {
        // The sheet's behavior metadata: the file the generator reads for
        // this tileset, or a fresh <stem>.json when none exists yet.
        const metaFile = tileset.metaFile || (tileset.file.replace(/\.png$/, '') + '.json');
        await api('/api/save', {
            path: 'resources/tiles/' + metaFile,
            content: JSON.stringify({ ...(tileset.meta || {}), tiles: meta }, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        refresh();
    };

    const numberField = (label, value, onSet, title) => (
        <div className="field" title={title}><label>{label}</label>
            <input type="number" value={value == null ? 0 : value}
                onChange={e => onSet(e.target.value === '' ? undefined : +e.target.value)} />
        </div>
    );
    const checkField = (label, key, title) => (
        <label title={title || key}><input type="checkbox" checked={!!entry[key]}
            onChange={e => patch(key, e.target.checked || undefined)} /> {label}</label>
    );
    const selectField = (label, key, options, title) => (
        <div className="field" title={title}><label>{label}</label>
            <select value={entry[key] || ''} onChange={e => patch(key, e.target.value || undefined)}>
                {options.map(o => <option key={o} value={o}>{o === '' ? '(none)' : o}</option>)}
            </select>
        </div>
    );

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Tilesets</h2>
                {tilesets.map((t, i) => (
                    <div className="row" key={t.file}>
                        <button className={'item' + (i === selSet ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => selectSet(i)}>{t.file.replace(/\.png$/, '')}</button>
                        <button className="delete" onClick={() => remove(t)}>×</button>
                    </div>
                ))}
                {tilesets.length === 0 && <p className="dim">No tilesets — create one below.</p>}
                <h2>New Tileset</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Create</button>
                </div>
                {error && <p className="fail">{error}</p>}
                <p className="dim">Scaffolds the starter sheet — replace it with real art beside the canvas.
                    texture.png is the default every room paints from until it picks a sheet.</p>
            </div>
            <div style={{ flex: 1, minWidth: 0 }}>
                <h1>{tileset.file || '(no tileset)'} {dirty && <span className="fail">(unsaved)</span>}</h1>
                <div className="row">
                    <SheetUpload dir="tiles" file={tileset.file || 'texture.png'}
                        onUploaded={() => { setImgVersion(v => v + 1); refresh(); }} />
                    <span className="dim">
                        {tileset.file
                            ? 'shared by ' + scan.rooms.filter(r => (r.raw.tileset || ((tilesets[scan.defaultTileset] || {}).file || '')) === tileset.file).length
                                + ' room(s); metadata: ' + (tileset.metaFile || tileset.file.replace(/\.png$/, '') + '.json (new)')
                            : ''}
                    </span>
                </div>
                <div className="scrollWrap">
                    <canvas ref={canvasRef} width={cols * ts} height={rows * ts}
                        style={{ width: cols * ts * 2, imageRendering: 'pixelated', cursor: 'pointer', display: 'block', background: '#3a3a44' }}
                        onClick={e => { const tile = tileFromEvent(e); if (tile) setSel(tile); }} />
                </div>

                <div className="panel" style={{ maxWidth: 560, marginTop: 12 }}>
                    <h2>Tile {sel - 1} Behavior <span className="dim">(id {sel} — click a tile above; amber outlines carry metadata)</span></h2>
                    <div className="row">
                        {checkField('pit (hole)', 'pit', 'The hero falls in; with a room pit-landing, drops through')}
                        {checkField('ice', 'ice', 'Momentum sliding')}
                        {checkField('heal', 'heal', 'Fairy-fountain water: refills hearts + magic')}
                        {checkField('dig', 'dig', 'Soft soil: the shovel turns it over')}
                    </div>
                    <div className="row">
                        {selectField('Water', 'water', ['', 'deep', 'shallow'],
                            'deep = splash/bounce (swimmable with flippers); shallow = wading')}
                        {selectField('Ledge', 'ledge', ['', 'down', 'right', 'left', 'up'],
                            'One-way vault-over lip facing that direction')}
                    </div>
                    <div className="row">
                        {checkField('cuttable', 'cuttable', 'Sword swings cut it (grass)')}
                        {numberField('Cut result', entry.cut_result, v => patch('cut_result', v), 'Tile left after cutting')}
                    </div>
                    <div className="row">
                        {checkField('bombable', 'bombable', 'Bombs blast it open')}
                        {numberField('Bomb result', entry.bomb_result, v => patch('bomb_result', v),
                            'Tile left after blasting (255 = open hole)')}
                    </div>
                    <h2>Current / Conveyor</h2>
                    <div className="row">
                        {numberField('Push dx (px/s)', (entry.push || [])[0],
                            v => patch('push', [v || 0, (entry.push || [])[1] || 0]),
                            'Drift while standing on it (e.g. 48 = belt speed)')}
                        {numberField('Push dy (px/s)', (entry.push || [])[1],
                            v => patch('push', [(entry.push || [])[0] || 0, v || 0]))}
                    </div>
                    <h2>Hazard</h2>
                    <div className="row">
                        {numberField('Damage (half-hearts)', entry.damage, v => patch('damage', v),
                            'Hurts on contact with knockback')}
                    </div>
                    <div className="row" style={{ marginTop: 16 }}>
                        <button className="btn primary" onClick={save}>Save Tiles</button>
                        <span className="dim">{status}</span>
                    </div>
                </div>
            </div>
        </div>
    );
}

// World flags: the baked registry (names + where they came from), read
// through the generator's tooling mode — the bake is the single authority
// on flag ids, so this list is the reference view. Flags are referenced
// by NAME everywhere in game data (dialogue, rooms, items), so renaming
// one means renaming it in the source files — the pointers here say where.
function FlagsTab() {
    const [flags, setFlags] = useState(null);
    const [error, setError] = useState('');
    const [filter, setFilter] = useState('');

    const load = useCallback(() => {
        api('/api/list_flags').then(result => {
            if (result.error) setError(result.error);
            else { setError(''); setFlags(result); }
        });
    }, []);
    useEffect(load, [load]);

    if (error) {
        return (
            <div>
                <h1>World Flags</h1>
                <p className="fail">{error}</p>
                <button className="btn" onClick={load}>Retry</button>
            </div>
        );
    }

    const shown = (flags || []).filter(f =>
        !filter || f.name.toLowerCase().includes(filter.toLowerCase()));

    return (
        <div>
            <h1>World Flags <span className="dim">{flags ? flags.length + ' registered' : 'loading…'}</span></h1>
            <p className="dim">
                The baked registry, in id order. Flags are referenced by name in dialogue files (if/unless/set/clear),
                item upgrades, NPC presence conditions, and encounter conditions — engine-registered ones
                (boss defeats, the crystal switch, shrine flags) are set by the engine at runtime.
                Renaming a flag means renaming it everywhere it's referenced; ids are first-use order,
                so <b>adding</b> new flags is safe while <b>reordering or deleting</b> existing ones invalidates saves.
            </p>
            <div className="row" style={{ margin: '10px 0' }}>
                <input placeholder="filter…" value={filter} style={{ width: 180 }}
                    onChange={e => setFilter(e.target.value)} />
                <button className="btn" onClick={load}>Refresh</button>
            </div>
            {flags && (
                <table className="flagTable" style={{ borderCollapse: 'collapse', minWidth: 480 }}>
                    <thead>
                        <tr>
                            <th style={{ textAlign: 'left' }}>id</th>
                            <th style={{ textAlign: 'left' }}>name</th>
                            <th style={{ textAlign: 'left' }}>first registered by</th>
                        </tr>
                    </thead>
                    <tbody>
                        {shown.map((f, i) => (
                            <tr key={f.name + i}>
                                <td className="dim">{i}</td>
                                <td><code>{f.name}</code></td>
                                <td className="dim">{f.source || '(engine-registered)'}</td>
                            </tr>
                        ))}
                    </tbody>
                </table>
            )}
        </div>
    );
}

function DialogueTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const dialogue = scan.dialogues[sel];

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'dialogue', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError(''); refresh();
    };

    const remove = async d => {
        if (!await confirm('Delete ' + d.file + '?')) return;
        await api('/api/delete', { path: 'resources/dialogue/' + d.file });
        setSel(0); refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Dialogues</h2>
                {scan.dialogues.map((d, i) => (
                    <div className="row" key={d.file}>
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => setSel(i)}>{d.file}</button>
                        <button className="delete" onClick={() => remove(d)}>×</button>
                    </div>
                ))}
                <h2>New</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Create</button>
                </div>
                {error && <p className="fail">{error}</p>}
            </div>
            {dialogue
                ? <DialogueEditor key={dialogue.file} dialogue={dialogue} scan={scan} onSaved={refresh} />
                : <p className="dim">No dialogues.</p>}
        </div>
    );
}

// Dialogue scripting: a line's editable fields, structured (no raw JSON).
// Plain strings normalize to { text }; the editor writes back only the
// keys that are set, so unedited conventions survive.
const normalizeLine = l =>
    (l && typeof l === 'object') ? { ...l } : { text: String(l != null ? l : '') };
const flagList = (entry, key) => (Array.isArray(entry[key]) ? entry[key].join(', ') : (entry[key] || ''));
const parseFlagList = text => text.split(',').map(s => s.trim()).filter(Boolean);
const NUM = v => (v === '' || v == null ? undefined : +v);

function DialogueEditor({ dialogue, scan, onSaved }) {
    const rawLines = (dialogue.data.lines || []).map(normalizeLine);
    const [lines, setLines] = useState(rawLines);
    const [meta, setMeta] = useState({
        credits: !!(dialogue.data.credits),
        next: dialogue.data.next || ''
    });
    const [open, setOpen] = useState(null);          // expanded line index
    const [view, setView] = useState('lines');       // lines | tree
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);

    const labels = lines.map((l, i) => l.label ? [l.label, i] : null).filter(Boolean);
    const patchLine = (i, mut) => {
        setLines(prev => prev.map((l, j) => j === i ? mut(l) : l));
        setDirty(true);
    };
    const setLine = (i, key, value) => patchLine(i, l => ({ ...l, [key]: value }));
    const dropKey = (i, key) => patchLine(i, l => {
        const copy = { ...l };
        delete copy[key];
        return copy;
    });

    const insertLine = (template, at) => {
        const index = at != null ? at + 1 : lines.length;
        setLines(prev => {
            const copy = prev.slice();
            copy.splice(index, 0, template);
            return copy;
        });
        setOpen(index);
        setDirty(true);
    };
    const removeLine = i => {
        setLines(prev => prev.filter((_, j) => j !== i));
        setDirty(true);
    };
    const moveLine = (i, dir) => {
        const j = i + dir;
        if (j < 0 || j >= lines.length) return;
        setLines(prev => {
            const copy = prev.slice();
            const tmp = copy[i]; copy[i] = copy[j]; copy[j] = tmp;
            return copy;
        });
        setDirty(true);
    };

    const save = async () => {
        // Write back only set keys — but "" is a real value for some fields
        // (music "" stops the bgm), so only undefined/empty-array drops.
        const clean = lines.map(l => {
            const copy = {};
            for (const [k, v] of Object.entries(l)) {
                if (v === undefined) continue;
                if (Array.isArray(v) && !v.length) continue;
                copy[k] = v;
            }
            return copy;
        });
        await api('/api/save', {
            path: 'resources/dialogue/' + dialogue.file,
            content: JSON.stringify({
                ...(dialogue.data || {}),
                credits: meta.credits || undefined,
                next: meta.next || undefined,
                lines: clean
            }, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    // ---- line summary (collapsed view): shared summaryBits below -------

    return (
        <div style={{ flex: 1, minWidth: 0 }}>
            <h1>{dialogue.file} {dirty && <span className="fail">(unsaved)</span>}</h1>
            <div className="row" style={{ marginBottom: 8 }}>
                <button className={'btn' + (view === 'lines' ? ' primary' : '')} onClick={() => setView('lines')}>Lines</button>
                <button className={'btn' + (view === 'tree' ? ' primary' : '')} onClick={() => setView('tree')}>Branch tree</button>
                <span className="toolbarSep" />
                <label title="Endgame roll: centered auto-advancing lines over black">
                    <input type="checkbox" checked={meta.credits}
                        onChange={e => { setMeta(m => ({ ...m, credits: e.target.checked })); setDirty(true); }} /> credits</label>
                <div className="field" title="Chain into this dialogue when this one runs out">
                    <label>next</label>
                    <select value={meta.next} onChange={e => { setMeta(m => ({ ...m, next: e.target.value })); setDirty(true); }}>
                        <option value="">(none)</option>
                        {scan.dialogues.filter(d => d.file !== dialogue.file).map(d =>
                            <option key={d.file} value={d.file}>{d.file}</option>)}
                    </select>
                </div>
                <span className="toolbarSep" />
                <button className="btn primary" onClick={save}>Save</button>
                <span className="dim">{status}</span>
            </div>

            {view === 'tree' ? <DialogueTree lines={lines} labels={labels} /> : (
                <div>
                    <div className="row" style={{ marginBottom: 10 }}>
                        <span className="dim">insert:</span>
                        <button className="btn" onClick={() => insertLine({ text: '' })}>+ Text</button>
                        <button className="btn" onClick={() => insertLine({ text: '', label: '' })}>+ Branch (label + goto)</button>
                        <button className="btn" onClick={() => insertLine({ text: '', choice: [{ text: 'yes' }, { text: 'no' }] })}>+ Choice</button>
                        <button className="btn" title="A silent beat: auto-advances after N seconds, no box"
                            onClick={() => insertLine({ text: '', wait: 2 })}>+ Beat</button>
                        <button className="btn" title="A cutscene cue: bars, camera pan, actor blocking"
                            onClick={() => insertLine({ text: '', letterbox: true, camera: { pan: [0, 0], time: 1 }, wait: 2 })}>+ Cutscene</button>
                    </div>

                    {lines.map((line, i) => (
                        <div key={i} className="encRow" style={{
                            display: 'block', marginBottom: 6, padding: '6px 8px',
                            border: open === i ? '2px solid #3b82f6' : '1px solid #3341553d',
                            borderRadius: 6
                        }}>
                            <div className="row">
                                <span className="dim" style={{ width: 22 }}>{i + 1}.</span>
                                <span style={{ flex: 1, color: line.text ? '#e2e8f0' : '#64748b', fontStyle: line.text ? 'normal' : 'italic' }}>
                                    {line.text ? line.text.slice(0, 60) + (line.text.length > 60 ? '…' : '') : '(no text)'}
                                </span>
                                {summaryBits(line).slice(0, 3).map((s, k) =>
                                    <span key={k} className="chip dim" style={{ fontSize: 10 }}>{s}</span>)}
                                <button className="tool" onClick={() => setOpen(open === i ? null : i)}>{open === i ? '▾' : '▸'}</button>
                                <button className="tool" title="Move up" onClick={() => moveLine(i, -1)}>↑</button>
                                <button className="tool" title="Move down" onClick={() => moveLine(i, 1)}>↓</button>
                                <button className="delete" onClick={() => removeLine(i)}>×</button>
                            </div>

                            {open === i && (
                                <div style={{ borderTop: '1px solid #3341553d', marginTop: 6, paddingTop: 8 }}>
                                    <div className="field"><label>Text</label>
                                        <input style={{ width: '100%' }} value={line.text || ''}
                                            onChange={e => setLine(i, 'text', e.target.value)} />
                                    </div>
                                    <div className="row">
                                        <div className="field"><label>Label <span className="dim">(goto target)</span></label>
                                            <input value={line.label || ''} onChange={e => setLine(i, 'label', e.target.value || undefined)} />
                                        </div>
                                        <div className="field"><label>Go to label</label>
                                            <select value={line.goto || ''} onChange={e => setLine(i, 'goto', e.target.value || undefined)}>
                                                <option value="">(advance)</option>
                                                {labels.filter(([lab]) => lab !== line.label).map(([lab]) =>
                                                    <option key={lab} value={lab}>{lab}</option>)}
                                            </select>
                                        </div>
                                    </div>
                                    <div className="row">
                                        <div className="field" title="Shown only while this flag holds"><label>if flag</label>
                                            <input value={line.if || ''} onChange={e => setLine(i, 'if', e.target.value || undefined)} />
                                        </div>
                                        <div className="field" title="Shown unless this flag holds"><label>unless flag</label>
                                            <input value={line.unless || ''} onChange={e => setLine(i, 'unless', e.target.value || undefined)} />
                                        </div>
                                        <div className="field" title="Flags set when the line shows (comma-separated)"><label>set flags</label>
                                            <input value={flagList(line, 'set')} onChange={e => setLine(i, 'set', e.target.value ? parseFlagList(e.target.value) : undefined)} />
                                        </div>
                                        <div className="field" title="Flags cleared when the line shows"><label>clear flags</label>
                                            <input value={flagList(line, 'clear')} onChange={e => setLine(i, 'clear', e.target.value ? parseFlagList(e.target.value) : undefined)} />
                                        </div>
                                    </div>
                                    <div className="row">
                                        <div className="field"><label>Sfx</label>
                                            <select value={line.sfx || ''} onChange={e => setLine(i, 'sfx', e.target.value || undefined)}>
                                                <option value="">(none)</option>
                                                {scan.audio.map(a => <option key={a} value={a}>{a}</option>)}
                                            </select>
                                        </div>
                                        <div className="field"><label>Music</label>
                                            <select value={line.music == null ? '__keep' : (line.music === '' ? '__stop' : line.music)}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '__keep') dropKey(i, 'music');
                                                    else setLine(i, 'music', v === '__stop' ? '' : v);
                                                }}>
                                                <option value="__keep">(keep playing)</option>
                                                <option value="__stop">(stop the bgm)</option>
                                                {scan.music.map(m => <option key={m} value={m}>{m}</option>)}
                                            </select>
                                        </div>
                                        <div className="field"><label>Give item</label>
                                            <select value={line.give || ''} onChange={e => setLine(i, 'give', e.target.value || undefined)}>
                                                <option value="">(none)</option>
                                                {scan.items.map(it => <option key={it.name} value={it.name}>{it.name}</option>)}
                                            </select>
                                        </div>
                                        <div className="field" title="Auto-advance after N seconds; 0 = button-advance">
                                            <label>Wait (s)</label>
                                            <input type="number" value={line.wait != null ? line.wait : ''}
                                                onChange={e => e.target.value === '' ? dropKey(i, 'wait') : setLine(i, 'wait', +e.target.value)} />
                                        </div>
                                    </div>
                                    <div className="row">
                                        <div className="field"><label>Warp to room</label>
                                            <select value={(line.warp || {}).room || ''}
                                                onChange={e => {
                                                    const room = e.target.value;
                                                    if (!room) { dropKey(i, 'warp'); return; }
                                                    setLine(i, 'warp', { room, tile: (line.warp || {}).tile || [0, 0] });
                                                }}>
                                                <option value="">(no warp)</option>
                                                {scan.rooms.map(r => <option key={r.file} value={r.file}>{stem(r.file)}</option>)}
                                            </select>
                                        </div>
                                        {(line.warp || {}).room && <>
                                            <div className="field"><label>tile x,y</label>
                                                <input type="number" style={{ width: 56 }} value={(line.warp.tile || [])[0] || 0}
                                                    onChange={e => setLine(i, 'warp', { ...line.warp, tile: [+e.target.value, (line.warp.tile || [])[1] || 0] })} />
                                                <input type="number" style={{ width: 56 }} value={(line.warp.tile || [])[1] || 0}
                                                    onChange={e => setLine(i, 'warp', { ...line.warp, tile: [(line.warp.tile || [])[0] || 0, +e.target.value] })} />
                                            </div>
                                        </>}
                                    </div>

                                    <div className="row">
                                        <div className="field" title="Cinematic bars"><label>Letterbox</label>
                                            <select value={line.letterbox == null ? '' : (line.letterbox ? 'on' : 'off')}
                                                onChange={e => {
                                                    if (e.target.value === '') dropKey(i, 'letterbox');
                                                    else setLine(i, 'letterbox', e.target.value === 'on');
                                                }}>
                                                <option value="">(leave)</option>
                                                <option value="on">on</option>
                                                <option value="off">off</option>
                                            </select>
                                        </div>
                                        <div className="field" title="Scripted camera"><label>Camera</label>
                                            <select value={line.camera ? (line.camera.pan ? 'pan' : 'hero') : ''}
                                                onChange={e => {
                                                    if (e.target.value === '') dropKey(i, 'camera');
                                                    else if (e.target.value === 'hero') setLine(i, 'camera', 'hero');
                                                    else setLine(i, 'camera', { pan: (line.camera || {}).pan || [0, 0], time: (line.camera || {}).time || 1 });
                                                }}>
                                                <option value="">(follow hero)</option>
                                                <option value="pan">pan to tile…</option>
                                                <option value="hero">return to hero</option>
                                            </select>
                                        </div>
                                        {(line.camera || {}).pan && <>
                                            <div className="field"><label>pan x,y</label>
                                                <input type="number" style={{ width: 56 }} value={(line.camera.pan || [])[0] || 0}
                                                    onChange={e => setLine(i, 'camera', { ...line.camera, pan: [+e.target.value, (line.camera.pan || [])[1] || 0] })} />
                                                <input type="number" style={{ width: 56 }} value={(line.camera.pan || [])[1] || 0}
                                                    onChange={e => setLine(i, 'camera', { ...line.camera, pan: [(line.camera.pan || [])[0] || 0, +e.target.value] })} />
                                            </div>
                                            <div className="field"><label>time</label>
                                                <input type="number" style={{ width: 56 }} value={(line.camera.time != null ? line.camera.time : 1)}
                                                    onChange={e => setLine(i, 'camera', { ...line.camera, time: +e.target.value })} />
                                            </div>
                                        </>}
                                    </div>
                                    <div className="row">
                                        <div className="field" title="Actor blocking: glide the hero or an npc to a tile">
                                            <label>Move</label>
                                            <select value={line.move ? String(line.move.actor) : ''}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '') { dropKey(i, 'move'); return; }
                                                    setLine(i, 'move', { actor: v === 'hero' ? 'hero' : +v, tile: (line.move || {}).tile || [0, 0], time: (line.move || {}).time || 1 });
                                                }}>
                                                <option value="">(no blocking)</option>
                                                <option value="hero">the hero</option>
                                                {Array.from({ length: 8 }, (_, n) =>
                                                    <option key={n} value={String(n)}>npc #{n}</option>)}
                                            </select>
                                        </div>
                                        {line.move && <>
                                            <div className="field"><label>to x,y</label>
                                                <input type="number" style={{ width: 56 }} value={(line.move.tile || [])[0] || 0}
                                                    onChange={e => setLine(i, 'move', { ...line.move, tile: [+e.target.value, (line.move.tile || [])[1] || 0] })} />
                                                <input type="number" style={{ width: 56 }} value={(line.move.tile || [])[1] || 0}
                                                    onChange={e => setLine(i, 'move', { ...line.move, tile: [(line.move.tile || [])[0] || 0, +e.target.value] })} />
                                            </div>
                                            <div className="field"><label>time</label>
                                                <input type="number" style={{ width: 56 }} value={(line.move.time != null ? line.move.time : 1)}
                                                    onChange={e => setLine(i, 'move', { ...line.move, time: +e.target.value })} />
                                            </div>
                                        </>}
                                    </div>
                                    <div className="row">
                                        <div className="field" title="Turn an actor to speak without a glide (move already faces the stride)">
                                            <label>Face</label>
                                            <select value={line.face ? String(line.face.actor) : ''}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '') { dropKey(i, 'face'); return; }
                                                    setLine(i, 'face', { actor: v === 'hero' ? 'hero' : +v, dir: (line.face || {}).dir || 'down' });
                                                }}>
                                                <option value="">(no turn)</option>
                                                <option value="hero">the hero</option>
                                                {Array.from({ length: 8 }, (_, n) =>
                                                    <option key={n} value={String(n)}>npc #{n}</option>)}
                                            </select>
                                        </div>
                                        {line.face && <>
                                            <div className="field"><label>dir</label>
                                                <select value={line.face.dir || 'down'}
                                                    onChange={e => setLine(i, 'face', { ...line.face, dir: e.target.value })}>
                                                    {DIRECTIONS.map(d => <option key={d} value={d}>{d}</option>)}
                                                </select>
                                            </div>
                                        </>}
                                        <div className="field" title="Mid-scene entrance: the actor (re)spawns at its spawn tile (npc) or steps back into the world (hero)">
                                            <label>Show</label>
                                            <select value={line.show == null ? '' : String(line.show)}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '') dropKey(i, 'show');
                                                    else setLine(i, 'show', v === 'hero' ? 'hero' : +v);
                                                }}>
                                                <option value="">(none)</option>
                                                <option value="hero">the hero</option>
                                                {Array.from({ length: 8 }, (_, n) =>
                                                    <option key={n} value={String(n)}>npc #{n}</option>)}
                                            </select>
                                        </div>
                                        <div className="field" title="Mid-scene exit: the actor steps off-stage until the next room entry">
                                            <label>Hide</label>
                                            <select value={line.hide == null ? '' : String(line.hide)}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '') dropKey(i, 'hide');
                                                    else setLine(i, 'hide', v === 'hero' ? 'hero' : +v);
                                                }}>
                                                <option value="">(none)</option>
                                                <option value="hero">the hero</option>
                                                {Array.from({ length: 8 }, (_, n) =>
                                                    <option key={n} value={String(n)}>npc #{n}</option>)}
                                            </select>
                                        </div>
                                    </div>
                                    <div className="row">
                                        <div className="field" title="Scripted screen fade: cuts for image scenes, punctuation for in-world ones">
                                            <label>Fade</label>
                                            <select value={line.fade ? line.fade.to : ''}
                                                onChange={e => {
                                                    const v = e.target.value;
                                                    if (v === '') { dropKey(i, 'fade'); return; }
                                                    setLine(i, 'fade', { to: v, time: (line.fade || {}).time != null ? line.fade.time : 0.5 });
                                                }}>
                                                <option value="">(no fade)</option>
                                                <option value="black">to black…</option>
                                                <option value="world">back to the world…</option>
                                            </select>
                                        </div>
                                        {line.fade && <>
                                            <div className="field"><label>time</label>
                                                <input type="number" style={{ width: 56 }} value={line.fade.time != null ? line.fade.time : 0.5}
                                                    onChange={e => setLine(i, 'fade', { ...line.fade, time: +e.target.value })} />
                                            </div>
                                        </>}
                                    </div>

                                    <h2 style={{ marginTop: 8 }}>Choices <span className="dim">(action opens the list; the pick decides where the script goes)</span></h2>
                                    {(line.choice || []).map((ch, ci) => (
                                        <div className="row" key={ci} style={{ marginBottom: 4 }}>
                                            <input placeholder="text" style={{ width: 120 }} value={ch.text || ''}
                                                onChange={e => setLine(i, 'choice', line.choice.map((c, j) => j === ci ? { ...c, text: e.target.value } : c))} />
                                            <div className="field" title="Jump to this label on pick"><label>goto</label>
                                                <select value={ch.goto || ''} onChange={e => setLine(i, 'choice', line.choice.map((c, j) =>
                                                    j === ci ? { ...c, goto: e.target.value || undefined } : c))}>
                                                    <option value="">(advance)</option>
                                                    {labels.map(([lab]) => <option key={lab} value={lab}>{lab}</option>)}
                                                </select>
                                            </div>
                                            <div className="field" title="Shop pick: deducts rupees"><label>cost</label>
                                                <input type="number" style={{ width: 56 }} value={ch.cost || ''}
                                                    onChange={e => setLine(i, 'choice', line.choice.map((c, j) =>
                                                        j === ci ? { ...c, cost: NUM(e.target.value) } : c))} />
                                            </div>
                                            <div className="field" title="Grant this item on pick"><label>give</label>
                                                <select value={ch.give || ''} onChange={e => setLine(i, 'choice', line.choice.map((c, j) =>
                                                    j === ci ? { ...c, give: e.target.value || undefined } : c))}>
                                                    <option value="">(none)</option>
                                                    {scan.items.map(it => <option key={it.name} value={it.name}>{it.name}</option>)}
                                                </select>
                                            </div>
                                            <button className="delete" onClick={() => setLine(i, 'choice', line.choice.filter((_, j) => j !== ci))}>×</button>
                                        </div>
                                    ))}
                                    <div className="row">
                                        <button className="btn" onClick={() => setLine(i, 'choice', [...(line.choice || []), { text: '' }])}>+ Choice</button>
                                        {line.choice && line.choice.length > 0 &&
                                            <button className="btn" onClick={() => dropKey(i, 'choice')}>Remove choices</button>}
                                    </div>
                                </div>
                            )}
                        </div>
                    ))}
                </div>
            )}
        </div>
    );
}

// Branch/choice tree: a read-only control-flow map of the script. Nodes
// stack vertically in line order; edges show the flow — default advance
// (gray), goto jumps (blue), choice splits (orange, one per option).
function DialogueTree({ lines, labels }) {
    const NODE_H = 54, NODE_W = 300, GAP = 10, X = 130;
    const height = lines.length * (NODE_H + GAP) + 20;
    const labelIndex = Object.fromEntries(labels);
    const nodeY = i => 10 + i * (NODE_H + GAP);

    const edges = [];
    lines.forEach((line, i) => {
        const y = nodeY(i);
        if (line.choice && line.choice.length) {
            (line.choice).forEach((ch, ci) => {
                const target = ch.goto != null ? labelIndex[ch.goto] : i + 1;
                edges.push([i, target == null ? null : target, '#f97316',
                    (ch.text || 'option ' + (ci + 1)).slice(0, 18)]);
            });
        } else if (line.goto != null) {
            const target = labelIndex[line.goto];
            edges.push([i, target == null ? null : target, '#3b82f6', 'goto ' + line.goto]);
        } else if (i + 1 < lines.length) {
            edges.push([i, i + 1, '#475569', null]);
        }
    });

    return (
        <div className="graphWrap">
            <svg width={NODE_W + 260} height={height} style={{ display: 'block' }}>
                <defs>
                    <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
                        <path d="M 0 0 L 10 5 L 0 10 z" fill="#64748b" />
                    </marker>
                </defs>
                {edges.map(([from, to, color, label], k) => {
                    if (to == null) return null;
                    const y0 = nodeY(from) + NODE_H / 2;
                    const y1 = nodeY(to) + NODE_H / 2;
                    const bend = to <= from ? -40 : 0;   // loops bend back
                    const d = `M ${X} ${y0} C ${X - 90 + bend} ${y0}, ${X - 90 + bend} ${y1}, ${X} ${y1}`;
                    return (
                        <g key={k}>
                            <path d={d} fill="none" stroke={color} strokeWidth={2} markerEnd="url(#arrow)" opacity={to <= from ? 0.9 : 0.55} />
                            {label && to <= from && (
                                <text x={X - 100 + bend} y={(y0 + y1) / 2} fill="#94a3b8" fontSize={10} textAnchor="end">{label}</text>
                            )}
                        </g>
                    );
                })}
                {lines.map((line, i) => {
                    const y = nodeY(i);
                    const bits = summaryBits(line);
                    return (
                        <g key={i}>
                            <rect x={X} y={y} width={NODE_W} height={NODE_H} rx={6}
                                fill={line.choice ? '#7c2d12' : '#1e293b'} stroke="#475569" strokeWidth={1.5} />
                            <text x={X + 10} y={y + 20} fill="#e2e8f0" fontSize={12} fontWeight="bold">
                                {i + 1}. {line.label ? <tspan fill="#3b82f6">[{line.label}] </tspan> : null}
                                {(line.text || '(no text)').slice(0, 34)}
                            </text>
                            <text x={X + 10} y={y + 38} fill="#94a3b8" fontSize={10}>
                                {bits.slice(0, 5).join(' · ')}
                            </text>
                        </g>
                    );
                })}
            </svg>
        </div>
    );
}
const summaryBits = line => {
    const bits = [];
    if (line.choice) bits.push(line.choice.length + ' choices');
    if (line.goto) bits.push('goto ' + line.goto);
    if (line.if) bits.push('if ' + line.if);
    if (line.unless) bits.push('unless ' + line.unless);
    if (line.wait != null) bits.push('beat ' + line.wait + 's');
    if (line.camera) bits.push('camera');
    if (line.letterbox != null) bits.push('bars');
    if (line.move) bits.push('blocking');
    if (line.face) bits.push('face ' + (line.face.dir || 'down'));
    if (line.show != null) bits.push('show');
    if (line.hide != null) bits.push('hide');
    if (line.fade) bits.push('fade ' + line.fade.to);
    if (line.warp) bits.push('warp');
    if (line.music != null) bits.push('music');
    if (line.sfx) bits.push('sfx');
    if (line.give) bits.push('give');
    if (line.set) bits.push('set ' + line.set.join(','));
    if (line.clear) bits.push('clear ' + line.clear.join(','));
    return bits;
};

function CharactersTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const character = scan.characters[sel];

    const create = async () => {
        if (!name.trim()) return;
        const result = await api('/api/create', { type: 'character', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError(''); refresh();
    };

    const remove = async c => {
        if (!await confirm('Delete ' + c.file + '? (the .png is kept)')) return;
        await api('/api/delete', { path: 'resources/characters/' + c.file });
        setSel(0); refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Characters</h2>
                {scan.characters.map((c, i) => (
                    <div className="row" key={c.file}>
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => setSel(i)}>{c.file}</button>
                        <button className="delete" onClick={() => remove(c)}>×</button>
                    </div>
                ))}
                <h2>New</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Create</button>
                </div>
                {error && <p className="fail">{error}</p>}
            </div>
            {character
                ? <CharacterEditor key={character.file} character={character} onSaved={refresh} />
                : <p className="dim">No characters. Create one, then upload a same-named .png in Music & Files.</p>}
        </div>
    );
}

// Must match MOVE_ANIM_FPS / STAND_ANIM_FPS in src/hero.cpp so the preview
// plays at the same speed the engine will. Action channels animate at the
// walk cadence; the rest breathe at the stand pace.
const ANIM_FPS = { stand: 2, move: 12, attack: 12, lift: 2, swim: 12, hurt: 2 };
// Character animation channels: the base pair plus the optional state
// blocks the hero falls back from (empty = use stand/move).
const CHAR_KINDS = ['stand', 'move', 'attack', 'lift', 'swim', 'hurt'];
const STRIP_CELL = 44;

function CharacterEditor({ character, onSaved }) {
    const [data, setData] = useState(character.data);
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);
    const [anim, setAnim] = useState({ kind: 'move', dir: 'down' });
    const [slot, setSlot] = useState(null);
    const [hoverFrame, setHoverFrame] = useState(null);
    const [hoverCell, setHoverCell] = useState(null);
    const [playing, setPlaying] = useState(true);
    const [previewIdx, setPreviewIdx] = useState(0);
    const [img, setImg] = useState(null);
    const [imgVersion, setImgVersion] = useState(0);
    const sheetRef = useRef(null);
    const stripRef = useRef(null);
    const previewRef = useRef(null);
    const pngPath = character.png ? 'resources/characters/' + stem(character.file) + '.png' : null;

    useEffect(() => {
        if (!pngPath) { setImg(null); return; }
        let alive = true;
        resourceUrl(pngPath).then(src => {
            if (!alive) return;
            const image = new Image();
            image.onload = () => setImg(image);
            image.src = src;
        }).catch(() => {});
        return () => { alive = false; };
    }, [pngPath, imgVersion]);

    // Same column logic as generate_assets.py: json width wins, else png width.
    const cols = data.width > 0 ? data.width : (img ? Math.floor(img.width / 32) : 1);
    const rows = img ? Math.ceil(img.height / 32) : 0;
    const frames = ((data[anim.kind] || {})[anim.dir] || []);

    const drawFrame = (ctx, frame, x, y, size) => {
        ctx.imageSmoothingEnabled = false;
        ctx.drawImage(img, (frame % cols) * 32, Math.floor(frame / cols) * 32, 32, 32, x, y, size, size);
    };

    // ---- sheet -----------------------------------------------------------

    useEffect(() => {
        const canvas = sheetRef.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0);

        ctx.strokeStyle = 'rgba(255,255,255,0.18)';
        ctx.lineWidth = 1;
        for (let x = 0; x <= cols; x++) { ctx.beginPath(); ctx.moveTo(x * 32 + 0.5, 0); ctx.lineTo(x * 32 + 0.5, rows * 32); ctx.stroke(); }
        for (let y = 0; y <= rows; y++) { ctx.beginPath(); ctx.moveTo(0, y * 32 + 0.5); ctx.lineTo(cols * 32, y * 32 + 0.5); ctx.stroke(); }

        ctx.font = '10px sans-serif';
        ctx.textAlign = 'left';
        ctx.textBaseline = 'top';
        ctx.fillStyle = 'rgba(255,255,255,0.65)';
        for (let i = 0; i < cols * rows; i++) ctx.fillText(i, (i % cols) * 32 + 3, Math.floor(i / cols) * 32 + 2);

        // Frames used by the current animation: orange outline + position badges.
        const where = {};
        frames.forEach((f, i) => { (where[f] = where[f] || []).push(i); });
        for (const f of Object.keys(where).map(Number)) {
            const sx = (f % cols) * 32, sy = Math.floor(f / cols) * 32;
            ctx.strokeStyle = '#f97316';
            ctx.lineWidth = 2;
            ctx.strokeRect(sx + 1, sy + 1, 30, 30);
            where[f].forEach((seq, n) => {
                ctx.fillStyle = '#f97316';
                ctx.fillRect(sx + 31 - 12 * (n + 1), sy + 20, 12, 11);
                ctx.fillStyle = '#fff';
                ctx.font = 'bold 9px sans-serif';
                ctx.textAlign = 'right';
                ctx.textBaseline = 'bottom';
                ctx.fillText(String(seq + 1), sx + 30 - 12 * n, sy + 30);
            });
        }

        if (hoverFrame != null && !(hoverFrame in where)) {
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 2;
            ctx.strokeRect((hoverFrame % cols) * 32 + 1, Math.floor(hoverFrame / cols) * 32 + 1, 30, 30);
        }
    }, [img, frames, hoverFrame, cols, rows]);

    const sheetCell = e => {
        const canvas = sheetRef.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / cols));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / rows));
        return (x >= 0 && y >= 0 && x < cols && y < rows) ? y * cols + x : null;
    };

    // ---- frame list manipulation ------------------------------------------

    const insertFrame = frame => {
        setData(prev => {
            const list = (((prev[anim.kind] || {})[anim.dir]) || []).slice();
            list.splice(slot == null ? list.length : Math.min(slot, list.length), 0, frame);
            return { ...prev, [anim.kind]: { ...(prev[anim.kind] || {}), [anim.dir]: list } };
        });
        if (slot != null) setSlot(s => s + 1);
        setDirty(true);
    };

    const removeFrame = i => {
        setData(prev => {
            const list = (((prev[anim.kind] || {})[anim.dir]) || []).filter((_, j) => j !== i);
            return { ...prev, [anim.kind]: { ...(prev[anim.kind] || {}), [anim.dir]: list } };
        });
        if (slot != null && slot > i) setSlot(s => s - 1);
        setDirty(true);
    };

    const clearAnim = () => {
        setData(prev => ({ ...prev, [anim.kind]: { ...(prev[anim.kind] || {}), [anim.dir]: [] } }));
        setSlot(null);
        setDirty(true);
    };

    const setFrames = (kind, dir, text) => {
        const values = text.split(',').map(s => parseInt(s.trim(), 10)).filter(n => !isNaN(n));
        setData(prev => ({ ...prev, [kind]: { ...(prev[kind] || {}), [dir]: values } }));
        setDirty(true);
    };

    // Delete key removes the frame at the selected insert point.
    useEffect(() => {
        const onKey = e => {
            const tag = (e.target && e.target.tagName) || '';
            if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
            if ((e.key === 'Delete' || e.key === 'Backspace') && slot != null && slot < frames.length) removeFrame(slot);
        };
        window.addEventListener('keydown', onKey);
        return () => window.removeEventListener('keydown', onKey);
    }, [slot, frames]);

    // ---- sequence strip ----------------------------------------------------

    useEffect(() => {
        const canvas = stripRef.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        frames.forEach((f, i) => {
            const x = i * STRIP_CELL;
            ctx.fillStyle = '#3c3c46';
            ctx.fillRect(x + 2, 2, 40, 40);
            drawFrame(ctx, f, x + 6, 6, 32);
            ctx.fillStyle = 'rgba(255,255,255,0.7)';
            ctx.font = '10px sans-serif';
            ctx.textAlign = 'left';
            ctx.textBaseline = 'top';
            ctx.fillText(String(i + 1), x + 3, 0);
            if (slot === i) { ctx.strokeStyle = '#2563eb'; ctx.lineWidth = 3; ctx.strokeRect(x + 1, 1, 42, 42); }
            if (hoverCell === i) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.strokeRect(x + 3, 3, 38, 38); }
        });
        const x = frames.length * STRIP_CELL;
        const active = slot == null || slot === frames.length;
        ctx.strokeStyle = active ? '#2563eb' : '#999';
        ctx.lineWidth = 2;
        ctx.setLineDash([4, 3]);
        ctx.strokeRect(x + 1, 1, 42, 42);
        ctx.setLineDash([]);
        ctx.fillStyle = active ? '#2563eb' : '#999';
        ctx.font = 'bold 20px sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('+', x + 22, 23);
    }, [img, frames, slot, hoverCell, cols]);

    const stripCell = e => {
        const canvas = stripRef.current, rect = canvas.getBoundingClientRect();
        const i = Math.floor((e.clientX - rect.left) / (rect.width / (frames.length + 1)));
        return (i >= 0 && i <= frames.length) ? i : null;
    };

    // ---- animated preview ---------------------------------------------------

    useEffect(() => { setPreviewIdx(0); }, [anim, img]);

    useEffect(() => {
        if (!playing) return;
        const id = setInterval(() => setPreviewIdx(i => i + 1), 1000 / (ANIM_FPS[anim.kind] || 8));
        return () => clearInterval(id);
    }, [playing, anim.kind]);

    useEffect(() => {
        const canvas = previewRef.current;
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        if (!img || frames.length === 0) return;
        drawFrame(ctx, frames[previewIdx % frames.length], 0, 0, canvas.width);
    }, [img, frames, previewIdx, cols]);

    // ---- save ---------------------------------------------------------------

    const save = async () => {
        await api('/api/save', {
            path: 'resources/characters/' + character.file,
            content: JSON.stringify(data, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    return (
        <div style={{ flex: 1 }}>
            <h1>{character.file}{!character.png && <span className="fail"> (missing {stem(character.file)}.png)</span>}
                {dirty && <span className="fail"> (unsaved changes)</span>}</h1>
            <div className="row">
                <SheetUpload dir="characters" file={character.file}
                    onUploaded={() => { setImgVersion(v => v + 1); onSaved(); }} />
                {img && <img className="sheet" src={img.src} style={{ maxHeight: 56 }} alt="" />}
            </div>
            <div className="field"><label>Width (tiles)</label>
                <input type="number" value={data.width != null ? data.width : ''}
                    placeholder={img ? String(Math.floor(img.width / 32)) : 'auto'}
                    onChange={e => setData(prev => ({ ...prev, width: e.target.value === '' ? undefined : +e.target.value }))} />
                <span className="dim">columns in the sheet; blank derives it from the png</span>
            </div>

            {img ? (
                <div>
                    <div className="charLayout">
                        <div style={{ flex: 1, minWidth: 0 }}>
                            <h2>Sprite Sheet <span className="dim">— click a tile to add it to the animation</span></h2>
                            <div className="scrollWrap" style={{ display: 'inline-block', maxWidth: 560 }}>
                                <canvas ref={sheetRef} width={img.width} height={img.height}
                                    style={{ width: img.width * 2, imageRendering: 'pixelated', cursor: 'pointer', display: 'block' }}
                                    onMouseMove={e => setHoverFrame(sheetCell(e))}
                                    onMouseLeave={() => setHoverFrame(null)}
                                    onClick={e => { const f = sheetCell(e); if (f != null) insertFrame(f); }} />
                            </div>
                            <p className="dim">{hoverFrame != null ? 'Frame ' + hoverFrame
                                : 'Numbers top-left are frame indices; orange badges show the order position of tiles used in the current animation.'}</p>
                        </div>
                        <div className="charSide">
                            <h2>Animation</h2>
                            <div className="animGrid">
                                {CHAR_KINDS.map(kind => (
                                    <div className="animRow" key={kind}>
                                        <span>{kind}</span>
                                        {DIRECTIONS.map(dir => (
                                            <button key={dir}
                                                className={'tool' + (anim.kind === kind && anim.dir === dir ? ' active' : '')}
                                                onClick={() => { setAnim({ kind, dir }); setSlot(null); }}>{dir}</button>
                                        ))}
                                    </div>
                                ))}
                            </div>
                            <p className="dim">attack/lift/swim/hurt are optional — empty ones fall back to
                                stand/move (duplicate stand frames there for placeholder art).</p>
                            <p className="dim">stand and lift play at {ANIM_FPS.stand} fps, the rest at {ANIM_FPS.move} fps — the engine's speeds.</p>
                            <h2>Preview</h2>
                            <div className="row">
                                <canvas ref={previewRef} width={64} height={64}
                                    style={{ imageRendering: 'pixelated', background: '#3c3c46', border: '1px solid #999' }} />
                                <button className="btn" onClick={() => setPlaying(p => !p)}>{playing ? 'Pause' : 'Play'}</button>
                            </div>
                            <p className="dim">{frames.length} frame(s){frames.length === 0 && ' — click tiles in the sheet to build the sequence'}</p>
                            <button className="btn" onClick={clearAnim}>Clear this animation</button>
                        </div>
                    </div>

                    <h2>Frame Sequence <span className="dim">— click a slot to set the insert point, right-click to remove a frame</span></h2>
                    <div className="scrollWrap" style={{ maxWidth: '100%' }}>
                        <canvas ref={stripRef} width={(frames.length + 1) * STRIP_CELL} height={46}
                            style={{ imageRendering: 'pixelated', cursor: 'pointer', display: 'block' }}
                            onMouseMove={e => setHoverCell(stripCell(e))}
                            onMouseLeave={() => setHoverCell(null)}
                            onClick={e => {
                                const i = stripCell(e);
                                if (i == null) return;
                                setSlot(i === frames.length ? null : i);
                            }}
                            onContextMenu={e => {
                                e.preventDefault();
                                const i = stripCell(e);
                                if (i != null && i < frames.length) removeFrame(i);
                            }} />
                    </div>
                    <p className="dim">{frames.length ? 'Indices: ' + frames.join(', ') : '(empty)'}</p>
                </div>
            ) : (
                <p className="dim">Upload a sheet above ({stem(character.file)}.png) to use the graphical frame picker. Frames can still be typed below.</p>
            )}

            <h2>Raw Frame List <span className="dim">({anim.kind}.{anim.dir}, comma separated indices)</span></h2>
            <div className="field">
                <label>{anim.kind}.{anim.dir}</label>
                <input style={{ width: 320 }} value={frames.join(', ')}
                    onChange={e => setFrames(anim.kind, anim.dir, e.target.value)} />
            </div>

            <div className="row" style={{ marginTop: 12 }}>
                <button className="btn primary" onClick={save}>Save Character</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

function FilesTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [dir, setDir] = useState('music');
    const [status, setStatus] = useState('');

    const upload = e => {
        const file = e.target.files[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = async () => {
            const result = await api('/api/upload', { dir, name: file.name, data: String(reader.result).split(',')[1] });
            setStatus(result.error || 'Uploaded ' + file.name);
            e.target.value = '';
            refresh();
        };
        reader.readAsDataURL(file);
    };

    const FileList = ({ title, dirName, files }) => (
        <div>
            <h2>{title}</h2>
            {files.length === 0 && <p className="dim">(empty)</p>}
            {files.map(f => (
                <div className="row" key={f}>
                    <span style={{ minWidth: 200 }}>{f}</span>
                    <button className="delete" onClick={async () => {
                        if (await confirm('Delete ' + dirName + '/' + f + '?')) {
                            await api('/api/delete', { path: 'resources/' + dirName + '/' + f });
                            refresh();
                        }
                    }}>×</button>
                </div>
            ))}
        </div>
    );

    // The artist's worklist: everything the generator would synthesize a
    // placeholder for (it never overwrites real files). Importing a real
    // file at the exact path replaces the placeholder at the next bake.
    const audit = [];
    for (const c of scan.characters) if (!c.png) audit.push(['characters', stem(c.file) + '.png', 'placeholder sheet (name-hued cloak blob)']);
    for (const en of (scan.enemies || [])) if (!en.png) audit.push(['enemies', en.name + '.png', 'placeholder sheet (tinted from the enemy json color)']);
    for (const t of (scan.objectTypes || [])) if (!t.png) audit.push(['objects', t.image || (t.name + '.png'), 'placeholder frame']);
    for (const f of ['border.png', 'hearts.png', 'cursor.png', 'map.png', 'items.png',
                     'bottles.png', 'misc.png', 'bar_boss.png', 'bar_magic.png',
                     'gauge_magic.png', 'counters.png'])
        if (!scan.ui.includes(f)) audit.push(['ui', f, 'placeholder UI bitmap']);
    if (!scan.pickups.some(f => f.startsWith('pickups'))) audit.push(['pickups', 'pickups.png', 'placeholder pickup frames']);
    for (const f of ['beam.png', 'boomerang.png']) if (!scan.effects.includes(f)) audit.push(['effects', f, 'placeholder effect frames']);
    if (!scan.music.length) audit.push(['music', '(any .xm/.mod/.wav)', 'no music yet — the rooms pick tracks by name']);
    const AuditRow = ({ d, file, note }) => (
        <div className="row" style={{ marginBottom: 4 }}>
            <span className="dim" style={{ width: 90 }}>resources/{d}/</span>
            <span style={{ minWidth: 220 }}>{file}</span>
            <span className="dim" style={{ flex: 1 }}>{note}</span>
            <label className="btn" title={'Upload into resources/' + d + '/ — name the file exactly ' + (file.includes('.') ? file : 'anything')}>
                Import<input type="file" style={{ display: 'none' }} onChange={e => {
                    const f = e.target.files[0];
                    if (!f) return;
                    const reader = new FileReader();
                    reader.onload = async () => {
                        const result = await api('/api/upload', { dir: d, name: f.name, data: String(reader.result).split(',')[1] });
                        setStatus(result.error || ('Uploaded ' + f.name + (f.name !== file && file.includes('.') ? ' — expected ' + file : '')));
                        refresh();
                    };
                    reader.readAsDataURL(f);
                }} /></label>
        </div>
    );

    return (
        <div>
            <h1>Music & Files</h1>
            <div className="field">
                <label>Upload to</label>
                <select value={dir} onChange={e => setDir(e.target.value)}>
                    <option value="music">resources/music</option>
                    <option value="audio">resources/audio</option>
                    <option value="ui">resources/ui</option>
                    <option value="tiles">resources/tiles</option>
                    <option value="characters">resources/characters</option>
                    <option value="enemies">resources/enemies</option>
                    <option value="pickups">resources/pickups</option>
                    <option value="effects">resources/effects</option>
                    <option value="objects">resources/objects</option>
                    <option value="fonts">resources/fonts</option>
                </select>
                <input type="file" onChange={upload} />
                <span className="dim">{status}</span>
            </div>
            <div className="panel" style={{ marginTop: 16 }}>
                <h2>Asset audit <span className="dim">(placeholder art — import real files at these paths)</span></h2>
                {audit.length === 0 && <p className="dim">Everything present — no placeholder art.</p>}
                {audit.map(([d, file, note], i) => <AuditRow key={i} d={d} file={file} note={note} />)}
            </div>
            <FileList title="Music (resources/music)" dirName="music" files={scan.music} />
            <FileList title="Audio (resources/audio)" dirName="audio" files={scan.audio} />
            <FileList title="UI (resources/ui)" dirName="ui" files={scan.ui} />
            <FileList title="Pickups (resources/pickups)" dirName="pickups" files={scan.pickups} />
            <FileList title="Effects (resources/effects)" dirName="effects" files={scan.effects} />
            <FileList title="Objects (resources/objects)" dirName="objects" files={scan.objects} />
            <FileList title="Fonts (resources/fonts)" dirName="fonts" files={scan.fonts} />
        </div>
    );
}

function GameIdentity({ scan, onSaved }) {
    const [data, setData] = useState(scan.game || { title: '', slug: '' });
    const [status, setStatus] = useState('');
    const [dirty, setDirty] = useState(false);

    const save = async () => {
        await api('/api/save', {
            path: 'resources/game.json',
            content: JSON.stringify(data, null, 4)
        });
        setDirty(false);
        setStatus('Saved');
        onSaved();
    };

    return (
        <div className="panel" style={{ maxWidth: 480 }}>
            <h2>Game Identity {dirty && <span className="fail">(unsaved)</span>}</h2>
            <div className="field"><label>Title</label>
                <input value={data.title || ''} onChange={e => { setData(prev => ({ ...prev, title: e.target.value })); setDirty(true); }} />
            </div>
            <div className="field"><label>Slug <span className="dim">(save identity — changing it orphans existing saves)</span></label>
                <input value={data.slug || ''} onChange={e => { setData(prev => ({ ...prev, slug: e.target.value })); setDirty(true); }} />
            </div>
            <div className="field"><label>Intro <span className="dim">(a fresh game plays this script at the first room drop — never on continue)</span></label>
                <select value={data.intro || ''}
                    onChange={e => { setData(prev => ({ ...prev, intro: e.target.value || undefined })); setDirty(true); }}>
                    <option value="">(no intro)</option>
                    {(scan.dialogues || []).map(d => <option key={d.file} value={d.file}>{d.file}</option>)}
                </select>
            </div>
            <div className="row">
                <button className="btn" onClick={save}>Save</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

// ---- game code (resources/code/*.cpp) ---------------------------------------

// Game-code behaviors, editable in the window: the file list and editor
// live here (scaffolding and per-enemy references stay on the Enemies
// tab). Saving writes through the same confined-to-resources/ backend
// command; the compile check runs the engine's syntax-only pass.
function CodeTab({ scan, refresh }) {
    const confirm = useConfirm();
    const [sel, setSel] = useState(0);
    const [name, setName] = useState('');
    const [error, setError] = useState('');
    const [dirty, setDirty] = useState(false);
    const [pending, setPending] = useState(null);
    const files = scan.codeFiles || [];
    const file = files[Math.min(sel, files.length - 1)];

    // Select a freshly scaffolded file once the refreshed scan carries it.
    useEffect(() => {
        if (pending == null) return;
        const i = files.findIndex(f => f === pending);
        if (i >= 0) setSel(i);
        setPending(null);
    }, [files, pending]);

    const select = async i => {
        if (dirty && !await confirm('Discard unsaved changes to ' + file + '?')) return;
        setSel(i);
    };

    const create = async () => {
        if (!name.trim()) return;
        if (dirty && !await confirm('Discard unsaved changes to ' + file + '?')) return;
        const result = await api('/api/create', { type: 'behavior', name: name.trim() });
        if (result.error) return setError(result.error);
        setName(''); setError('');
        setPending(name.trim() + '.cpp');
        await refresh();
    };

    const remove = async f => {
        if (dirty && f === file && !await confirm('Discard unsaved changes to ' + f + '?')) return;
        if (!await confirm('Delete ' + f + '? Enemies referencing "custom:' + f.replace(/\.cpp$/, '') + '" will fail the manifest check until removed.')) return;
        await api('/api/delete', { path: 'resources/code/' + f });
        setSel(0);
        refresh();
    };

    return (
        <div className="layout">
            <div className="sidebar">
                <h2>Game Code</h2>
                {files.map((f, i) => (
                    <div className="row" key={f}>
                        <button className={'item' + (i === sel ? ' active' : '')} style={{ flex: 1 }}
                            onClick={() => select(i)}>{f}</button>
                        <button className="delete" onClick={() => remove(f)}>×</button>
                    </div>
                ))}
                {files.length === 0 && <p className="dim">No behaviors yet.</p>}
                <h2>New Behavior</h2>
                <div className="row">
                    <input placeholder="name" value={name} style={{ width: 92 }} onChange={e => setName(e.target.value)} />
                    <button className="btn" onClick={create}>Scaffold</button>
                </div>
                {error && <p className="fail">{error}</p>}
                <p className="dim" style={{ marginTop: 10 }}>
                    Compiled into the binary; enemies reference them as
                    <code>custom:&lt;name&gt;</code>. Registered behaviors:
                    {(scan.behaviors || []).map(b => ' ' + b).toString() || ' none'}
                </p>
            </div>
            {file
                ? <CodeEditor key={file} file={file} dirty={dirty} onSaved={refresh} onDirty={setDirty} />
                : <p className="dim">Scaffold a behavior to start writing game code.</p>}
        </div>
    );
}

function CodeEditor({ file, dirty, onSaved, onDirty }) {
    const [text, setText] = useState('');
    const [loaded, setLoaded] = useState(false);
    const [status, setStatus] = useState('');
    const [check, setCheck] = useState(null);
    const areaRef = useRef(null);

    useEffect(() => {
        let alive = true;
        setLoaded(false);
        setCheck(null);
        resourceText('resources/code/' + file).then(t => {
            if (!alive) return;
            setText(t);
            setLoaded(true);
            onDirty(false);
            setStatus('');
        }).catch(err => setStatus(String(err)));
        return () => { alive = false; };
    }, [file]);

    const edit = value => {
        setText(value);
        onDirty(true);
        setStatus('');
    };

    // Tab inserts spaces (the textarea is the code editor's typing surface).
    const onKeyDown = e => {
        if (e.key !== 'Tab') return;
        e.preventDefault();
        const el = areaRef.current;
        const start = el.selectionStart, end = el.selectionEnd;
        setText(t => {
            const next = t.slice(0, start) + '    ' + t.slice(end);
            requestAnimationFrame(() => { el.selectionStart = el.selectionEnd = start + 4; });
            return next;
        });
        onDirty(true);
    };

    const save = async () => {
        const result = await api('/api/save', { path: 'resources/code/' + file, content: text });
        if (result.error) return setStatus(result.error);
        onDirty(false);
        setStatus('Saved ' + file + ' — rebuild (Build tab) to bake it into the game.');
        onSaved();
    };

    const checkFile = async () => {
        setCheck(null);
        if (dirty) setStatus('Save first — the check compiles the file on disk.');
        else setCheck(await api('/api/check_code', { file }));
    };

    return (
        <div style={{ flex: 1, minWidth: 0 }}>
            <h1>{file}
                {dirty && <span className="fail"> (unsaved changes)</span>}
                <span className="dim" style={{ fontSize: 12 }}> — resources/code/{file}</span>
            </h1>
            <div className="toolbar" style={{ marginBottom: 8 }}>
                <button className="btn primary" onClick={save} disabled={!loaded}>Save</button>
                <button className="btn" onClick={checkFile} disabled={!loaded}>Compile check</button>
                <span className="dim">{text.split('\n').length} lines</span>
            </div>
            <textarea ref={areaRef} className="codeArea" spellCheck={false} value={text}
                onChange={e => edit(e.target.value)} onKeyDown={onKeyDown}
                placeholder={loaded ? '' : 'Loading…'} />
            {status && <p className="dim" style={{ marginTop: 6 }}>{status}</p>}
            {check && (
                <div style={{ marginTop: 8 }}>
                    {check.ok
                        ? <p className="dim" style={{ color: '#22c55e' }}>✓ Compiles clean.</p>
                        : <p className="fail">✗ Compile errors:</p>}
                    {check.output && check.output.trim() && check.output !== '(web build compiled clean)' && (
                        <pre className="build" style={{ maxHeight: 220 }}>{check.output.slice(-4000)}</pre>
                    )}
                </div>
            )}
        </div>
    );
}

function BuildTab({ scan, refresh }) {
    const [state, setState] = useState({ running: false, output: '', ok: null });
    const isMac = scan.platform === 'darwin';
    const script = isMac ? './build-mac.sh' : './build-web.sh';

    const run = async () => {
        setState({ running: true, output: 'Running ' + script + ' ...', ok: null });
        const result = await api('/api/build');
        setState({ running: false, output: (result.output || '') + (result.error ? '\n' + result.error : ''), ok: result.ok });
    };

    return (
        <div>
            <h1>Build</h1>
            <p className="dim">Runs the generator and compiles the game. Platform: {scan.platform}.</p>
            <button className="btn primary" onClick={run} disabled={state.running}>
                {state.running ? 'Building…' : 'Run ' + script}
            </button>
            {!isMac && <p className="dim">Non-Mac hosts build the web package (needs emcc on PATH; its errors show below).</p>}
            {state.ok === false && <p className="fail">Build failed.</p>}
            {state.output && <pre className="build">{state.output}</pre>}
            <p className="dim" style={{ marginTop: 20 }}>Game identity and save slots live on the File tab.</p>
        </div>
    );
}

createRoot(document.getElementById('root')).render(<App />);