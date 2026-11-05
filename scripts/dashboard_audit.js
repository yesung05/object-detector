// 대시보드 감사 (읽기 전용): 재디자인 중 JS 가 참조하는 DOM id 가 사라지는 회귀를 막고, 설명 문구 수를 센다.
//
// 왜 필요한가: index.html 은 3천 줄이고 인라인 JS 가 $('id').textContent=… 로 요소를 직접 건드린다. 설명 문구를
// 지우다가 JS 가 쓰는 요소까지 지우면 null 참조 오류로 화면이 깨지는데, 브라우저를 열기 전에는 알 수 없다.
//
// 사용: node scripts/dashboard_audit.js [dashboard 폴더] [--json 출력.json]
// 종료 코드: JS 가 참조하지만 정의되지 않은 id 가 있으면 1.
const fs = require('node:fs');
const path = require('node:path');

const dir = process.argv[2] && !process.argv[2].startsWith('--') ? process.argv[2] : path.join(__dirname, '..', 'dashboard');
const jsonOut = process.argv.includes('--json') ? process.argv[process.argv.indexOf('--json') + 1] : null;

// 설명성 문장: 종결 어미로 끝나는 한국어 문장, 또는 진행 문구(…중, 확인 중, 로딩 중).
const SENTENCE = /(합니다|입니다|하세요|됩니다|있습니다|않습니다|습니다|해요|세요)[.!]?\s*$|(?:중|중입니다)[.…]*\s*$|\.{3}\s*$|…\s*$/;
const DESC_CLASS = /class="[^"]*\b(setting-desc|preset-desc|preset-subtitle|hint|desc|sub-text)\b[^"]*"/g;

function stripTags(html) {
  return html.replace(/<script[\s\S]*?<\/script>/g, ' ').replace(/<style[\s\S]*?<\/style>/g, ' ')
             .replace(/<!--[\s\S]*?-->/g, ' ');
}

function audit(file) {
  const html = fs.readFileSync(path.join(dir, file), 'utf8');
  const scripts = [...html.matchAll(/<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).join('\n');
  // 같은 폴더의 외부 스크립트(src="x.js")도 JS 로 본다
  const ext = [...html.matchAll(/<script[^>]*\bsrc="([^"]+\.js)"/g)].map(m => m[1])
    .filter(s => !s.startsWith('http')).map(s => path.join(dir, s)).filter(fs.existsSync)
    .map(f => fs.readFileSync(f, 'utf8')).join('\n');
  const js = scripts + '\n' + ext;

  const referenced = new Set();
  for (const re of [/getElementById\(\s*['"]([^'"]+)['"]\s*\)/g, /\$\(\s*['"]([^'"]+)['"]\s*\)/g,
                    /querySelector\(\s*['"]#([A-Za-z0-9_-]+)['"]\s*\)/g]) {
    for (const m of js.matchAll(re)) referenced.add(m[1]);
  }
  const defined = new Set();
  for (const m of html.matchAll(/\bid\s*=\s*["']([^"'$]+)["']/g)) defined.add(m[1]);      // 정적·JS 템플릿 안의 id="…"
  for (const m of js.matchAll(/\.id\s*=\s*['"]([^'"]+)['"]/g)) defined.add(m[1]);         // el.id = '…'
  for (const m of js.matchAll(/\bid\s*:\s*['"]([^'"]+)['"]/g)) defined.add(m[1]);         // {id:'…'}
  const missing = [...referenced].filter(id => !defined.has(id)).sort();

  const text = stripTags(html);
  const lines = text.split(/\n/).map(l => l.replace(/<[^>]+>/g, ' ').replace(/&[a-z]+;/g, ' ').trim()).filter(Boolean);
  const sentences = lines.filter(l => /[가-힣]/.test(l) && SENTENCE.test(l));
  const jsStrings = [...js.matchAll(/(['"`])((?:\\.|(?!\1)[^\\\n]){6,200})\1/g)].map(m => m[2])
    .filter(s => /[가-힣]/.test(s) && SENTENCE.test(s));
  const descNodes = (html.match(DESC_CLASS) || []).length;
  const emoji = (text.match(/[\u{1F300}-\u{1FAFF}\u{2600}-\u{27BF}]/gu) || []).length;
  return { file, ids_referenced: referenced.size, ids_missing: missing, static_sentence_lines: sentences.length,
           js_sentence_strings: jsStrings.length, desc_class_nodes: descNodes, emoji_in_markup: emoji,
           google_fonts: /fonts\.googleapis\.com/.test(html), pretendard: /pretendard/i.test(html) || /pretendard/i.test(js),
           sample_sentences: sentences.slice(0, 3).concat(jsStrings.slice(0, 3)) };
}

const files = fs.readdirSync(dir).filter(f => f.endsWith('.html')).sort();
const results = files.map(audit);
let bad = 0;
for (const r of results) {
  bad += r.ids_missing.length;
  console.log(`${r.file.padEnd(24)} 참조 id ${String(r.ids_referenced).padStart(3)} | 누락 ${r.ids_missing.length} | ` +
    `정적 문장 ${String(r.static_sentence_lines).padStart(3)} | JS 문장 ${String(r.js_sentence_strings).padStart(3)} | ` +
    `desc 클래스 ${String(r.desc_class_nodes).padStart(3)} | 이모지 ${String(r.emoji_in_markup).padStart(3)} | ` +
    `구글폰트 ${r.google_fonts ? 'O' : '-'} Pretendard ${r.pretendard ? 'O' : '-'}`);
  if (r.ids_missing.length) console.log('   누락 id:', r.ids_missing.join(', '));
}
if (jsonOut) fs.writeFileSync(jsonOut, JSON.stringify(results, null, 2));
process.exit(bad ? 1 : 0);
