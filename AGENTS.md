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
| Installer | Inno Setup 6 via `package.bat` → `dist\RD_Desktop_Software-<ver>-setup.exe` (**committed** so users need no build) |
| Settings | `%APPDATA%\RD Desktop Software\settings.json` |

Build with one click via `build.bat` (auto-detects CLion's bundled CMake/Ninja/MinGW, then PATH, then
Visual Studio; `run`/`debug`/`clean` arguments), or from a terminal:

```
cmake -S . -B build -G Ninja
cmake --build build
build\RD_Desktop_Software.exe
```

The first configure requires internet access (FetchContent downloads ImGui + json).

## 2. Repository map

```
CMakeLists.txt            Single target: RD_Desktop_Software (WIN32 exe). New .cpp files MUST be added here.
build.bat                 One-click build script (toolchain discovery, configure + build into build\, optional run).
package.bat               build.bat + Inno Setup -> dist\RD_Desktop_Software-<ver>-setup.exe (installs Inno Setup via winget if missing).
installer/RD_Desktop_Software.iss  Inno Setup script (per-user default, Start menu + optional desktop shortcut, uninstaller; ships exe + *.dll + RDicon.ico;
                          fixed AppId + UsePrevious* = in-place updates, [Code] reads the previous version/dir from the uninstall key).
                          Version lives in package.bat (`VERSION`) and the .iss fallback `AppVersion`; bump both per release.
installer/RDicon.ico      App icon (generated from RDicon.png by tools\make_icon.ps1); embedded via src/app.rc, SetupIconFile, shortcut IconFilename.
RDicon.png                Icon source image (51x53). Replace it and re-run tools\make_icon.ps1 to change the icon.
tools/make_icon.ps1       PNG -> multi-size PNG-compressed .ico (System.Drawing, no external tool).
dist/                     Ready-made installer, committed to git on purpose. Re-run package.bat after code changes.
README.md                 User-facing docs: building, OAuth/API-key setup, feature list.
AGENTS.md                 This file.
src/
  main.cpp                Win32 window + DX11 device/swap chain, ImGui backend init, main loop; window class icon = IDI_APPICON.
  app.rc / resource.h     Windows resources: IDI_APPICON (app icon). Added to the CMake target (project LANGUAGES CXX RC).
  App.h / App.cpp         Top-level application state, navigation rail, page routing, global search bar.
  Pages.h                 Declarations of every page/panel draw function.
  pages/ListPage.h        Shared paged/searchable list scaffolding used by Tickets/Invoices/Customers.
  pages/InventoryPage.cpp Inventory table, column chooser, bulk actions, product page (suggest drop-downs, price pill + formula popup).
  pages/StockTakePanel.cpp Right-hand stock-take sidebar (SKU → UPC → name matching, Submit).
  pages/PosPage.cpp       Product grid, barcode entry, cart, invoice creation.
  pages/TicketsPage.cpp / InvoicesPage.cpp / CustomersPage.cpp  Paged lists → RecordViewer.
  pages/RecordViewer.cpp  Generic "show every field of a JSON record" viewer.
  pages/CommunicationPage.cpp  SMS inbox + send-SMS form.
  pages/SearchPage.cpp    Global search results across products/invoices/tickets/customers.
  pages/SettingsPage.cpp  API key / OAuth2 credentials, theme, accent colour, font size.
  DataTable.h/.cpp        Reusable ImGui table: column visibility, reorder, resize, sort, multi-select; layout persisted by
                          column key in Settings::tableLayouts (bindSettings); single-click inline editing with Tab/Shift+Tab
                          along the row (staged, one onRowEdited per row), double-click = onRowDoubleClicked.
  Widgets.h/.cpp          Custom Win11-styled ImGui widgets (buttons, inputs, toggles, SuggestField/ComboField, PillButton).
  Theme.h/.cpp            Dark/light theme + accent colour application.
  Settings.h/.cpp         Load/save settings.json (OAuth tokens, theme/accent/fontSize, TableLayout per table id,
                          inventoryLastCount for the progress bar, suggested-price formula `suggestedPrice()`).
  Http.h/.cpp             WinHTTP wrapper (GET/POST/PUT/DELETE, headers, query params).
  RepairDeskApi.h/.cpp    Typed wrappers over the RepairDesk Public API; OAuth2 token exchange/refresh;
                          `Lookups` reference data (manufacturers/devices/suppliers/tax classes/locations) and
                          `resolveLookupIds()` which turns UI names into the ids the write endpoints need.
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
- Inventory writes go through `RepairDeskApi::addInventoryItem` / `updateInventoryItem`, which run the body
  through `resolveLookupIds()` using `App::lookups` (set via `api()`). Pages pass *names* for manufacturer,
  device, supplier, tax class and location; never hand-build id mappings in a page. Pass a `WriteNotes`
  and surface its `messages` to the user (dropped unknown values, created brands/models).
- Colours/sizes come from `Theme.*`; do not hard-code accent colours in pages.
- **User preferences live in `settings.json`, nowhere else.** Anything the user customises (theme, accent,
  font size, table layouts, credentials) goes into `Settings` so an installer update keeps it. Table
  layouts are keyed by column *key* (not index) so adding/removing columns in a release never scrambles
  them. Don't rely on ImGui's own `layout.ini` table settings for persistence — `DataTable` overrides them.
- Fonts are rebuilt only between frames: a page that changes the font size calls
  `app.requestFontRebuild()`; `main.cpp` invalidates the DX11 device objects and calls
  `Theme::loadFonts(dpi, settings.fontScale())` before the next `NewFrame`. Never call `loadFonts` mid-frame.
- Table editing: pages supply `isEditable` + `onRowEdited` (one callback with all changed cells of the row)
  and `onRowDoubleClicked` for the detail page. Don't add per-cell save callbacks back — the row-at-once
  commit is what makes Tab-through editing a single API request.
- Adding a new page: declare in `Pages.h`, implement in `src/pages/`, register in `App.cpp`
  navigation, and add the `.cpp` to `CMakeLists.txt`.

### Dependencies
- Don't add new third-party libraries without a strong reason. If you must, use `FetchContent` in
  `CMakeLists.txt` with a pinned tag/hash, like the existing ImGui/json entries.
- Windows-only APIs are fine; there is no cross-platform requirement.

### Verification
- There is no automated test suite yet. At minimum, verify a change compiles:
  `build.bat release` (non-interactive; builds into `build\` with the bundled MinGW) or
  `cmake --build build` (or `cmake-build-debug` from CLion).
- For UI changes, run the exe and exercise the affected page manually. Describe what you checked in
  your summary.
- Don't commit `cmake-build-*`/`build` directories or `settings.json`. **Do** commit `dist\` — the
  installer must always be present so a fresh clone can be installed without building. After any
  change to the app, run `package.bat` so `dist\` is refreshed before committing.
- Installer changes: run `package.bat nobuild`, then test with
  `setup.exe /VERYSILENT /SUPPRESSMSGBOXES /CURRENTUSER /DIR=<scratch>` and `unins000.exe /VERYSILENT`.
  Check runtime deps with `objdump -p build\RD_Desktop_Software.exe | findstr "DLL Name"` (MinGW's
  `objdump`): only system DLLs plus `libwinpthread-1.dll` (shipped) are allowed.

### Documentation
- When user-visible behaviour changes, update `README.md` (Features / Connecting sections).
- When agent-relevant knowledge changes (new rule, pitfall, workflow), update **this file**.

---

## 4. Progress log

Newest entries first. One line per task: date, what changed, status.

| Date | Change | Status |
| --- | --- | --- |
| 2026-10-09 | v1.1.0: (1) installer updates in place — fixed `AppId`, `UsePreviousPrivileges/AppDir/Tasks`, `DirExistsWarning=no`, `[Code]` reads previous `DisplayVersion`/`InstallLocation` from HKCU/HKLM, skips the folder page and lists the kept preferences on the Ready page; `package.bat`/`.iss` version 1.1.0. (2) Preferences persisted in `settings.json`: `fontSize`, `inventoryLastCount`, `tableLayouts` (order/width/visibility/sort by column key) via `DataTable::bindSettings` + `applySavedLayout` (writes `ImGuiTableSettings`, requests reload) / `captureLayout` (debounced save in `App::frame`). (3) Settings → Appearance font-size slider 11–24 px (`Theme::loadFonts(dpi, fontScale)`, atlas rebuilt between frames via `App::requestFontRebuild`). (4) Inventory table: single click edits a cell in place, Tab/Shift+Tab stage edits along the row, Enter/click-away/end of row commit one `saveRowEdits` PUT, Esc discards; double-click opens the product page (`onRowDoubleClicked`; `onCellEdited` removed). (5) Progress bar: 100 % mark falls back to the persisted previous count on the first load after start-up, fills when the load finishes early. `build.bat` quotes `-DCMAKE_MAKE_PROGRAM` (CLion under `Program Files`). | Done (verified: release build; app starts and writes the new keys; silent 1.0.0 → 1.1.0 update keeps dir/registry entry/settings, uninstall clean). Inline editing / Tab / font size / progress bar need a manual pass against live data |
| 2026-10-08 | App icon from `RDicon.png`: `tools\make_icon.ps1` pads to square and writes `installer\RDicon.ico` (16–256 px, PNG entries); `src\app.rc` + `resource.h` embed it (CMake `LANGUAGES CXX RC`), `main.cpp` sets `hIcon`/`hIconSm` so taskbar + title bar show it; `.iss` gained `SetupIconFile`, ships `RDicon.ico` and uses it as `IconFilename` for Start menu/desktop shortcuts. `dist\` installer rebuilt. | Done (verified: exe/setup icon extract OK, silent install → shortcuts point at `{app}\RDicon.ico`, uninstall clean) |
| 2026-10-08 | Installer committed + `libwinpthread-1.dll` fix: `dist/` removed from `.gitignore` so the setup exe ships with the repo (README "Installing (no build needed)"); CMake copies `libwinpthread-1.dll` from the compiler's dir next to the exe post-build and the `.iss` packages `*.dll` beside the exe. Static linking of winpthread was tried (`-static`, `-Bstatic -lpthread`, `-mcrtdll=ucrt`) and fails with CLion's bundled MinGW 15.2 — see §5. | Done (verified: installed app starts with `PATH` reduced to System32; uninstall clean) |
| 2026-10-08 | Real installer: `installer\RD_Desktop_Software.iss` (Inno Setup 6; per-user default with all-users option, Start menu + optional desktop shortcut, uninstaller that offers to delete `%APPDATA%\RD Desktop Software`, `CloseApplications`) and `package.bat` (runs `build.bat`, finds/installs ISCC via winget, `<version>`/`nobuild` args, output `dist\RD_Desktop_Software-<ver>-setup.exe`). `dist/` git-ignored; README "Creating an installer" section. | Done (verified: silent install → files + Start menu shortcut, silent uninstall → clean, settings kept) |
| 2026-10-08 | Added `build.bat`: double-clickable one-step build. Finds CLion's bundled CMake/Ninja/MinGW (`%LOCALAPPDATA%\Programs\CLion\bin`), falls back to PATH tools, then Visual Studio via `vswhere` + `VsDevCmd.bat`; reconfigures `build\` if its cache was made with another compiler; `run`/`debug`/`clean` args; pauses only when double-clicked (no args). README Building section documents it. | Done (verified: full build + no-op rebuild with bundled MinGW) |
| 2026-10-07 | Product save made the quantity climb: every PUT sent the full `in_stock`, which RepairDesk *adds*. Live-probed the exact semantics (positive = add, 0/negative = replace, absent = wiped to null) and centralised the fix in `RepairDeskApi::saveInventoryItem(id, fields, currentStock, targetStock)` (send → re-read → correct, ≤3 rounds); `toUpdatePayload` no longer emits `in_stock`; product page, inline cell edits, bulk edits and the stock take all use it and swap the re-read item in via `replaceInventoryItem`. Inventory search now filters live across every flattened field (description, notes, …), all space-separated terms must match; Enter only triggers the server keyword when the inventory load is capped. | Done (user verified quantity + search) |
| 2026-10-07 | Product editor: Category/Manufacturer/Device/Supplier/Condition/Physical location became `ui::SuggestField`s (inventory values + `Lookups` from `/devices`, `/purchaseorders`, `/inventory/additionalinfo`; free text still allowed), Valuation method (`kValuationMethods`, FIFO default) and Tax class (`/taxclasses`) became fixed `ui::ComboField`s, price pill (`Settings::suggestedPrice`, `round(cost×2 to 0.20)−0.05`, USD) with pencil → formula popup persisted in settings. Create-product 500 fixed: `addInventoryItem` always sends `tax_inclusive`; `toResult` copies the body `statusCode`. Unknown manufacturers/devices are created via `/createbrand`/`/createmodel`; unknown locations are dropped with a warning. | Done (user verified create + UI) |
| 2026-10-07 | Stock take decreases ended at the negative change (14 → scanned 1 gave −13): a negative `in_stock` replaces the stock instead of adjusting it. `applyToRepairDesk` now loops (≤4 rounds): send delta → re-read via `getInventoryItem` → send `scanned − actual` until the server reports the scanned quantity; a non-converging item is reported as failed ("ended at X instead of Y") and stays in the sidebar with its `dbQty` refreshed. | Done (awaiting user re-test against live API) |
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
- [ ] `valuation_method` is accepted by `POST /inventory/addnewitem` but never persisted (tested 1/2/3 and
      the strings FIFO/LIFO/WAC — the web UI shows "none" every time). The dropdown is kept so the payload
      is ready once RepairDesk fixes it; re-test occasionally.
- [ ] Suppliers are harvested from `/purchaseorders` (no supplier listing endpoint); a store with no POs
      gets an empty supplier list and typed suppliers are dropped with a warning.
- [x] Inventory inline/product-page edits use the per-item `saveInventoryItem` + `replaceInventoryItem`
      path; bulk actions still finish with a full `refreshInventory()` (2026-10-07; acceptable because
      many rows change at once).
- [x] Bulk "Edit quantity", inline `in_stock` edits and the product page now go through
      `saveInventoryItem`, which converts the absolute quantity into RepairDesk's adjustment. (2026-10-07)
- [x] Add a short "Troubleshooting" section to `README.md` (OAuth redirect issues, firewall prompts
      for the loopback listener on port 8731). (2026-10-07)
- [ ] Loopback listener (`http::waitForLoopbackRedirect`) accepts a single IPv4 connection and has no
      `recv` timeout; consider dual-stack (`::1`) listening, skipping favicon/preconnect requests and
      parsing `code` from a POST body if RepairDesk ever switches to `response_mode=form_post`.
- [ ] Inline table editing uses a plain text field for every editable column; suggest/combo cells
      (category, tax class) and validation of numeric input could reuse the product-page widgets.
- [ ] Only the inventory table is editable; Tickets/Invoices/Customers still open the record viewer on a
      single click (`onRowClicked`), which is intended but inconsistent with the inventory double-click.
- [ ] Non-square `RDicon.png`; a 256×256 source would remove the blur at large icon sizes.

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
- **MinGW runtime.** The exe is linked with `-static-libgcc -static-libstdc++` but still imports
  `libwinpthread-1.dll`, so CMake copies that DLL next to the exe (POST_BUILD) and the installer ships
  it. Statically linking winpthread does **not** work with CLion's bundled MinGW 15.2: its
  `libpthread.a` references `__intrinsic_setjmpex`, which only exists in the UCRT import libs, while
  the toolchain (and its libstdc++) target msvcrt — `-static`, `-Wl,-Bstatic -lpthread` and
  `-mcrtdll=ucrt` all end in link errors. Also note `target_link_options` flags land *before* the
  object files, so a `-lpthread` placed there is silently ignored; libraries belong in
  `target_link_libraries`.
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
- **`in_stock` in `PUT /inventory/{id}` is neither a value nor a clean delta.** Probed on a throw-away
  item (2026-10-07): *positive* value → added to the current stock (5 + 5 → 10, 10 + 3 → 13); *zero or
  negative* → replaces the stock (13, −2 → −2; 0 → 0); *field absent* → stock wiped to `null`. There is
  no stock-adjustment endpoint. **Always** write stock through `RepairDeskApi::saveInventoryItem`
  (going up sends the difference, going down/unchanged sends 0 then the target, then re-reads via
  `getInventoryItem` and corrects). `toUpdatePayload` deliberately omits `in_stock` so a caller cannot
  accidentally "add" the current quantity; never call `updateInventoryItem` directly with a body that
  lacks it.
- **`GET /inventory?keyword=<id>` fetches a single item.** The keyword filter matches id, name and SKU,
  so filter the result by exact id (`RepairDeskApi::getInventoryItem`). Use it to refresh individual
  rows after an update rather than re-downloading the whole inventory.
- **`POST /inventory/addnewitem` needs `tax_inclusive`.** Without it the server answers
  `{"success":false,"statusCode":500,"message":"Internal Server Error"}` *inside an HTTP 200* for any
  otherwise valid body; with `tax_inclusive: 0` the same body succeeds. `addInventoryItem` adds it.
  Bisect such 500s with a PowerShell script that adds one field at a time to a minimal body
  (`name, sku, in_stock, cost_price, price`), reading `success` from the JSON — not the HTTP status —
  and deleting each created item (`DELETE /inventory/{id}` works even though it is undocumented).
- **Write endpoints want ids, the list returns names.** `manufacturer`, `device`, `supplier`, `tax_class`
  must be integers; `physical_location` must exactly match a name from `/inventory/additionalinfo`
  (HTTP 400 otherwise); `condition` and `item_type` accept free text. Manufacturer/device lists come
  from `GET /devices` (`company`/`manufacturer_id` + `id`/`name`); there is no supplier listing, so
  `Lookups` harvests `supplier`/`vendorID` from `/purchaseorders`. `/createbrand` and `/createmodel`
  (`manufacturerId`) create missing references and answer 409 with the existing record on duplicates.
- **`ImGui::BeginCombo` cannot hold free text.** For "pick or type" fields use `ui::SuggestField`
  (InputText + a separate `ImGui::Begin` window positioned under the field, state in
  `ImGui::GetStateStorage()`); using a popup would steal focus from the text input on every keystroke.
- **A running `RD_Desktop_Software.exe` blocks linking** (`LNK1168: cannot open ... for writing`).
  Close the app before building, or link into a scratch dir with
  `-DFETCHCONTENT_SOURCE_DIR_IMGUI=<build>\_deps\imgui-src -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<build>\_deps\nlohmann_json-src`
  to reuse the downloaded deps offline. `cmake-build-debug` is configured for MSVC via CLion's bundled
  CMake, so from a plain terminal run it inside `VsDevCmd.bat -arch=x64`.
- **`cmd` variable substring replacement can't contain `=`.** `%VAR:-Dkey=value=%` stops at the first `=`,
  so `build.bat` keeps the bare compiler name in `COMPILER` and builds `-DCMAKE_CXX_COMPILER=` from it.
  When testing a batch file that ends in `pause` from a non-interactive shell, run it as
  `cmd /c 'build.bat release < nul'` so `pause` returns immediately.
- **Icon pipeline.** `windres` resolves the `ICON "..."` path in `app.rc` relative to the `.rc` file
  (`../installer/RDicon.ico`). The resource id must be `#include`d in `main.cpp` (`resource.h`) — a missing
  include is a plain compile error. The source PNG is tiny (51×53) and not square; `make_icon.ps1` pads
  it and upsamples the larger sizes, so expect some blur at 128/256 px until a bigger `RDicon.png` exists.
