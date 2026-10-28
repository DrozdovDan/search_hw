// Общая работа с термами для подсветки и контекстных сниппетов. Модуль не
// создаёт HTML: текст корпуса остаётся текстом, даже если содержит теги.
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.MarcoText = api;
})(globalThis, function () {
  "use strict";

  function normalizeWord(word) {
    // ICU simple lowercase работает с отдельными кодовыми точками, без
    // контекстного изменения греческой сигмы или расширения İ в два символа.
    return Array.from(word, (c) => (c === "İ" ? "i" : c.toLowerCase())).join(
      "",
    );
  }
  function findMatches(text, terms) {
    if (!terms.size) return [];
    const matches = [];
    for (const word of text.matchAll(/[\p{L}\p{N}][\p{L}\p{N}\p{M}]*/gu)) {
      const term = normalizeWord(word[0]);
      if (terms.has(term))
        matches.push({
          start: word.index,
          end: word.index + word[0].length,
          term,
        });
    }
    return matches;
  }
  function makeSnippet(value, terms, maxLength = 280) {
    const text = String(value ?? "")
      .replace(/\s+/gu, " ")
      .trim();
    const matches = findMatches(text, terms);
    if (text.length <= maxLength) return { text, matched: matches.length > 0 };

    let focusStart = 0,
      focusEnd = 0,
      bestScore = -1,
      left = 0;
    const counts = new Map();
    // Скользящее окно предпочитает несколько разных слов запроса поблизости,
    // а не множество повторов одного слова. Это только выбор фрагмента выдачи,
    // он не меняет порядок документов и булеву семантику поиска.
    for (let right = 0; right < matches.length; right++) {
      const current = matches[right];
      counts.set(current.term, (counts.get(current.term) || 0) + 1);
      while (
        left < right &&
        current.end - matches[left].start > maxLength * 0.65
      ) {
        const term = matches[left++].term;
        const count = counts.get(term) - 1;
        if (count) counts.set(term, count);
        else counts.delete(term);
      }
      const score = counts.size * 100 + Math.min(right - left + 1, 4);
      if (score > bestScore) {
        bestScore = score;
        focusStart = matches[left].start;
        focusEnd = current.end;
      }
    }
    let start = matches.length
      ? Math.max(
          0,
          focusStart - Math.floor((maxLength - (focusEnd - focusStart)) / 2),
        )
      : 0;
    let end = Math.min(text.length, start + maxLength);
    start = Math.max(0, end - maxLength);
    // Небольшое расширение до пробела сохраняет слова по краям. Ограничение
    // в 24 символа не даёт одному гигантскому токену занять всю карточку.
    if (start) {
      const space = text.lastIndexOf(" ", start);
      if (space >= start - 24) start = space + 1;
    }
    if (end < text.length) {
      const space = text.indexOf(" ", end);
      if (space >= 0 && space <= end + 24) end = space;
    }
    // Индексы JS измеряются в UTF-16. На жёсткой границе длинного слова не
    // разрезаем surrogate pair и не начинаем фрагмент с combining mark.
    if (start && /[\uDC00-\uDFFF]/u.test(text[start])) start++;
    while (start < end && /\p{M}/u.test(text[start])) start++;
    if (end < text.length && /[\uD800-\uDBFF]/u.test(text[end - 1])) end--;
    return {
      text: `${start ? "… " : ""}${text.slice(start, end).trim()}${end < text.length ? " …" : ""}`,
      matched: matches.length > 0,
    };
  }
  function documentSnippet(document, terms, maxLength = 280) {
    let fallback;
    for (const field of ["body", "headings", "title"]) {
      const snippet = makeSnippet(document[field], terms, maxLength);
      if (snippet.matched) return { ...snippet, field };
      if (!fallback && snippet.text) fallback = { ...snippet, field };
    }
    return (
      fallback || {
        text: "В документе нет текста для фрагмента.",
        field: "",
        matched: false,
      }
    );
  }
  return { normalizeWord, findMatches, makeSnippet, documentSnippet };
});
