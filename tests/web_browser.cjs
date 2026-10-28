// Необязательная проверка интерфейса в Chromium. Playwright нужен только для QA,
// сервер и страница работают без Node.js и без npm-зависимостей.
const { chromium } = require(
  process.env.SEARCH_PLAYWRIGHT_PATH || "playwright",
);
const assert = require("node:assert/strict");
const path = require("node:path");
let activeBrowser;

(async () => {
  const browser = await chromium.launch({
    headless: true,
    args: ["--no-sandbox", "--no-proxy-server"],
  });
  activeBrowser = browser;
  const base = process.env.SEARCH_WEB_URL || "http://127.0.0.1:8080";
  const output =
    process.env.SEARCH_SCREENSHOTS || path.join(__dirname, "..", "reports");
  const errors = [];
  const page = await browser.newPage({
    viewport: { width: 1440, height: 1080 },
    deviceScaleFactor: 1,
  });
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => {
    if (
      message.type() === "error" &&
      !message.text().includes("400 (Bad Request)")
    )
      errors.push(message.text());
  });
  const waitDocument = () =>
    page.locator(".body-text").waitFor({ state: "visible" });
  const waitIdle = () =>
    page.locator('#results[aria-busy="false"]').waitFor({ state: "visible" });
  const ids = () =>
    page
      .locator(".result-card")
      .evaluateAll((cards) => cards.map((card) => Number(card.dataset.id)));
  await page.goto(base);
  await page.locator("#index:not([disabled])").waitFor({ state: "visible" });
  assert.equal(await page.locator("#index").inputValue(), "index-full");
  await page.screenshot({
    path: path.join(output, "web-home.png"),
    fullPage: true,
  });
  await page.locator("#query").fill("machine ANDNOT (python OR model)");
  await page.locator("#search-button").click();
  await waitDocument();
  const firstIds = await ids();
  assert.equal(firstIds.length, 10);
  assert(firstIds.includes(166));
  await page.locator('.result-card[data-id="166"] button').click();
  await page
    .locator(".document-meta")
    .filter({ hasText: "msmarco_doc_00_2263019" })
    .waitFor();
  assert(
    (await page.locator(".body-text").textContent()).includes(
      "Your espresso is ready",
    ),
  );
  assert(
    (await page.locator(".headings-text").textContent()).includes(
      "How to use E.S.E. pods",
    ),
  );
  assert.equal(
    await page
      .locator(".body-text mark")
      .allTextContents()
      .then((items) => items.includes("models")),
    false,
  );
  const selectedPreview = page.locator(
    '.result-card[data-id="166"] .result-snippet[data-state="ready"]',
  );
  await selectedPreview.waitFor();
  assert.equal(
    await selectedPreview.locator(".snippet-field").textContent(),
    "BODY",
  );
  assert(
    (await selectedPreview.locator("mark").allTextContents()).some(
      (word) => word.toLowerCase() === "machine",
    ),
  );
  const matches = await page.locator(".document-content mark").count();
  assert(matches >= 2);
  await page.locator(".match-jump").click();
  assert.equal(
    await page.locator(".match-count").textContent(),
    `1 из ${matches}`,
  );
  assert.equal(await page.locator("mark.match-active").count(), 1);
  await page
    .getByRole("button", { name: "Следующее совпадение", exact: true })
    .click();
  assert.equal(
    await page.locator(".match-count").textContent(),
    `2 из ${matches}`,
  );
  await page
    .getByRole("button", { name: "Предыдущее совпадение", exact: true })
    .click();
  assert.equal(
    await page.locator(".match-count").textContent(),
    `1 из ${matches}`,
  );
  await page.evaluate(() => scrollTo(0, 0));
  await page.screenshot({
    path: path.join(output, "web-desktop.png"),
    fullPage: true,
  });
  await page.locator("#next").click();
  await waitIdle();
  assert.equal(await page.locator("#page-number").textContent(), "Страница 2");
  assert((await ids())[0] > firstIds.at(-1));
  await page.locator("#previous").click();
  await waitIdle();
  assert.equal(await page.locator("#page-number").textContent(), "Страница 1");
  assert.deepEqual(await ids(), firstIds);
  await page.locator("#mode").selectOption("sequential");
  await waitIdle();
  assert.deepEqual(await ids(), firstIds);
  await page.locator("#query").fill("machine AND");
  await page.locator("#search-button").click();
  await page.locator("#error").waitFor({ state: "visible" });
  assert((await page.locator("#error").textContent()).includes("операнд"));
  await page.locator("#index").selectOption("index-50k-snippets");
  await page.locator("#query").fill("machine AND learning");
  await page.locator("#search-button").click();
  await waitDocument();
  assert.equal(await page.locator("#error").isVisible(), false);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.locator(".result-card").first().locator("button").click();
  await waitDocument();
  assert(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  );
  await page.screenshot({
    path: path.join(output, "web-mobile.png"),
    fullPage: true,
  });

  // Отдельный небольшой корпус проверяет, что HTML и javascript: URL из входных
  // данных не исполняются, а body действительно полный, независимо от сниппета.
  if (process.env.SEARCH_FIXTURE_URL) {
    const documentRequests = [];
    page.on("request", (request) => {
      if (
        request
          .url()
          .startsWith(`${process.env.SEARCH_FIXTURE_URL}/api/document?`)
      )
        documentRequests.push(request.url());
    });
    await page.goto(process.env.SEARCH_FIXTURE_URL);
    await page.locator("#index:not([disabled])").waitFor({ state: "visible" });
    await page.locator("#query").fill("machine");
    await page.locator("#search-button").click();
    await waitDocument();
    assert(
      (await page.locator(".document-title").textContent()).includes(
        "<script>",
      ),
    );
    assert.equal(await page.locator("#document script").count(), 0);
    assert.equal(await page.evaluate(() => window.searchXss), undefined);
    assert(
      (await page.locator(".body-text").textContent()).includes(
        "FULL BODY END",
      ),
    );
    assert.equal(
      await page.locator(".document-link").getAttribute("href"),
      null,
    );
    const preview = page.locator('.result-snippet[data-state="ready"]').first();
    await preview.waitFor();
    assert(
      (await preview.locator(".result-preview").textContent()).includes(
        "FULL BODY END",
      ),
    );
    assert(
      (await preview.locator(".result-preview").textContent()).startsWith("…"),
    );
    assert(
      (await preview.locator("mark").allTextContents()).some(
        (word) => word.toLowerCase() === "machine",
      ),
    );
    assert.equal(
      await page.evaluate(
        () => document.documentElement.scrollWidth <= innerWidth,
      ),
      true,
    );
    // Панель и сниппет используют общий запрос, повторный выбор — кэш браузера.
    assert.equal(documentRequests.length, 1);
    await page.locator(".result-card").first().locator("button").click();
    await waitDocument();
    assert.equal(documentRequests.length, 1);
  }
  assert.deepEqual(errors, []);
  await browser.close();
  console.log(
    "Browser QA passed: full fields, contextual snippets, exact highlights, match navigation, pagination, algorithms, index switch, errors, mobile, XSS.",
  );
})().catch(async (error) => {
  console.error(error);
  if (activeBrowser) await activeBrowser.close();
  process.exitCode = 1;
});
