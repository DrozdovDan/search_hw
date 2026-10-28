const test = require("node:test");
const assert = require("node:assert/strict");
const {
  normalizeWord,
  findMatches,
  makeSnippet,
  documentSnippet,
} = require("../web/text.js");

test("Подсветка ищет целые Unicode-термы, без стемминга и подстрок", () => {
  const text = "Model models MODEL model_foo МАШИНА машина";
  const matches = findMatches(text, new Set(["model", "машина"]));
  assert.deepEqual(
    matches.map((match) => text.slice(match.start, match.end)),
    ["Model", "MODEL", "model", "МАШИНА", "машина"],
  );
  assert.equal(normalizeWord("ΟΣ"), "οσ");
  assert.equal(normalizeWord("İ"), "i");
});

test("Сниппет находит слово далеко за начальным префиксом документа", () => {
  const source =
    "Ненужное вступление. ".repeat(100) +
    "Здесь machine learning и важный контекст. FULL BODY END";
  const result = makeSnippet(source, new Set(["machine", "learning"]));
  assert(result.matched);
  assert(result.text.startsWith("… "));
  assert(result.text.includes("machine learning"));
  assert(result.text.endsWith("FULL BODY END"));
  assert(result.text.length < 335);
});

test("Окно предпочитает несколько разных слов повторам одного", () => {
  const source =
    "machine ".repeat(80) + "пусто ".repeat(80) + "python рядом model";
  const result = makeSnippet(source, new Set(["machine", "python", "model"]));
  assert(result.text.includes("python рядом model"));
  assert(!result.text.includes("machine"));
});

test("Источник фрагмента выбирается из body, headings или title", () => {
  const terms = new Set(["python"]);
  assert.equal(
    documentSnippet({ body: "python в тексте", title: "python" }, terms).field,
    "body",
  );
  assert.equal(
    documentSnippet(
      { body: "Иной текст", headings: "Python", title: "python" },
      terms,
    ).field,
    "headings",
  );
  assert.equal(
    documentSnippet({ body: "Иной текст", title: "Python" }, terms).field,
    "title",
  );
  assert.equal(
    documentSnippet(
      { body: "models", title: "Без совпадения" },
      new Set(["model"]),
    ).matched,
    false,
  );
  assert.equal(documentSnippet({}, terms).field, "");
});

test("Сниппет сохраняет обычные слова и схлопывает переводы строк", () => {
  const source = "Начало\n\n\tPython\r\nконец";
  assert.equal(
    makeSnippet(source, new Set(["python"])).text,
    "Начало Python конец",
  );
  assert.equal(
    makeSnippet("<script>alert(1)</script> python", new Set(["python"])).text,
    "<script>alert(1)</script> python",
  );
});

test("Жёсткая граница не разрезает UTF-16 и не раздувает длинный токен", () => {
  const result = makeSnippet(
    "😀".repeat(250) + " python " + "😀".repeat(250),
    new Set(["python"]),
    64,
  );
  assert(result.text.includes("python"));
  assert(
    !/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(
      result.text,
    ),
  );
  const long = makeSnippet("a".repeat(20000), new Set(["python"]));
  assert(!long.matched);
  assert(long.text.length < 335);
});
