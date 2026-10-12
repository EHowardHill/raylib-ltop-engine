import { useState, useEffect, useRef, useCallback, useContext, createContext } from 'react';
import { createRoot } from 'react-dom/client';
import { open as openDialog } from '@tauri-apps/plugin-dialog';
import { api, resourceUrl } from './api.js';
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
        ['rooms', 'Rooms'], ['dialogue', 'Dialogue'], ['characters', 'Characters'],
        ['files', 'Music & Files'], ['build', 'Build']
    ];

    return (
        <ConfirmProvider>
            <div id="wrap" style={{ display: 'flex', width: '100%' }}>
                <nav>
                    <div style={{ color: '#fff', fontWeight: 'bold', padding: '6px 10px 14px' }}>LttP Editor</div>
                    {tabs.map(([key, label]) =>
                        <button key={key} className={tab === key ? 'active' : ''} onClick={() => setTab(key)}>{label}</button>)}
                    <div style={{ marginTop: 16, padding: '0 10px' }}>
                        <div className="dim" style={{ wordBreak: 'break-all' }} title={scan.project}>{scan.project}</div>
                        <ChangeProject onPicked={refresh} />
                    </div>
                </nav>
                <main>
                    {tab === 'rooms' && <RoomsTab scan={scan} refresh={refresh} />}
                    {tab === 'dialogue' && <DialogueTab scan={scan} refresh={refresh} />}
                    {tab === 'characters' && <CharactersTab scan={scan} refresh={refresh} />}
                    {tab === 'files' && <FilesTab scan={scan} refresh={refresh} />}
                    {tab === 'build' && <BuildTab scan={scan} />}
                </main>
            </div>
        </ConfirmProvider>
    );
}

function ChangeProject({ onPicked }) {
    const { pick, error } = useProject();
    return (
        <div>
            <button className="btn" style={{ marginTop: 6 }} onClick={() => pick(onPicked)}>Change…</button>
            {error && <p className="fail">{error}</p>}
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
    ['interactive', 'Place Interactive', 'M']
];
const TOOL_HINTS = {
    paint: 'Left-drag paints the active layer. Right-drag erases.',
    erase: 'Left-drag erases tiles from the active layer.',
    fill: 'Click to flood-fill a region of the active layer. Right-click fills with empty.',
    rect: 'Drag a rectangle to fill it on the active layer. Right-drag fills with empty.',
    pick: 'Click a tile to grab it into the palette.',
    select: 'Click or drag the spawn / encounter markers to move them. Del removes the selected encounter.',
    spawn: 'Click a tile to place the hero spawn.',
    instant: 'Click a tile to add an instant encounter.',
    interactive: 'Click a tile to add an interactive (action-button) encounter.'
};
const ENCOUNTER_COLORS = { instant: '#f97316', interactive: '#3b82f6' };

function RoomEditor({ room, scan, onSaved }) {
    const tileset = scan.tileset || { image: '', columns: 1, tileCount: 0, tileWidth: 32, tileHeight: 32 };
    const cols = tileset.columns || 1;
    const ts = tileset.tileWidth || 32;

    const [size, setSize] = useState({ w: room.mapWidth, h: room.mapHeight });
    const [def, setDef] = useState(room.default || {});
    const [enc, setEnc] = useState({ instant: [], interactive: [], ...(room.encounters || {}) });
    const [layers, setLayers] = useState({ decor: room.decor.slice(), colliders: room.colliders.slice() });
    const [layer, setLayer] = useState('decor');
    const [vis, setVis] = useState({ decor: true, colliders: true });
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

    // Keyboard shortcuts (ignored while typing in a field).
    useEffect(() => {
        const onKey = e => {
            const tag = (e.target && e.target.tagName) || '';
            if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
            const key = e.key.toLowerCase();
            const keys = { b: 'paint', e: 'erase', g: 'fill', r: 'rect', i: 'pick', v: 'select', s: 'spawn', n: 'instant', m: 'interactive' };
            if (keys[key]) { setTool(keys[key]); return; }
            if (key === 'escape') { setSelected(null); setRectStart(null); return; }
            if ((key === 'delete' || key === 'backspace') && selected && selected !== 'spawn') {
                setEnc(prev => ({ ...prev, [selected.kind]: prev[selected.kind].filter((_, j) => j !== selected.index) }));
                setSelected(null);
                setDirty(true);
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
    }, [img, layers, vis, def, enc, hover, tool, rectStart, selected, showGrid, zoom, W, H]);

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

    const markerAt = tile => {
        if (!tile) return null;
        for (const kind of ['instant', 'interactive']) {
            const list = enc[kind] || [];
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
        } else {
            setEnc(prev => {
                const list = prev[marker.kind] || [];
                const entry = list[marker.index];
                if (!entry || (entry.location && entry.location[0] === tile[0] && entry.location[1] === tile[1])) return prev;
                const copy = list.slice();
                copy[marker.index] = { ...entry, location: tile.slice() };
                return { ...prev, [marker.kind]: copy };
            });
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
        setLayers(prev => ({ decor: remap(prev.decor), colliders: remap(prev.colliders) }));
        const clampLoc = loc => [Math.min(loc[0] || 0, w - 1), Math.min(loc[1] || 0, h - 1)];
        if (def.spawn) patchDef('spawn', clampLoc(def.spawn));
        setEnc(prev => ({
            instant: (prev.instant || []).map(e => ({ ...e, location: clampLoc(e.location || [0, 0]) })),
            interactive: (prev.interactive || []).map(e => ({ ...e, location: clampLoc(e.location || [0, 0]) }))
        }));
        setSize({ w, h });
        setDirty(true);
    };

    const save = async () => {
        await api('/api/save', {
            path: 'resources/rooms/' + room.file,
            content: JSON.stringify({
                width: W, height: H,
                default: def,
                encounters: enc,
                decor: layers.decor,
                colliders: layers.colliders
            }, null, 4)
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
                    <div className="toolbar">
                        {['decor', 'colliders'].map(l =>
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
                                    <div className={'row encRow' + (isSelected ? ' selected' : '')} key={i}
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
                                );
                            })}
                        </div>
                    ))}

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
                ? <RoomEditor key={room.file + ':' + scan.rooms.length} room={room} scan={scan} onSaved={refresh} />
                : <p className="dim">No rooms. Create one on the left.</p>}
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
                ? <DialogueEditor key={dialogue.file} dialogue={dialogue} onSaved={refresh} />
                : <p className="dim">No dialogues.</p>}
        </div>
    );
}

