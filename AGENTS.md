# AGENTS.md — RepairDesk Desktop

Guidance for AI coding agents (and humans) working in this repository. Read this file before making
changes, and **update it at the end of every task** (see "Maintaining this file").

---

## 1. Project overview

A Windows 11–styled desktop client for the [RepairDesk](https://www.repairdesk.co/) public API.

| Aspect | Details |
| --- | --- |
| Language | C++20 |
| Platform | Windows only (Win32 + DirectX 11) |
| UI | Dear ImGui (`v1.91.9b`, fetched by CMake) with `imgui_stdlib`, Win32 + DX11 backends |
| JSON | nlohmann/json `v3.12.0` (header only, fetched by CMake) |
| Networking | WinHTTP (built in, no external SDK) |
| Build | CMake ≥ 3.24 + Ninja; CLion bundled MinGW works out of the box |
| Settings | `%APPDATA%\RD Desktop Software\settings.json` |

Build from a terminal:

```
cmake -S . -B build -G Ninja
cmake --build build
build\RD_Desktop_Software.exe
```

The first configure requires internet access (FetchContent downloads ImGui + json).

## 2. Repository map

```
CMakeLists.txt            Single target: RD_Desktop_Software (WIN32 exe). New .cpp files MUST be added here.
README.md                 User-facing docs: building, OAuth/API-key setup, feature list.
AGENTS.md                 This file.
src/
  main.cpp                Win32 window + DX11 device/swap chain, ImGui backend init, main loop.
  App.h / App.cpp         Top-level application state, navigation rail, page routing, global search bar.
  Pages.h                 Declarations of every page/panel draw function.
  pages/ListPage.h        Shared paged/searchable list scaffolding used by Tickets/Invoices/Customers.
  pages/InventoryPage.cpp Inventory table, column chooser, bulk actions, product page.
  pages/StockTakePanel.cpp Right-hand stock-take sidebar (SKU → UPC → name matching, Submit).
  pages/PosPage.cpp       Product grid, barcode entry, cart, invoice creation.
  pages/TicketsPage.cpp / InvoicesPage.cpp / CustomersPage.cpp  Paged lists → RecordViewer.
  pages/RecordViewer.cpp  Generic "show every field of a JSON record" viewer.
  pages/CommunicationPage.cpp  SMS inbox + send-SMS form.
  pages/SearchPage.cpp    Global search results across products/invoices/tickets/customers.
  pages/SettingsPage.cpp  API key / OAuth2 credentials, theme, accent colour.
  DataTable.h/.cpp        Reusable ImGui table: column visibility, reorder, resize, sort, multi-select, click-to-edit.
  Widgets.h/.cpp          Custom Win11-styled ImGui widgets (buttons, inputs, toggles, etc.).
  Theme.h/.cpp            Dark/light theme + accent colour application.
  Settings.h/.cpp         Load/save settings.json (including OAuth tokens).
  Http.h/.cpp             WinHTTP wrapper (GET/POST/PUT/DELETE, headers, query params).
  RepairDeskApi.h/.cpp    Typed wrappers over the RepairDesk Public API; OAuth2 token exchange/refresh.
  Async.h                 Helpers for running API calls off the UI thread and polling results per frame.
```

## 3. Conventions and rules

### Code style
- Follow the style of the surrounding file: 4-space indentation, `PascalCase` for types/functions,
  `camelCase` for locals/members, `#pragma once` headers.
- Keep comments sparse and in English; match the existing comment density.
- Prefer `std::string` + `imgui_stdlib` (`ImGui::InputText(..., &str)`) over fixed `char` buffers.
- Use `nlohmann::json` with `.value(key, default)` / `.contains()` — RepairDesk responses are
  inconsistent (fields may be missing, numeric fields may arrive as strings).

### Architecture rules
- **Never block the UI thread** with a network call. Use the patterns in `Async.h`; poll results in the
  page's draw function.
- All HTTP goes through `Http.*`; all RepairDesk endpoints go through `RepairDeskApi.*`. Pages must not
  build URLs themselves.
- Authentication: prefer `Authorization: Bearer <token>` (OAuth2). Fall back to the `api_key` query
  parameter only when no OAuth token exists. Token refresh is automatic — don't duplicate it in pages.
- Nothing in **Stock take** writes to RepairDesk until **Submit** is pressed. Preserve this.
- Colours/sizes come from `Theme.*`; do not hard-code accent colours in pages.
- Adding a new page: declare in `Pages.h`, implement in `src/pages/`, register in `App.cpp`
  navigation, and add the `.cpp` to `CMakeLists.txt`.

### Dependencies
- Don't add new third-party libraries without a strong reason. If you must, use `FetchContent` in
  `CMakeLists.txt` with a pinned tag/hash, like the existing ImGui/json entries.
- Windows-only APIs are fine; there is no cross-platform requirement.

### Verification
- There is no automated test suite yet. At minimum, verify a change compiles:
  `cmake --build build` (or `cmake-build-debug` from CLion).
- For UI changes, run the exe and exercise the affected page manually. Describe what you checked in
  your summary.
- Don't commit `cmake-build-*`/`build` directories or `settings.json`.

### Documentation
- When user-visible behaviour changes, update `README.md` (Features / Connecting sections).
- When agent-relevant knowledge changes (new rule, pitfall, workflow), update **this file**.

---

## 4. Progress log

Newest entries first. One line per task: date, what changed, status.

| Date | Change | Status |
| --- | --- | --- |
| 2026-10-07 | Stock take submit sent the scanned count as `in_stock`, but RepairDesk applies `in_stock` in `PUT /inventory/{id}` as a *change* (adjustment), so stock was being added instead of set. `StockTakePanel.cpp` now sends `scanned − dbQty` (`TargetEntry::change`), skips items with change 0, and the Submit tooltip/README say "adjusts by the change". | Done (awaiting user re-test against live API) |
| 2026-10-07 | OAuth token exchange: `tokenRequest` now unwraps RepairDesk's `{success, statusCode, message, data}` envelope (tokens and `error`/`error_description`/`hint` live in `data`, HTTP is always 200), `finishOAuthWithCode` accepts a full redirect URL, token-exchange failures get a redirect-URI/credentials hint, loopback landing page no longer claims "connected" before the exchange; README gained an OAuth troubleshooting section. | Done (awaiting user re-test with real credentials) |
| 2026-10-07 | Stock take submit returned HTTP 500: `InventoryItem::toUpdatePayload` now sends only the fields of the API's `Item` schema (scalars; reference objects collapse to their id) instead of echoing the whole list record. After submit only the updated items are re-read (`RepairDeskApi::getInventoryItem`, keyword=id) and swapped into the cache via `App::replaceInventoryItem` (+ `inventoryRevision` so the table re-sorts); no full inventory reload. Updated rows leave the sidebar; failed ones stay for re-submit. | Done (awaiting user re-test against live API) |
| 2026-10-07 | Inventory refresh progress bar: 3px accent bar along the top edge (`App::drawLoadProgressBar`), previous item count = 100%, extra items drawn as a dotted segment; `RepairDeskApi::listInventory` gained an optional per-page progress callback. | Done |
| 2026-10-06 | Created `AGENTS.md` with project map, conventions, progress log, lessons and prompt wishes. | Done |
| (earlier) | Core app: Win32/DX11 shell, ImGui UI, Inventory + DataTable, Stock take, POS, Tickets/Invoices/Customers, Communication, Global search, Settings (API key + OAuth2), theming. | Done (per README) |

### Open items / backlog
- [ ] No automated tests; consider a small console test target for `RepairDeskApi` JSON parsing and
      `Settings` round-tripping (these don't need a window).
- [ ] Document known RepairDesk API quirks (pagination parameters, rate limits, field names) in
      §5 as they are discovered.
- [ ] Inventory bulk actions and inline edits (`InventoryPage.cpp`) still call `app.refreshInventory()`
      after saving; switch them to the per-item `getInventoryItem` + `replaceInventoryItem` path used by
      the stock take.
- [ ] Bulk "Edit quantity", inline `in_stock` edits and the product page (`InventoryPage.cpp`) still send
      the typed value as `in_stock`; since the API treats it as an adjustment they must be converted to
      `new − current` (or relabelled as "adjust by") the same way the stock take now does.
- [x] Add a short "Troubleshooting" section to `README.md` (OAuth redirect issues, firewall prompts
      for the loopback listener on port 8731). (2026-10-07)
- [ ] Loopback listener (`http::waitForLoopbackRedirect`) accepts a single IPv4 connection and has no
      `recv` timeout; consider dual-stack (`::1`) listening, skipping favicon/preconnect requests and
      parsing `code` from a POST body if RepairDesk ever switches to `response_mode=form_post`.

## 5. Lessons learned

Things that cost time before. Add to this list whenever a task hits a non-obvious problem.

- **Keep the UI thread free.** Any synchronous WinHTTP call in a draw function freezes the window;
  always go through `Async.h`.
- **RepairDesk JSON is loosely typed.** Numbers sometimes come back as strings and optional fields are
  simply absent. Always use defaulted access (`j.value("qty", 0)`), never `j["qty"].get<int>()`.
- **Legacy `api_key` vs OAuth.** The two auth modes must not be sent together. Bearer header wins when a
  token is present; `api_key` is only a fallback.
- **New source files need a CMake entry.** Forgetting to add a `.cpp` to `CMakeLists.txt` produces
  confusing "undefined reference" link errors, not compile errors.
- **`NOMINMAX` / `WIN32_LEAN_AND_MEAN` are defined project-wide.** Use `std::min`/`std::max`; don't
  rely on Windows macros or on headers excluded by lean-and-mean (include them explicitly).
- **Static MinGW runtime.** The exe is linked with `-static-libgcc -static-libstdc++` so it runs on
  machines without MinGW. Don't remove these flags when touching link options.
- **First CMake configure needs network.** FetchContent downloads ImGui and json; offline builds fail
  at configure time, not at compile time.
- **Worker-thread progress goes through `std::atomic`.** `Async::run` continuations run on the UI
  thread, but progress callbacks (e.g. `listInventory`'s per-page callback) run on the worker. Write
  only to an atomic counter from there and read it in the draw function; never touch ImGui or
  `App` vectors from the worker.
- **RepairDesk OAuth endpoints always answer HTTP 200.** `/oauth2/authorize`, `/oauth2/token` and
  `/oauth2/revoke` wrap the RFC 6749 payload as `{"success":bool,"statusCode":N,"message":"Bad Request",
  "data":{"error":"invalid_grant","error_description":"..."}}`. Check `success`/`statusCode` and read
  tokens/errors from `data`; the top-level `message` is just the generic HTTP reason phrase. The
  Swagger spec (form-encoded body) and the help-center article (JSON body) disagree — the live server
  accepts both. Verify endpoint behaviour with dummy credentials via `Invoke-WebRequest` before guessing.
- **`PUT /inventory/{id}` wants a flat `Item` body, not the list record.** Echoing a raw list item
  (nested `manufacturer`/`tax_class`/`condition` objects, `customer_group_prices` arrays, computed
  fields) back to the server produces an "internal server error" (HTTP 500). Build update bodies with
  `InventoryItem::toUpdatePayload`, which whitelists the documented fields and collapses reference
  objects to ids. The spec lives at <https://api-docs.repairdesk.co/rd-unresolved-swagger.json>
  (components → schemas → `Item`) — fetch it instead of guessing field names.
- **`in_stock` in `PUT /inventory/{id}` is a delta, not an absolute quantity.** RepairDesk adds the
  value to the current stock (it does not accept an overwrite). Send `desired − current` and skip the
  request when the change is 0 (see `applyToRepairDesk` in `StockTakePanel.cpp`). Re-read the item
  afterwards to pick up the server's resulting quantity.
- **`GET /inventory?keyword=<id>` fetches a single item.** The keyword filter matches id, name and SKU,
  so filter the result by exact id (`RepairDeskApi::getInventoryItem`). Use it to refresh individual
  rows after an update rather than re-downloading the whole inventory.
- **A running `RD_Desktop_Software.exe` blocks linking** (`LNK1168: cannot open ... for writing`).
  Close the app before building, or link into a scratch dir with
  `-DFETCHCONTENT_SOURCE_DIR_IMGUI=<build>\_deps\imgui-src -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<build>\_deps\nlohmann_json-src`
  to reuse the downloaded deps offline. `cmake-build-debug` is configured for MSVC via CLion's bundled
  CMake, so from a plain terminal run it inside `VsDevCmd.bat -arch=x64`.

## 6. Prompt wishes

What a well-formed task request should include so an agent can work without guesswork. When a task
was ambiguous, record here what information would have helped.

### Please include in requests
- **Which page/feature** is affected (e.g. "Inventory bulk edit", "Stock take sidebar", "Settings →
  OAuth"), ideally with the file name if known.
- **Expected vs. actual behaviour** for bugs, plus the RepairDesk endpoint involved if API-related.
- **Sample API payload** (anonymised) when the task depends on response shape — the public docs at
  <https://api-docs.repairdesk.co/> are often incomplete.
- **Scope boundaries**: whether README/AGENTS updates are wanted, and whether running the app
  manually is acceptable as verification.
- **Visual intent** for UI work: a sketch, screenshot, or reference to a Win11 control.

### Wishes gathered from past tasks
- (2026-10-06) When asking for repository meta-files (like this one), say whether they should be
  prescriptive (rules) or descriptive (status), or both.
- (2026-10-07) For OAuth/connection bugs, include the exact text of the info bar under **Sign in with
  RepairDesk** and what the browser tab showed after consent; "not receiving a token" alone does not
  distinguish a listener problem from a token-exchange rejection.
- (2026-10-07) For "internal server error" reports, paste the full toast text (it includes the server's
  `message`) and, if possible, the JSON of one affected item from the inventory list; the 500 could not
  be reproduced without live credentials, so the fix was derived from the API schema.
- (2026-10-07) When an API field behaves differently from its documentation (e.g. `in_stock` being an
  adjustment rather than a value), state the observed before/after numbers ("had 5, scanned 3, ended
  up with 8") so the semantics are unambiguous and other callers of the same field can be checked.

---

## 7. Maintaining this file

At the end of every task, the agent should:
1. Add a row to **§4 Progress log** (newest first) and tick/add backlog items.
2. Add any non-obvious pitfall to **§5 Lessons learned**.
3. If the request was ambiguous or missing context, note what was needed in **§6 Prompt wishes**.
4. Update **§2 Repository map** and **§3 Conventions** if files or rules changed.

Keep entries short; this file is read at the start of every session.