- **Inno Setup `/SUPPRESSMSGBOXES` does not silence `MsgBox` in `[Code]`.** A custom prompt in the
  uninstaller hung a silent uninstall; guard such prompts with `not UninstallSilent` (or
  `WizardSilent` in the installer). Inno Setup installed via winget lands in
  `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`, not `Program Files (x86)`.
- **Inno Setup updates are driven by `AppId`.** The same `AppId` + `UsePreviousAppDir`/`UsePreviousPrivileges`
  make a newer setup reuse folder and install mode and *replace* the Add/Remove entry; a changed `AppId`
  silently creates a second installation. The previous version is in
  `HKCU|HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\<AppId>_is1` (`DisplayVersion`,
  `InstallLocation`); `UsePreviousPrivileges` only works when `AppId` has no constants. Test updates with
  two installers of different versions (`package.bat 1.0.0`, `package.bat 1.1.0`) silently into a scratch
  `/DIR`, then check the registry key and that `settings.json`'s hash is unchanged.
- **ImGui table settings are per column *index*.** `ImGuiTableSettings` only stores `Index`/`DisplayOrder`,
  so a column added by `discoverColumns` or a new release shifts every saved width/order. `DataTable`
  therefore keeps its own key-based `TableLayout` in `Settings`, and before `BeginTable` writes it into
  `TableSettingsFindByID/Create` + `table->IsSettingsRequestLoad = true` (`SettingsOffset = -1`) so the
  live table reloads; `TableLoadSettings` ignores entries whose `ColumnsCount` differs, so set it to the
  real count. `RefScale` must be stored too or a font-size change does not rescale the widths.
