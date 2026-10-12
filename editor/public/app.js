const { useState, useEffect, useRef, useCallback } = React;

const api = (url, body) =>
    fetch(url, body
        ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
        : undefined)
        .then(r => r.json());

const stem = file => file.replace(/\.json$/, '');
const DIRECTIONS = ['down', 'right', 'left', 'up'];

function csvText(values, width, height) {
    const rows = [];
    for (let y = 0; y < height; y++) {
        const row = [];
        for (let x = 0; x < width; x++) row.push(values[y * width + x] || 0);
        rows.push(row.join(','));
    }
    return rows.join(',\n');
}

function tmxText(layers, width, height) {
    const layerXml = [['decor', layers.decor], ['colliders', layers.colliders]].map(([name, values], i) =>
        ` <layer id="${i + 1}" name="${name}" width="${width}" height="${height}">\n  <data encoding="csv">\n${csvText(values, width, height)}\n</data>\n </layer>`)
        .join('\n');
    return `<?xml version="1.0" encoding="UTF-8"?>\n<map version="1.10" tiledversion="1.12.2" orientation="orthogonal" renderorder="right-down" width="${width}" height="${height}" tilewidth="32" tileheight="32" infinite="0" nextlayerid="3" nextobjectid="1">\n <tileset firstgid="1" source="../tiles/texture.tsx"/>\n${layerXml}\n</map>\n`;
}

function App() {
    const [scan, setScan] = useState(null);
    const [tab, setTab] = useState('rooms');
    const refresh = useCallback(() => api('/api/scan').then(setScan), []);
    useEffect(() => { refresh(); }, []);

    if (!scan) return <p className="dim">Loading...</p>;

    const tabs = [
        ['rooms', 'Rooms'], ['dialogue', 'Dialogue'], ['characters', 'Characters'],
        ['files', 'Music & Files'], ['build', 'Build']
    ];

    return (
        <div id="wrap" style={{ display: 'flex', width: '100%' }}>
            <nav>
                <div style={{ color: '#fff', fontWeight: 'bold', padding: '6px 10px 14px' }}>LttP Editor</div>
                {tabs.map(([key, label]) =>
                    <button key={key} className={tab === key ? 'active' : ''} onClick={() => setTab(key)}>{label}</button>)}
            </nav>
            <main>
                {tab === 'rooms' && <RoomsTab scan={scan} refresh={refresh} />}
                {tab === 'dialogue' && <DialogueTab scan={scan} refresh={refresh} />}
                {tab === 'characters' && <CharactersTab scan={scan} refresh={refresh} />}
                {tab === 'files' && <FilesTab scan={scan} refresh={refresh} />}
                {tab === 'build' && <BuildTab scan={scan} />}
            </main>
        </div>
    );
}

