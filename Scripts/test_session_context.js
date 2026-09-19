const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2] + '/Resources/FilesWeb/app.js', 'utf8');
const start = source.indexOf('function filesFetch(');
const end = source.indexOf('\nfunction parseFiles', start);
(async () => {
    for (const present of [false, true]) {
        let sent;
        const context = { URL, Headers, location: { href: 'http://files.test/', origin: 'http://files.test' },
            fetch: async (input, options) => { sent = { input, options }; } };
        context.window = context;
        if (present) context.outerLoop = { sessionContext: Object.freeze({ username: 'alice' }) };
        vm.runInNewContext(source.slice(start, end), context);
        const signal = new AbortController().signal;
        await context.filesFetch('/api/files?path=/', { signal, headers: { 'X-Test': 'preserved' } });
        assert.equal(sent.options.headers.get('X-Files-User'), present ? 'alice' : null);
        assert.equal(sent.options.headers.get('X-Test'), 'preserved');
        assert.equal(sent.options.signal, signal);
        await context.filesFetch('http://other.test/api/files');
        assert.equal(sent.options.headers.get('X-Files-User'), null);
    }
    console.log('PASS Files explicit identity, standalone fallback, origin boundary, and request options');
})().catch(error => { console.error(error); process.exitCode = 1; });