- **Tab inside an active `InputText` triggers ImGui's nav tabbing** (focus jumps to the next widget and
  the field deactivates). To own Tab, call `ImGui::SetKeyOwner(ImGuiKey_Tab, GetItemID())` every frame
  while the item is active (`NavUpdateTabbing` tests `ImGuiKeyOwner_NoOwner`) and read the key with
  `IsKeyPressed`. The `\t` char never reaches the buffer without `AllowTabInput`.
- **Row clicks in `DataTable` are detected geometrically** (`table->RowPosY1/Y2` × `WorkRect`, clipped to
  `BgClipRect`, column from `HoveredColumnBody`, `IsWindowHovered(..AllowWhenBlockedByActiveItem)`), not via
  `IsItemHovered` on the row selectable — an active text field would otherwise swallow the hover and
  click-away/next-cell clicks could not be told apart. A double click cancels the edit the first click
  started; `IsMouseClicked && !IsMouseDoubleClicked` is the single-click test.
- **Rebuilding the font atlas needs the backend's cooperation.** Call `ImGui_ImplDX11_InvalidateDeviceObjects()`
  then `Theme::loadFonts(...)` *before* `ImGui_ImplDX11_NewFrame()`; the backend recreates the font
  texture on the next frame. Doing it inside a draw function asserts.

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
- (2026-10-07) Report both directions of a quantity change (increase *and* decrease) when testing; the
  `in_stock` fix for increases looked complete until a decrease showed asymmetric server behaviour.