function RoomEditor({ room, scan, onSaved }) {
    const tileset = scan.tileset || { image: '', columns: 1, tileCount: 0, tileWidth: 32, tileHeight: 32 };
    const firstgid = room.firstgid || 1;
    const W = room.mapWidth, H = room.mapHeight, ts = tileset.tileWidth || 32;

    const [def, setDef] = useState(room.default || {});
    const [enc, setEnc] = useState({ instant: [], interactive: [], ...(room.encounters || {}) });
    const [layers, setLayers] = useState({ decor: room.decor.slice(), colliders: room.colliders.slice() });
    const [layer, setLayer] = useState('decor');
    const [gid, setGid] = useState(firstgid);
    const [erase, setErase] = useState(false);
    const [status, setStatus] = useState('');
    const [img, setImg] = useState(null);
    const canvasRef = useRef(null);
    const painting = useRef(false);

    useEffect(() => {
        const image = new Image();
        image.onload = () => setImg(image);
        image.src = '/' + tileset.image;
    }, [tileset.image]);

    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#555';
        ctx.fillRect(0, 0, canvas.width, canvas.height);

        const cols = tileset.columns || 1;
        for (let y = 0; y < H; y++) {
            for (let x = 0; x < W; x++) {
                const id = layers[layer][y * W + x];
                if (id) {
                    const frame = id - firstgid;
                    ctx.drawImage(img, (frame % cols) * ts, Math.floor(frame / cols) * ts, ts, ts, x * ts, y * ts, ts, ts);
                }
            }
        }

        ctx.strokeStyle = 'rgba(255,255,255,0.12)';
        ctx.lineWidth = 1;
        for (let x = 0; x <= W; x++) { ctx.beginPath(); ctx.moveTo(x * ts + 0.5, 0); ctx.lineTo(x * ts + 0.5, H * ts); ctx.stroke(); }
        for (let y = 0; y <= H; y++) { ctx.beginPath(); ctx.moveTo(0, y * ts + 0.5); ctx.lineTo(W * ts, y * ts + 0.5); ctx.stroke(); }

        if (def.spawn) {
            ctx.strokeStyle = '#22c55e';
            ctx.lineWidth = 2;
            ctx.strokeRect(def.spawn[0] * ts + 1, def.spawn[1] * ts + 1, ts - 2, ts - 2);
        }
        const dot = (location, color) => {
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.arc(location[0] * ts + ts / 2, location[1] * ts + ts / 2, 6, 0, Math.PI * 2);
            ctx.fill();
        };
        (enc.instant || []).forEach(e => e.location && dot(e.location, '#f97316'));
        (enc.interactive || []).forEach(e => e.location && dot(e.location, '#3b82f6'));
    }, [img, layers, layer, def, enc]);

    const tileFromEvent = e => {
        const canvas = canvasRef.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / W));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / H));
        return (x >= 0 && y >= 0 && x < W && y < H) ? [x, y] : null;
    };

    const paint = e => {
        const tile = tileFromEvent(e);
        if (!tile) return;
        const index = tile[1] * W + tile[0];
        const value = erase ? 0 : gid;
        if (layers[layer][index] === value) return;
        const copy = layers[layer].slice();
        copy[index] = value;
        setLayers(prev => ({ ...prev, [layer]: copy }));
    };

    const patchDef = (key, value) => setDef(prev => ({ ...prev, [key]: value }));

    const save = async () => {
        await api('/api/save', {
            path: 'resources/rooms/' + room.file,
            content: JSON.stringify({ default: def, encounters: enc }, null, 4)
        });
        if (room.tmx) {
            await api('/api/save', { path: room.tmx, content: tmxText(layers, W, H) });
        }
        setStatus('Saved');
        onSaved();
    };

    const encounterKinds = ['instant', 'interactive'];
    const kindLabels = { instant: 'Instant (on entry)', interactive: 'Interactive (action button)' };

    return (
        <div style={{ flex: 1, minWidth: 0 }}>
            <h1>{room.file}{!room.tmx && <span className="fail"> (missing .tmx)</span>}</h1>

            <h2>Default</h2>
            <div className="field"><label>Hero</label>
                <select value={def.hero || ''} onChange={e => patchDef('hero', e.target.value)}>
                    <option value="">(none)</option>
                    {scan.characters.map(c => <option key={c.file} value={stem(c.file)}>{stem(c.file)}</option>)}
                </select>
            </div>
            <div className="field"><label>Spawn</label>
                <input type="checkbox" checked={!!def.spawn}
                    onChange={e => patchDef('spawn', e.target.checked ? [0, 0] : undefined)} />
                {def.spawn && <span>
                    <input type="number" value={def.spawn[0]} onChange={e => patchDef('spawn', [+e.target.value, def.spawn[1]])} />
                    <input type="number" value={def.spawn[1]} onChange={e => patchDef('spawn', [def.spawn[0], +e.target.value])} />
                </span>}
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

            <h2>Encounters</h2>
            {encounterKinds.map(kind => (
                <div key={kind}>
                    <h2>{kindLabels[kind]}</h2>
                    {(enc[kind] || []).map((entry, i) => (
                        <div className="row" key={i}>
                            <span className="dim">X,Y</span>
                            <input type="number" value={entry.location ? entry.location[0] : 0}
                                onChange={e => setEnc(prev => ({
                                    ...prev, [kind]: prev[kind].map((x, j) => j === i
                                        ? { ...x, location: [+e.target.value, (x.location || [0, 0])[1]] } : x)
                                }))} />
                            <input type="number" value={entry.location ? entry.location[1] : 0}
                                onChange={e => setEnc(prev => ({
                                    ...prev, [kind]: prev[kind].map((x, j) => j === i
                                        ? { ...x, location: [(x.location || [0, 0])[0], +e.target.value] } : x)
                                }))} />
                            <select value={entry.dialogue || ''}
                                onChange={e => setEnc(prev => ({
                                    ...prev, [kind]: prev[kind].map((x, j) => j === i ? { ...x, dialogue: e.target.value } : x)
                                }))}>
                                <option value="">(no dialogue)</option>
                                {scan.dialogues.map(d => <option key={d.file} value={d.file}>{d.file}</option>)}
                            </select>
                            <label><input type="checkbox" checked={!!entry.repeat}
                                onChange={e => setEnc(prev => ({
                                    ...prev, [kind]: prev[kind].map((x, j) => j === i ? { ...x, repeat: e.target.checked } : x)
                                }))} /> repeat</label>
                            <button className="delete"
                                onClick={() => setEnc(prev => ({ ...prev, [kind]: prev[kind].filter((_, j) => j !== i) }))}>×</button>
                        </div>
                    ))}
                    <div className="row">
                        <button className="btn" onClick={() => setEnc(prev => ({
                            ...prev,
                            [kind]: (prev[kind] || []).concat({
                                location: [0, 0],
                                dialogue: scan.dialogues[0] ? scan.dialogues[0].file : '',
                                repeat: false
                            })
                        }))}>+ Add</button>
                    </div>
                </div>
            ))}

            <h2>Tile Map ({W} × {H})</h2>
            <div className="row">
                {['decor', 'colliders'].map(l =>
                    <button key={l} className={'btn' + (layer === l ? ' primary' : '')} onClick={() => setLayer(l)}>{l}</button>)}
                <button className={'btn' + (erase ? '' : ' primary')} onClick={() => setErase(false)}>Paint</button>
                <button className={'btn' + (erase ? ' primary' : '')} onClick={() => setErase(true)}>Erase</button>
            </div>
            <div className="row" style={{ marginTop: 8 }}>
                <Palette tileset={tileset} img={img} firstgid={firstgid} selected={gid}
                    onSelect={g => { setGid(g); setErase(false); }} />
                <canvas ref={canvasRef} width={W * ts} height={H * ts}
                    style={{ maxWidth: '100%', cursor: 'crosshair' }}
                    onMouseDown={e => { painting.current = true; paint(e); }}
                    onMouseMove={e => { if (painting.current) paint(e); }}
                    onMouseUp={() => { painting.current = false; }}
                    onMouseLeave={() => { painting.current = false; }} />
            </div>
            <p className="dim">Markers: green = spawn, orange = instant, blue = interactive. Only the selected layer is painted.</p>

            <div className="row" style={{ marginTop: 12 }}>
                <button className="btn primary" onClick={save}>Save Room</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

function Palette({ tileset, img, firstgid, selected, onSelect }) {
    const ref = useRef(null);
    const cols = tileset.columns || 1;
    const rows = Math.max(1, Math.ceil((tileset.tileCount || 1) / cols));
    const tw = tileset.tileWidth || 32;

    useEffect(() => {
        const canvas = ref.current;
        if (!canvas || !img) return;
        const ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        ctx.drawImage(img, 0, 0);
        const frame = selected - firstgid;
        if (frame >= 0) {
            ctx.strokeStyle = '#f00';
            ctx.lineWidth = 2;
            ctx.strokeRect((frame % cols) * tw + 1, Math.floor(frame / cols) * tw + 1, tw - 2, tw - 2);
        }
    }, [img, selected]);

    const click = e => {
        const canvas = ref.current, rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) / (rect.width / cols));
        const y = Math.floor((e.clientY - rect.top) / (rect.height / rows));
        if (x >= 0 && y >= 0 && x < cols && y < rows) onSelect(firstgid + y * cols + x);
    };

    return (
        <canvas ref={ref} width={cols * tw} height={rows * tw} onClick={click}
            style={{ width: 160, background: '#444' }} />
    );
}

