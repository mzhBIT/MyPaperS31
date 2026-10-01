// Run with: node tools/check_ui_localization.cjs
// Check enum/table alignment, printf arguments, and transfer-page translations.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const header = read('main/ui/UiLocale.h');
const source = read('main/ui/UiLocale.cpp');
const ids = header.match(/enum class TextId[^]*?\{([^]*?)\n\};/)[1]
    .split(',').map(id => id.trim()).filter(id => id && id !== 'Count');
const dictionary = source.match(/constexpr Translation kText\[\] = \{([^]*?)\n\};/)[1];
const rows = [...dictionary.matchAll(/\{("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")\s*\}/g)]
    .map(match => match.slice(1).map(value => JSON.parse(value)));
assert.equal(rows.length, ids.length, 'TextId and translation table must align');
assert.equal(new Set(ids).size, ids.length, 'TextIds must be unique');
const formats = text => (text.match(/%(?:\d+\$)?[-+#0]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcsp%]/g) || [])
    .filter(value => value !== '%%');
rows.forEach((row, i) => {
    row.forEach(value => assert.ok(value.length > 0, `Empty translation: ${ids[i]}`));
    assert.deepEqual(formats(row[1]), formats(row[0]), `Simplified format: ${ids[i]}`);
    assert.deepEqual(formats(row[2]), formats(row[0]), `Traditional format: ${ids[i]}`);
});

const transfer = read('main/activities/WifiTransferActivity.cpp');
const html = transfer.match(/FILE_MANAGER_HTML\[\] = R"HTML\(([^]*?)\)HTML";/)[1];
const labels = [...transfer.matchAll(/\{"(\w+)", TextId::(\w+)\}/g)];
const htmlKeys = [...html.matchAll(/data-i18n="(\w+)"/g)].map(match => match[1]);
const scriptKeys = [...html.matchAll(/\bt\('(\w+)'\)/g)].map(match => match[1]);
for (let language = 0; language < 3; language++) {
    const translations = { lang: ['en', 'zh-Hans', 'zh-Hant'][language] };
    for (const [, key, id] of labels) {
        assert.ok(ids.includes(id), `Unknown web TextId: ${id}`);
        translations[key] = rows[ids.indexOf(id)][language];
    }
    for (const key of [...htmlKeys, ...scriptKeys]) {
        assert.ok(translations[key], `Missing web translation: ${key}`);
    }
    const rendered = html.replace('__UI_LOCALE__', JSON.stringify(translations));
    const elements = new Map();
    const makeElement = () => ({ textContent: '', value: '', files: [], children: [],
        classList: { toggle() {} }, append(...items) { this.children.push(...items); },
        appendChild(item) { this.children.push(item); } });
    const staticElements = htmlKeys.map(key => ({ ...makeElement(), dataset: { i18n: key } }));
    const document = { documentElement: {}, title: '',
        querySelectorAll: () => staticElements,
        getElementById: id => {
            if (!elements.has(id)) elements.set(id, makeElement());
            return elements.get(id);
        }, createElement: makeElement };
    let confirmation = '';
    const context = { document, location: {},
        confirm: message => { confirmation = message; return false; }, prompt: () => null,
        fetch: async () => ({ ok: true, json: async () => ({ path: '/', entries: [] }) }) };
    vm.createContext(context);
    vm.runInContext(rendered.match(/<script>([^]*?)<\/script>/)[1], context);
    assert.equal(document.documentElement.lang, translations.lang);
    staticElements.forEach(el => assert.equal(el.textContent, translations[el.dataset.i18n]));
    vm.runInContext("removeEntry('sample.txt')", context);
    assert.equal(confirmation, translations.WebDeletePrefix + 'sample.txt?');
    context.expectedEmpty = translations.WebEmptyDirectory;
    context.assertEmpty = actual => assert.equal(actual, translations.WebEmptyDirectory);
    vm.runInContext("reload().then(()=>assertEmpty(document.getElementById('entries').textContent))", context);
}
console.log(`Checked ${ids.length} translations and transfer-page behavior in all 3 languages.`);
