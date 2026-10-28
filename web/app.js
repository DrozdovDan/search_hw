"use strict";

// Текст корпуса всегда вставляется через textContent/TextNode. Даже документ
// с HTML или <script> показывается как текст и не становится кодом страницы.
const $ = (selector) => document.querySelector(selector);
const ui = {
  form: $("#search-form"),
  query: $("#query"),
  index: $("#index"),
  limit: $("#limit"),
  mode: $("#mode"),
  button: $("#search-button"),
  error: $("#error"),
  welcome: $("#welcome"),
  workspace: $("#workspace"),
  results: $("#results"),
  document: $("#document"),
  previous: $("#previous"),
  next: $("#next"),
};
const state = {
  indexes: [],
  results: [],
  terms: new Set(),
  page: 0,
  cursors: [null],
  hasMore: false,
  selection: null,
  searchRequest: null,
  documentRequest: null,
  searching: false,
  snapshot: null,
  previewRequest: null,
  previewObserver: null,
  documentCache: new Map(),
  pendingDocuments: new Map(),
  cachedBytes: 0,
};
const format = new Intl.NumberFormat("ru-RU");
const compact = new Intl.NumberFormat("ru-RU", {
  notation: "compact",
  maximumFractionDigits: 2,
});

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
function highlighted(text, terms = state.terms) {
  const fragment = document.createDocumentFragment();
  const source = String(text ?? "");
  let end = 0;
  for (const match of MarcoText.findMatches(source, terms)) {
    // Несовпавшие слова объединяются в один TextNode. Для длинной книги это
    // существенно меньше DOM-узлов, чем отдельный узел на каждое слово/пробел.
    fragment.append(
      document.createTextNode(source.slice(end, match.start)),
      element("mark", "", source.slice(match.start, match.end)),
    );
    end = match.end;
  }
  fragment.append(document.createTextNode(source.slice(end)));
  return fragment;
}
function safeUrl(value) {
  try {
    const url = new URL(value);
    return ["http:", "https:"].includes(url.protocol) ? url.href : null;
  } catch {
    return null;
  }
}
function domain(value) {
  try {
    return new URL(value).hostname.replace(/^www\./, "");
  } catch {
    return "Документ корпуса";
  }
}
function link(label, value, className) {
  const url = safeUrl(value);
  const node = element(url ? "a" : "span", className, label);
  if (url) {
    node.href = url;
    node.target = "_blank";
    node.rel = "noopener noreferrer";
  }
  return node;
}
async function api(path, options = {}) {
  const response = await fetch(path, options);
  let data;
  try {
    data = await response.json();
  } catch {
    throw new Error("Сервер вернул неожиданный ответ. Проверьте подключение.");
  }
  if (!response.ok)
    throw new Error(data.error || `Ошибка сервера: ${response.status}`);
  return data;
}
function rememberDocument(key, data) {
  const bytes = Object.values(data).reduce(
    (sum, value) => sum + (typeof value === "string" ? value.length * 2 : 0),
    0,
  );
  const budget = 24 * 1024 * 1024;
  if (bytes > budget) return;
  const previous = state.documentCache.get(key);
  if (previous) {
    state.cachedBytes -= previous.bytes;
    state.documentCache.delete(key);
  }
  state.documentCache.set(key, { data, bytes });
  state.cachedBytes += bytes;
  while (state.cachedBytes > budget || state.documentCache.size > 12) {
    const oldest = state.documentCache.keys().next().value;
    state.cachedBytes -= state.documentCache.get(oldest).bytes;
    state.documentCache.delete(oldest);
  }
}
function loadDocument(result, index, signal) {
  const key = `${index}:${result.internal_id}`;
  const cached = state.documentCache.get(key);
  if (cached) {
    state.documentCache.delete(key);
    state.documentCache.set(key, cached);
    return Promise.resolve(cached.data);
  }
  const pending = state.pendingDocuments.get(key);
  if (pending?.signal === signal) return pending.promise;
  const parameters = new URLSearchParams({
    index,
    id: String(result.internal_id),
  });
  // Карточка и открытый документ разделяют один запрос. Смена выбранной
  // карточки не отменяет данные, которые ещё нужны её сниппету.
  const promise = api(`/api/document?${parameters}`, { signal })
    .then((data) => {
      if (!signal.aborted) rememberDocument(key, data);
      return data;
    })
    .finally(() => {
      if (state.pendingDocuments.get(key)?.promise === promise)
        state.pendingDocuments.delete(key);
    });
  state.pendingDocuments.set(key, { signal, promise });
  return promise;
}
function updateCardSnippet(card, data, terms) {
  const preview = card.querySelector(".result-snippet");
  const snippet = MarcoText.documentSnippet(data, terms);
  preview.dataset.state = "ready";
  preview.querySelector(".snippet-field").textContent =
    snippet.field.toUpperCase();
  preview
    .querySelector(".result-preview")
    .replaceChildren(highlighted(snippet.text, terms));
}
function observePreviews() {
  const controller = state.previewRequest;
  const index = state.snapshot.index;
  const terms = new Set(state.terms);
  const results = new Map(
    state.results.map((result) => [String(result.internal_id), result]),
  );
  const queue = [];
  let running = false;
  async function drain() {
    if (running) return;
    running = true;
    // Только один фоновый читатель: видимые карточки получают контекст постепенно,
    // без ста одновременных распаковок корпуса при большом размере страницы.
    while (queue.length && !controller.signal.aborted) {
      const card = queue.shift();
      try {
        const data = await loadDocument(
          results.get(card.dataset.id),
          index,
          controller.signal,
        );
        if (!controller.signal.aborted && card.isConnected)
          updateCardSnippet(card, data, terms);
      } catch (error) {
        if (error.name === "AbortError" || controller.signal.aborted) break;
        if (card.isConnected) {
          const preview = card.querySelector(".result-snippet");
          preview.dataset.state = "unavailable";
          preview.querySelector(".snippet-field").textContent = "";
          preview.querySelector(".result-preview").textContent =
            "Фрагмент недоступен. Откройте документ, чтобы повторить загрузку.";
        }
      }
    }
    running = false;
  }
  function enqueue(card) {
    if (card.dataset.previewQueued) return;
    card.dataset.previewQueued = "true";
    queue.push(card);
    void drain();
  }
  if (typeof IntersectionObserver === "undefined") {
    ui.results.querySelectorAll(".result-card").forEach(enqueue);
    return;
  }
  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries)
        if (entry.isIntersecting) {
          observer.unobserve(entry.target);
          enqueue(entry.target);
        }
    },
    { rootMargin: "200px 0px" },
  );
  state.previewObserver = observer;
  ui.results
    .querySelectorAll(".result-card")
    .forEach((card) => observer.observe(card));
}
function showError(message) {
  ui.error.textContent = message;
  ui.error.hidden = false;
}
function updateStats() {
  const index = state.indexes.find((item) => item.id === ui.index.value);
  $("#document-count").textContent = index
    ? compact.format(index.documents)
    : "—";
  $("#term-count").textContent = index ? compact.format(index.terms) : "—";
  $("#document-count").title = index ? format.format(index.documents) : "";
  $("#term-count").title = index ? format.format(index.terms) : "";
}
function placeholder(title, message, loading = false) {
  const container = element("div", "document-placeholder");
  container.append(
    element(
      "span",
      loading ? "loading-dot" : "placeholder-icon",
      loading ? "" : "↗",
    ),
    element("h2", "", title),
    element("p", "", message),
  );
  ui.document.replaceChildren(container);
  ui.document.setAttribute("aria-busy", String(loading));
}
function pagination() {
  $("#pagination").hidden = !state.results.length && !state.page;
  ui.previous.disabled = state.searching || state.page === 0;
  ui.next.disabled = state.searching || !state.hasMore;
  $("#page-number").textContent = `Страница ${state.page + 1}`;
}
function setBusy(value) {
  state.searching = value;
  ui.button.disabled = value;
  ui.button.replaceChildren(
    document.createTextNode(value ? "Ищем…" : "Найти "),
  );
  if (!value) ui.button.append(element("span", "", "↗"));
  ui.results.setAttribute("aria-busy", String(value));
  pagination();
}
function renderResults(data) {
  ui.results.replaceChildren();
  $("#result-count").textContent = String(state.results.length);
  $("#elapsed").textContent =
    `${new Intl.NumberFormat("ru-RU", { maximumFractionDigits: 2 }).format(data.elapsed_ms)} мс`;
  $("#results-note").textContent = state.results.length
    ? `По возрастанию ID · ${data.has_more ? "есть ещё результаты" : "конец выдачи"}`
    : "Попробуйте изменить слова или условия.";
  if (!state.results.length) {
    ui.results.append(
      element(
        "div",
        "empty-results",
        "Совпадений нет. Проверьте написание слов или используйте OR вместо AND.",
      ),
    );
    placeholder(
      "Ничего не найдено",
      "Текст появится, когда запрос найдёт подходящий документ.",
    );
    return;
  }
  state.results.forEach((result, i) => {
    const card = element("div", "result-card");
    card.dataset.id = String(result.internal_id);
    const button = element("button", "result-select");
    button.type = "button";
    button.setAttribute("aria-pressed", "false");
    const top = element("div", "result-top");
    top.append(
      element(
        "span",
        "result-number",
        String(state.page * Number(ui.limit.value) + i + 1).padStart(2, "0"),
      ),
      element("span", "result-domain", domain(result.url)),
      element("span", "result-arrow", "↗"),
    );
    const title = element("h3", "result-title");
    title.append(highlighted(result.title || "Документ без заголовка"));
    button.append(top, title);
    const snippet = element("div", "result-snippet");
    snippet.dataset.state = "loading";
    const preview = element("p", "result-preview");
    preview.append(
      result.snippet
        ? highlighted(result.snippet)
        : document.createTextNode("Подбираем фрагмент вокруг слов запроса…"),
    );
    snippet.append(
      element("span", "snippet-field", "ФРАГМЕНТ ТЕКСТА"),
      preview,
    );
    button.append(snippet);
    button.append(element("div", "result-id", result.docid));
    button.addEventListener("click", () => selectDocument(result, true));
    card.append(button);
    ui.results.append(card);
  });
}
function renderDocument(data) {
  const toolbar = element("div", "document-toolbar");
  const back = element("button", "document-back", "← К результатам");
  back.type = "button";
  back.addEventListener("click", () =>
    ui.results.scrollIntoView({ behavior: "smooth", block: "start" }),
  );
  toolbar.append(
    back,
    element("span", "", "ИСХОДНЫЙ ДОКУМЕНТ"),
    link("Открыть сайт ↗", data.url, "document-link"),
  );
  const content = element("div", "document-content");
  content.append(element("p", "field-label", "TITLE · ЗАГОЛОВОК"));
  const title = element("h2", "document-title");
  title.append(highlighted(data.title || "Без заголовка"));
  const metadata = element("div", "document-meta");
  metadata.append(
    element("span", "", data.docid),
    element("span", "", `ID ${data.internal_id}`),
  );
  content.append(
    title,
    link(data.url || "Адрес не указан", data.url, "document-url"),
    metadata,
  );
  for (const [key, label, className] of [
    ["headings", "HEADINGS · ПОДЗАГОЛОВКИ", "headings-text"],
    ["body", "BODY · ОСНОВНОЙ ТЕКСТ", "body-text"],
  ]) {
    const section = element("section", "field-section");
    section.append(element("h3", "field-label", label));
    if (data[key]?.trim()) {
      const text = element("div", className);
      text.append(highlighted(data[key]));
      section.append(text);
    } else
      section.append(
        element("p", "empty-field", "Поле не заполнено в корпусе."),
      );
    content.append(section);
  }
  const marks = Array.from(content.querySelectorAll("mark"));
  const navigation = element("div", "match-navigation");
  if (marks.length) {
    let active = -1;
    const jump = element("button", "match-jump", "К совпадениям ↓");
    jump.type = "button";
    const count = element("output", "match-count", `Найдено: ${marks.length}`);
    count.setAttribute("aria-live", "polite");
    const previous = element("button", "match-step", "↑");
    previous.type = "button";
    previous.setAttribute("aria-label", "Предыдущее совпадение");
    const next = element("button", "match-step", "↓");
    next.type = "button";
    next.setAttribute("aria-label", "Следующее совпадение");
    function move(position) {
      if (active >= 0) marks[active].classList.remove("match-active");
      active = (position + marks.length) % marks.length;
      const mark = marks[active];
      mark.classList.add("match-active");
      count.textContent = `${active + 1} из ${marks.length}`;
      // Прокручиваем только панель текста: поле запроса и список результатов
      // остаются на своих местах, включая мобильный режим.
      const top =
        content.scrollTop +
        mark.getBoundingClientRect().top -
        content.getBoundingClientRect().top;
      content.scrollTo({
        top: Math.max(0, top - content.clientHeight / 2),
        behavior: matchMedia("(prefers-reduced-motion: reduce)").matches
          ? "instant"
          : "smooth",
      });
    }
    jump.addEventListener("click", () => move(active < 0 ? 0 : active));
    previous.addEventListener("click", () =>
      move(active < 0 ? marks.length - 1 : active - 1),
    );
    next.addEventListener("click", () => move(active + 1));
    navigation.append(jump, count, previous, next);
  } else navigation.hidden = true;
  ui.document.replaceChildren(toolbar, navigation, content);
  ui.document.setAttribute("aria-busy", "false");
}
async function selectDocument(result, scroll = false) {
  state.documentRequest?.abort();
  const controller = new AbortController();
  state.documentRequest = controller;
  const pageController = state.previewRequest;
  state.selection = result.internal_id;
  for (const card of ui.results.querySelectorAll(".result-card")) {
    const selected = Number(card.dataset.id) === state.selection;
    card.classList.toggle("selected", selected);
    card.querySelector("button").setAttribute("aria-pressed", String(selected));
  }
  placeholder(
    "Читаем документ",
    "Загружаем полный текст из исходного корпуса. Первый запрос может занять несколько секунд.",
    true,
  );
  if (scroll && matchMedia("(max-width: 700px)").matches)
    ui.document.scrollIntoView({ behavior: "smooth", block: "start" });
  try {
    const data = await loadDocument(
      result,
      state.snapshot.index,
      pageController.signal,
    );
    if (
      controller.signal.aborted ||
      state.documentRequest !== controller ||
      state.previewRequest !== pageController
    )
      return;
    const card = ui.results.querySelector(
      `.result-card[data-id="${result.internal_id}"]`,
    );
    if (card) updateCardSnippet(card, data, state.terms);
    renderDocument(data);
  } catch (error) {
    if (
      error.name === "AbortError" ||
      controller.signal.aborted ||
      state.documentRequest !== controller
    )
      return;
    const box = element("div", "document-error");
    box.append(
      element("p", "", "Не удалось загрузить полный текст."),
      element("p", "", error.message),
    );
    const retry = element("button", "", "Повторить");
    retry.type = "button";
    retry.addEventListener("click", () => selectDocument(result));
    box.append(retry);
    ui.document.replaceChildren(box);
    ui.document.setAttribute("aria-busy", "false");
  }
}
async function search(page = 0, updateUrl = true) {
  const query = ui.query.value.trim();
  if (!query) {
    ui.query.focus();
    return;
  }
  if (!state.indexes.length) return;
  state.searchRequest?.abort();
  state.documentRequest?.abort();
  state.previewRequest?.abort();
  state.previewObserver?.disconnect();
  state.previewRequest = new AbortController();
  const controller = new AbortController();
  state.searchRequest = controller;
  if (page === 0) {
    state.cursors = [null];
    state.snapshot = {
      query,
      index: ui.index.value,
      limit: Number(ui.limit.value),
      mode: ui.mode.value,
    };
  }
  const previousPage = state.page;
  state.page = page;
  state.selection = null;
  ui.error.hidden = true;
  ui.welcome.hidden = true;
  ui.workspace.hidden = false;
  document.body.classList.add("has-results");
  setBusy(true);
  ui.results.replaceChildren(
    ...Array.from({ length: 3 }, () => element("div", "skeleton")),
  );
  placeholder("Ищем совпадения", "Составляем выдачу по вашему запросу.", true);
  try {
    const data = await api("/api/search", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ...state.snapshot, after: state.cursors[page] }),
      signal: controller.signal,
    });
    if (state.searchRequest !== controller) return;
    state.results = data.results;
    state.terms = new Set(data.terms);
    state.hasMore = data.has_more;
    if (data.has_more && data.results.length)
      state.cursors[page + 1] = data.results.at(-1).internal_id;
    renderResults(data);
    if (updateUrl) {
      const url = new URL(location.href);
      url.search = new URLSearchParams({
        q: state.snapshot.query,
        index: state.snapshot.index,
        limit: String(state.snapshot.limit),
        mode: state.snapshot.mode,
      });
      history.replaceState(null, "", url);
    }
    if (state.results.length) {
      void selectDocument(state.results[0]);
      observePreviews();
    }
  } catch (error) {
    if (error.name === "AbortError" || state.searchRequest !== controller)
      return;
    state.page = previousPage;
    state.results = [];
    state.hasMore = false;
    ui.results.replaceChildren();
    $("#result-count").textContent = "";
    $("#elapsed").textContent = "";
    $("#results-note").textContent = "";
    showError(error.message);
    placeholder("Запрос не выполнен", "Исправьте запрос и попробуйте ещё раз.");
  } finally {
    if (state.searchRequest === controller) setBusy(false);
  }
}
ui.form.addEventListener("submit", (event) => {
  event.preventDefault();
  void search();
});
document.querySelectorAll("[data-query]").forEach((button) =>
  button.addEventListener("click", () => {
    ui.query.value = button.dataset.query;
    void search();
  }),
);
ui.next.addEventListener("click", () => {
  if (state.hasMore && !state.searching) void search(state.page + 1);
});
ui.previous.addEventListener("click", () => {
  if (state.page && !state.searching) void search(state.page - 1);
});
for (const control of [ui.index, ui.limit, ui.mode])
  control.addEventListener("change", () => {
    updateStats();
    if (state.snapshot && ui.query.value.trim()) void search();
  });