function RoomsTab({ scan, refresh }) {
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
        if (!confirm('Delete ' + r.file + (r.tmx ? ' and its .tmx' : '') + '?')) return;
        await api('/api/delete', { path: 'resources/rooms/' + r.file });
        if (r.tmx) await api('/api/delete', { path: r.tmx });
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
        if (!confirm('Delete ' + d.file + '?')) return;
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
        if (!confirm('Delete ' + c.file + '? (the .png is kept)')) return;
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

function CharacterEditor({ character, onSaved }) {
    const [data, setData] = useState(character.data);
    const [status, setStatus] = useState('');
    const pngUrl = character.png ? '/resources/characters/' + stem(character.file) + '.png' : null;

    const setFrames = (kind, dir, text) => {
        const values = text.split(',').map(s => parseInt(s.trim(), 10)).filter(n => !isNaN(n));
        setData(prev => ({ ...prev, [kind]: { ...(prev[kind] || {}), [dir]: values } }));
    };

    const save = async () => {
        await api('/api/save', {
            path: 'resources/characters/' + character.file,
            content: JSON.stringify(data, null, 4)
        });
        setStatus('Saved');
        onSaved();
    };

    return (
        <div style={{ flex: 1 }}>
            <h1>{character.file}{!character.png && <span className="fail"> (missing {stem(character.file)}.png)</span>}</h1>
            <div className="field"><label>Width (tiles)</label>
                <input type="number" value={data.width != null ? data.width : 4}
                    onChange={e => setData(prev => ({ ...prev, width: +e.target.value }))} />
            </div>
            {['stand', 'move'].map(kind => (
                <div key={kind}>
                    <h2>{kind} (frame indices, comma separated)</h2>
                    {DIRECTIONS.map(dir => (
                        <div className="field" key={dir}>
                            <label>{dir}</label>
                            <input style={{ width: 240 }}
                                value={((data[kind] || {})[dir] || []).join(', ')}
                                onChange={e => setFrames(kind, dir, e.target.value)} />
                        </div>
                    ))}
                </div>
            ))}
            {pngUrl && (
                <div>
                    <h2>Sprite Sheet</h2>
                    <img className="sheet" src={pngUrl} alt="sprite sheet" />
                    <p className="dim">Frame index = row × width + column, top-left is frame 0.</p>
                </div>
            )}
            <div className="row" style={{ marginTop: 12 }}>
                <button className="btn primary" onClick={save}>Save Character</button>
                <span className="dim">{status}</span>
            </div>
        </div>
    );
}

function FilesTab({ scan, refresh }) {
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
                        if (confirm('Delete ' + dirName + '/' + f + '?')) {
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

ReactDOM.createRoot(document.getElementById('root')).render(<App />);