- (2026-10-07) When a save "adds instead of edits", give one concrete example (field changed, quantity
  before, quantity after) — "5 became 10 after saving the description" immediately points at `in_stock`
  being echoed and saves a round of guessing.
- (2026-10-07) For "server error" bugs say up front whether the agent may create (and delete) clearly
  named test records in the live store; bisecting the `addnewitem` 500 was only possible by sending
  real requests. Also mention whether a formula/behaviour should be user-editable and where it should be
  stored (settings vs. per product) — the price pill needed a follow-up question for that.
- (2026-10-08) "Create an exe to build the software" was taken to mean a double-clickable build script
  (`build.bat`), since a compiled launcher would itself need a compiler. Say explicitly if a real `.exe`
  (e.g. a self-extracting package or an installer) is wanted instead.
- (2026-10-08) For installer requests, state whether installing a packaging tool (Inno Setup via winget)
  on the dev machine is acceptable, whether the installer should be per-user or per-machine, and which
  version number/branding (publisher, icon) to use; defaults chosen were per-user, `1.0.0`, no icon.
- (2026-10-08) For "DLL not found" reports, say where the exe was run from (installed copy, `build\`,
  another PC without CLion) — it decides between shipping the DLL and changing link flags. "The exe
  should be present in the repository" was taken to mean committing the installer in `dist\` (not the
  raw `build\` exe); say so if the bare exe should be committed as well.
- (2026-10-08) For icon requests, supply a square image of at least 256×256 px (or an `.ico`); the given
  51×53 PNG had to be padded and upscaled. Also say whether only the installer/shortcuts or also the
  running window (taskbar/title bar) should use it — here both were done.
- (2026-10-09) For multi-part requests (installer update + settings + table editing + progress bar) a
  short numbered list with the expected behaviour of each part helps; the version number to ship
  (`1.1.0` was chosen) and whether a new entry should appear in Add/Remove Programs should be stated.
  For table-editing changes say which columns must be editable, what Tab should do at the end of the row
  (here: save) and whether Esc should discard the whole row or just the current cell (here: whole row).
  For "progress bar does not fill" say whether it was the very first load after start-up (no previous
  count known) or a later refresh — that was the actual cause here.

---

## 7. Maintaining this file

At the end of every task, the agent should:
1. Add a row to **§4 Progress log** (newest first) and tick/add backlog items.
2. Add any non-obvious pitfall to **§5 Lessons learned**.
3. If the request was ambiguous or missing context, note what was needed in **§6 Prompt wishes**.
4. Update **§2 Repository map** and **§3 Conventions** if files or rules changed.

Keep entries short; this file is read at the start of every session.