function DialogueEditor({ dialogue, onSaved }) {
    const [lines, setLines] = useState((dialogue.data.lines || []).map(l => l.text != null ? l.text : ''));
    const [status, setStatus] = useState('');

    const save = async () => {
        await api('/api/save', {
            path: 'resources/dialogue/' + dialogue.file,
            content: JSON.stringify({ lines: lines.map(text => ({ text })) }, null, 4)
        });
        setStatus('Saved');
        onSaved();
    };

    return (
        <div style={{ flex: 1 }}>
            <h1>{dialogue.file}</h1>
            <h2>Lines</h2>
            {lines.map((text, i) => (
                <div className="row" key={i}>
                    <span className="dim">{i + 1}.</span>
                    <input style={{ flex: 1, minWidth: 300 }} value={text}
                        onChange={e => setLines(prev => prev.map((t, j) => j === i ? e.target.value : t))} />
                    <button className="delete" onClick={() => setLines(prev => prev.filter((_, j) => j !== i))}>×</button>
                </div>
            ))}
            <div className="row" style={{ marginTop: 8 }}>
                <button className="btn" onClick={() => setLines(prev => prev.concat(''))}>+ Line</button>
                <button className="btn primary" onClick={save}>Save</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

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
// plays at the same speed the engine will.
const ANIM_FPS = { stand: 2, move: 12 };
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
    }, [pngPath]);

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
                            {['stand', 'move'].map(kind => (
                                <div className="field" key={kind}>
                                    <label>{kind}</label>
                                    {DIRECTIONS.map(dir => (
                                        <button key={dir}
                                            className={'tool' + (anim.kind === kind && anim.dir === dir ? ' active' : '')}
                                            onClick={() => { setAnim({ kind, dir }); setSlot(null); }}>{dir}</button>
                                    ))}
                                </div>
                            ))}
                            <p className="dim">stand plays at {ANIM_FPS.stand} fps, move at {ANIM_FPS.move} fps — the engine's speeds.</p>
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
                <p className="dim">Upload {stem(character.file)}.png in Music & Files to use the graphical frame picker. Frames can still be typed below.</p>
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
                </select>
                <input type="file" onChange={upload} />
                <span className="dim">{status}</span>
            </div>
            <FileList title="Music (resources/music)" dirName="music" files={scan.music} />
            <FileList title="Audio (resources/audio)" dirName="audio" files={scan.audio} />
            <FileList title="UI (resources/ui)" dirName="ui" files={scan.ui} />
        </div>
    );
}

function BuildTab({ scan }) {
    const [state, setState] = useState({ running: false, output: '', ok: null });
    const isMac = scan.platform === 'darwin';

    const run = async () => {
        setState({ running: true, output: 'Running ./build-mac.sh ...', ok: null });
        const result = await api('/api/build');
        setState({ running: false, output: (result.output || '') + (result.error ? '\n' + result.error : ''), ok: result.ok });
    };

    return (
        <div>
            <h1>Build</h1>
            <p className="dim">Runs the generator and compiles the game. Platform: {scan.platform}.</p>
            <button className="btn primary" onClick={run} disabled={state.running || !isMac}>
                {state.running ? 'Building…' : 'Run ./build-mac.sh'}
            </button>
            {!isMac && <p className="fail">build-mac.sh only runs on Mac.</p>}
            {state.ok === false && <p className="fail">Build failed.</p>}
            {state.output && <pre className="build">{state.output}</pre>}
        </div>
    );
}

createRoot(document.getElementById('root')).render(<App />);