'use strict';
// Panel Manager words, loaded before the other scripts so their tables can mark words
// with tx() as they are built.

// Words (docs/features/37): English is the source; tools/i18n.py extracts t(), tc(), tn(), ht()
// and tx() here as it does TR() in the panel's C. {0}, {1} are values, never glued pieces. tx()
// only marks a word for a table built before a language is loaded; t(entry.title) translates it
// where it is shown. ht() is t() escaped for HTML: a translation is text, never markup (I18N-008).
// The panel sends a language's words from its own tables (/i18n/<locale>.json, i18n_web_json),
// keyed by the same hash as the C lookup, so they take no second copy in its image (I18N-031).
const words = {locale: 'en-US', plural: ['one', 'other'], entries: null};
const utf8 = new TextEncoder();
// 32-bit FNV-1a over the context, one 0x04 byte, then the English (i18n_hash in C), in hex.
function wordKey(ctx, text) {
  let h = 0x811c9dc5;
  const add = bytes => { for (const b of bytes) h = Math.imul(h ^ b, 0x01000193) >>> 0; };
  if (ctx) { add(utf8.encode(ctx)); add([4]); }
  add(utf8.encode(text));
  return h.toString(16).padStart(8, '0');
}
// English has no table: nothing to look up, nothing to hash.
const lookup = (ctx, text) => words.entries ? words.entries[wordKey(ctx, text)] : undefined;
function fill(text, values) {
  return String(text).replace(/\{\{|\}\}|\{(\d)\}/g, (m, d) => m === '{{' ? '{' : m === '}}' ? '}'
    : String(values[Number(d)] ?? ''));
}
function t(text, ...values) {
  const found = lookup(null, text);
  return fill(typeof found === 'string' ? found : text, values);
}
function tc(ctx, text, ...values) {
  const found = lookup(ctx, text);
  return fill(typeof found === 'string' ? found : text, values);
}
function tn(one, other, n, ...values) {
  const forms = lookup(null, one);
  let text = n === 1 ? one : other;
  if (Array.isArray(forms)) {
    const i = words.plural.indexOf(new Intl.PluralRules(words.locale).select(n));
    text = forms[i >= 0 ? i : forms.length - 1];
  }
  return fill(text, [n, ...values]);
}
const tx = text => text;
const nt = text => text; // text deliberately not translated (a product's name), like NOTR() in C
const escWords = text => String(text).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'})[c]);
function ht(text, ...values) {
  return escWords(t(text, ...values));
}
// ht() around values that are already HTML (a link, a tag the caller escaped): the words are
// escaped, the values go in as they are.
function hx(text, ...html) {
  const found = lookup(null, text);
  return escWords(typeof found === 'string' ? found : text).replace(/\{\{|\}\}|\{(\d)\}/g,
    (m, d) => m === '{{' ? '{' : m === '}}' ? '}' : String(html[Number(d)] ?? ''));
}
// Dates and numbers the browser writes: the chosen language's way, or the browser's own for English.
const wordsLocale = () => words.entries ? words.locale : undefined;

// This browser's language (Display and sound): its own choice, kept in this browser only.
function browserLanguage() {
  try { return localStorage.getItem('panel-language') || ''; } catch { return ''; }
}
// Without one, the panel's own language (I18N-031): /i18n/panel says which, before sign-in too.
async function panelLanguage() {
  try {
    const response = await fetch('/i18n/panel', {cache: 'no-store'});
    return response.ok ? String((await response.json()).locale || '') : '';
  } catch { return ''; }
}
async function loadWords() {
  const locale = browserLanguage() || await panelLanguage();
  if (!locale || /^en(-US)?$/i.test(locale)) return; /* English is the source: no catalog */
  try {
    const response = await fetch('/i18n/' + encodeURIComponent(locale) + '.json', {cache: 'no-cache'});
    if (!response.ok) return; // not on this panel: English
    const doc = await response.json();
    Object.assign(words, {locale: doc.locale, plural: doc.plural, entries: doc.entries || {}});
    document.documentElement.lang = doc.locale;
  } catch { /* English */ }
}
// panel.html's own words: an element marked data-t has its text translated, one marked
// data-t-label its aria-label (tools/i18n.py reads both). Once, after loadWords().
function translatePage(root = document) {
  if (!words.entries) return;
  for (const el of root.querySelectorAll('[data-t]')) el.textContent = t(el.textContent.trim());
  for (const el of root.querySelectorAll('[data-t-label]')) el.setAttribute('aria-label', t(el.getAttribute('aria-label')));
}