document.addEventListener("keydown", (event) => {
  const editing = ["INPUT", "SELECT", "TEXTAREA"].includes(
    document.activeElement?.tagName,
  );
  if (event.key === "/" && !editing && !event.ctrlKey && !event.metaKey) {
    event.preventDefault();
    ui.query.focus();
  }
});
async function initialize() {
  try {
    const data = await api("/api/indexes");
    state.indexes = data.indexes;
    ui.index.replaceChildren(
      ...data.indexes.map((item) => {
        const option = element(
          "option",
          "",
          `${item.id} · ${compact.format(item.documents)}`,
        );
        option.value = item.id;
        return option;
      }),
    );
    const parameters = new URLSearchParams(location.search);
    const requestedIndex = parameters.get("index");
    ui.index.value = data.indexes.some((item) => item.id === requestedIndex)
      ? requestedIndex
      : data.default;
    if (["10", "25", "50", "100"].includes(parameters.get("limit")))
      ui.limit.value = parameters.get("limit");
    if (["binary", "sequential"].includes(parameters.get("mode")))
      ui.mode.value = parameters.get("mode");
    ui.index.disabled = false;
    $("#connection-text").textContent = "Сервер подключён";
    updateStats();
    if (parameters.get("q")) {
      ui.query.value = parameters.get("q");
      await search(0, false);
    }
  } catch (error) {
    $("#connection-text").textContent = "Нет подключения";
    showError(error.message);
  }
}
void initialize();
