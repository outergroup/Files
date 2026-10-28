const EnvironmentBinary = (() => {
    'use strict';
    const encoder = new TextEncoder(), decoder = new TextDecoder('utf-8', {fatal:true});
    const operations = {list:1, read:2, save:3, move:4, targets:7, preview:8, draft:9, places:10};
    function encode(operation, values = {}) {
        const op = operations[operation];
        if (!op) throw Error('Unsupported environment operation');
        const fields = [values.target ?? 'host', values.user ?? '', values.path ?? '~',
                        values.destination ?? '', values.revision ?? '', values.contents ?? ''];
        const strings = fields.map(value => {
            if (typeof value !== 'string' || value.includes('\0')) throw Error('Invalid environment field');
            return encoder.encode(value);
        });
        const size = 72 + strings.reduce((n, s) => n + s.length, 0);
        if (size > 16 * 1024 * 1024 || strings[5].length > 4 * 1024 * 1024) throw Error('Document exceeds size limit');
        const bytes = new Uint8Array(size), view = new DataView(bytes.buffer);
        [0x51434c46, 1, size, op].forEach((n, i) => view.setUint32(i * 4, n, true));
        let offset = 72;
        strings.forEach((s, i) => {
            view.setUint32(24 + i * 8, offset, true); view.setUint32(28 + i * 8, s.length, true);
            bytes.set(s, offset); offset += s.length;
        });
        return bytes;
    }
    function reader(buffer, minimum) {
        if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < minimum) throw Error('Truncated environment response');
        const view = new DataView(buffer);
        const u32 = offset => view.getUint32(offset, true);
        const u64 = offset => {
            const n = view.getBigUint64(offset, true);
            if (n > BigInt(Number.MAX_SAFE_INTEGER)) throw Error('Environment integer exceeds precision');
            return Number(n);
        };
        function bytes(field, variable = minimum) {
            const offset = u32(field), size = u32(field + 4);
            if (offset < variable || offset > buffer.byteLength || size > buffer.byteLength - offset)
                throw Error('Invalid environment offset');
            return buffer.slice(offset, offset + size);
        }
        function string(field, variable = minimum) {
            const s = decoder.decode(bytes(field, variable));
            if (s.includes('\0')) throw Error('Invalid environment string');
            return s;
        }
        return {u32, u64, bytes, string};
    }
    function targets(buffer) {
        const r = reader(buffer, 80), count = r.u32(16), errors = r.u32(40), variable = r.u32(52);
        if (r.u32(0) !== 0x5054454f || r.u32(4) !== 1 || r.u32(8) !== buffer.byteLength ||
            r.u32(12) > 1 || r.u32(20) !== 80 || r.u32(24) !== 56 || r.u32(28) !== 0 ||
            r.u32(32) !== 80 + count * 56 || r.u32(36) !== 112 ||
            r.u32(44) !== 80 + count * 56 || r.u32(48) !== 16 ||
            variable !== 80 + count * 56 + errors * 16 || variable > buffer.byteLength || r.u32(68))
            throw Error('Invalid environment list');
        if (r.u32(12)) throw Error(r.string(72, variable));
        const fields = ['id', 'runtime', 'container', 'name', 'state', 'image', 'user'];
        return {
            targets:Array.from({length:count}, (_, i) => Object.fromEntries(fields.map((name, j) =>
                [name, r.string(80 + i * 56 + j * 8, variable)]))),
            errors:Array.from({length:errors}, (_, i) => ({runtime:r.string(r.u32(44) + i * 16, variable),
                error:r.string(r.u32(44) + i * 16 + 8, variable)}))
        };
    }
    function decode(buffer, operation) {
        const r = reader(buffer, 48);
        if (r.u32(0) !== 0x52434c46 || r.u32(4) !== 1 || r.u32(8) !== buffer.byteLength ||
            r.u32(12) > 1 || r.u32(40) > 3 || r.u32(44)) throw Error('Invalid environment response');
        if (r.u32(12)) throw Error(r.string(32));
        const metadata = r.bytes(16), body = r.bytes(24);
        if (operation === 'targets') return targets(metadata);
        const m = reader(metadata, 64), count = m.u32(20), stride = m.u32(24), variable = m.u32(32);
        const expected = operation === 'preview' ? 2 : operation === 'draft' ? 3 : operations[operation];
        if (!expected || m.u32(0) !== 0x53484c46 || m.u32(4) !== 2 || m.u32(8) !== expected ||
            m.u32(12) !== 0 || m.u32(16) !== metadata.byteLength || m.u32(28) !== 64 || m.u32(36) ||
            m.u64(40) !== body.byteLength || stride !== ((operation === 'list' || operation === 'places') ? 48 : 0) ||
            (!stride && count) || variable !== 64 + count * stride || variable > metadata.byteLength ||
            (expected !== 2 && body.byteLength)) throw Error('Invalid environment metadata');
        const result = {ok:true, path:m.string(48, variable)};
        if ((operation === 'list' || operation === 'places')) {
            result.parent = m.string(56, variable);
            result.entries = Array.from({length:count}, (_, i) => {
                const o = 64 + i * stride;
                if (m.u32(o + 24) > 1) throw Error('Invalid directory flag');
                return {name:m.string(o, variable), path:m.string(o + 8, variable), mode:m.string(o + 16, variable),
                    directory:!!m.u32(o + 24), access:m.u32(o + 28), size:m.u64(o + 32), modified:m.u64(o + 40)};
            });
        } else if (operation === 'preview') {
            result.data = body; result.type = r.string(32);
        } else {
            m.string(56, variable);
            if (operation === 'read') {
                result.contents = decoder.decode(body);
                if (result.contents.includes('\0')) throw Error('Invalid document text');
                result.dirty = !!(r.u32(40) & 1); result.draftConflict = !!(r.u32(40) & 2);
            }
        }
        return result;
    }
    return {encode, decode};
})();
