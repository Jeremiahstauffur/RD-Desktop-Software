# RepairDesk Desktop

A Windows 11–styled desktop client for [RepairDesk](https://www.repairdesk.co/) built with C++20,
Win32 + DirectX 11 and Dear ImGui. No external SDKs are required: Dear ImGui and nlohmann/json are
downloaded automatically by CMake, and networking uses the built-in WinHTTP library.

## Installing (no build needed)

A ready-made installer ships with the repository: run `dist\RD_Desktop_Software-1.1.0-setup.exe`. It
installs per user by default (no administrator prompt; "all users" can be chosen), adds a Start menu
entry, an optional desktop shortcut (both with the RepairDesk icon, which the app also shows in the
taskbar and title bar) and an uninstaller (Settings → Apps). The app needs no other software: the MinGW
runtime (`libwinpthread-1.dll`) is included next to the exe. Uninstalling asks
whether to also delete `%APPDATA%\RD Desktop Software` (your saved credentials). Silent install:
`RD_Desktop_Software-1.1.0-setup.exe /VERYSILENT /CURRENTUSER`.

**Updating:** download the new installer and run it. It detects an existing installation (per-user or
all-users), updates it in place in the same folder with the same install mode and shortcut choices
(the folder page is skipped and the summary page shows *Existing installation found: 1.0.0 → 1.1.0*), and
replaces the Add/Remove Programs entry instead of adding a second one. Your preferences are kept: they
live in `%APPDATA%\RD Desktop Software\settings.json`, which Setup never touches – accent colour,
theme, font size, the column order/widths/visibility of every table, the API key / OAuth credentials
and tokens. Close the app first (Setup asks if it is running).

Building from source is only needed to change the code; after a change, run `package.bat` to refresh
the installer in `dist\` and commit it together with the source.

## Building

**One click:** double-click `build.bat` in the project folder. It finds CLion's bundled CMake, Ninja
and MinGW automatically (falling back to tools on `PATH` or an installed Visual Studio), builds a
Release configuration into `build\` (exe plus `libwinpthread-1.dll`) and prints the path of
`RD_Desktop_Software.exe`. From a terminal:

```
build.bat          Release build
build.bat run      build, then start the app
build.bat debug    Debug build
build.bat clean    wipe build\ and rebuild from scratch
```

Alternatively, open the folder in CLion (bundled MinGW + Ninja work out of the box) and build the
`RD_Desktop_Software` target, or from a terminal:

```
cmake -S . -B build -G Ninja
cmake --build build
build\RD_Desktop_Software.exe
```

The first configure downloads Dear ImGui and nlohmann/json, so an internet connection is needed once.

## Rebuilding the installer

Double-click `package.bat` (or run `package.bat 1.2.0` to set the version). It runs `build.bat`, then
compiles `installer\RD_Desktop_Software.iss` with [Inno Setup 6](https://jrsoftware.org/isinfo.php)
into `dist\RD_Desktop_Software-<version>-setup.exe` (the exe, `libwinpthread-1.dll`, `RDicon.ico` and this
README). The icon comes from `RDicon.png` in the project root; after replacing that file run
`powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1` to regenerate `installer\RDicon.ico`, which
is compiled into the exe (`src\app.rc`) and used by the setup wizard and the shortcuts.
If Inno Setup is not installed the script installs it for the current user via
`winget install JRSoftware.InnoSetup`. `package.bat nobuild` skips the build and packages the existing
`build\RD_Desktop_Software.exe`. The `dist\` folder is intentionally **not** git-ignored so the
installer is always available to anyone who clones the repository.

## Connecting to RepairDesk

Open **Settings** (bottom of the navigation rail). Two authentication methods are supported, matching
the [RepairDesk Public API](https://api-docs.repairdesk.co/):

| Method | What to enter | How requests are authenticated |
| --- | --- | --- |
| **OAuth 2.0** (recommended) | Client ID, Client Secret, Redirect URI | `Authorization: Bearer <access_token>` header. Tokens are refreshed automatically (access token lifetime 1 h, refresh token 1 month). |
| **API key** (legacy) | API key | `api_key` query parameter. Used only when no OAuth token is present. |

### OAuth 2.0 flow

1. Create an OAuth application in RepairDesk and register the redirect URI shown in Settings
   (default `http://localhost:8731/callback`).
2. Enter the Client ID and Client Secret, then press **Sign in with RepairDesk**.
3. Your browser opens RepairDesk's consent page. After you approve, RepairDesk redirects back to the
   loopback URI; the app captures the authorization code and exchanges it at `/oauth2/token`.
4. If your registered redirect URI is not a localhost address, copy the `code` parameter from the
   redirect URL (or paste the whole redirect URL) into **Or paste an authorization code**.

Settings (including tokens) are stored in `%APPDATA%\RD Desktop Software\settings.json`.

### Troubleshooting OAuth

The browser page that says *Authorization received* only confirms that the authorization code reached
the app; the token exchange happens afterwards and its outcome is shown in the info bar under
**Sign in with RepairDesk**.

| Message in Settings | What it means / what to do |
| --- | --- |
| `Token exchange failed: invalid_client: ...` | Client ID or Client Secret is wrong, or belongs to a different OAuth app. |
| `Token exchange failed: invalid_grant` / `invalid_request` / redirect-URI errors | The redirect URI registered in RepairDesk must match the one in Settings **character for character** (default `http://localhost:8731/callback`, no trailing slash). Codes are single-use and short-lived, so press **Sign in with RepairDesk** again after fixing the app registration. |
| `Sign-in failed: Could not listen on 127.0.0.1:8731` | Another program uses the port. Change the port in the redirect URI (and in the RepairDesk app registration). |
| `Sign-in failed: Timed out waiting for the browser` | The consent page was not approved within 5 minutes, or the browser could not reach `localhost`. Approve again, or use the paste-a-code field. |
| Windows Firewall prompt | The app opens a loopback-only listener on port 8731 for the redirect; allowing it is safe. |

RepairDesk's OAuth endpoints answer `HTTP 200` even on failure and wrap the result in
`{"success", "statusCode", "message", "data"}`; the app reads the real `error_description` from `data`.

## Features

- **Inventory** – list view with row dividers only, selectable columns (gear icon at the right of the
  header shows a checkbox list of every field RepairDesk returns), drag-to-reorder and drag-to-resize
  columns, sortable headers, multi-select checkboxes, bulk actions (delete, edit cost/price/quantity/
  category), in-table editing and a product page with name, quantity, cost, price, SKU, UPC and
  description at the top. **Single-click** a value (name, SKU, UPC, category, quantity, prices, …) to edit
  it right in the table; **Tab** / **Shift+Tab** move to the next / previous editable column of the same
  row (changed cells are shown in the accent colour until saved), **Enter**, tabbing past the last column
  or clicking anywhere else saves the whole row to RepairDesk in one request, **Esc** discards it.
  **Double-click** a row to open the product page. Column order, widths and which columns are shown are
  remembered per table across restarts and updates. While the inventory refreshes, a thin accent-coloured
  progress bar runs along the top edge of the window: it fills from left to right as items arrive, reaching
  the right edge when as many items as the previous load (remembered between sessions) have been
  received; any additional items extend it as a dotted segment on the right, in proportion to the previous
  total. The search box filters live across *every* field of every loaded
  product (description, notes, location, tax class…); multiple words must all match. Saving a product
  (page, inline cell or bulk edit) leaves the quantity exactly as shown – the app compensates for
  RepairDesk's API, which otherwise adds the sent quantity to the stock.
- **Product editor** – Category, Manufacturer, Device, Supplier, Condition and Physical location are
  drop-downs filled with the values already used in your inventory (plus RepairDesk's device, supplier and
  location lists); you can still type a new value. New manufacturers/devices are created in RepairDesk on
  save; a physical location that RepairDesk does not know is left out with a warning (add it under Store
  settings first). Valuation method (FIFO default, LIFO, WAC) and Tax class are fixed drop-downs loaded
  from your account. Next to the Price box a pill shows a suggested price
  (`round(cost × 2 to the nearest 0.20) − 0.05`, shown in USD) – click it to copy the amount into Price;
  the small pencil beside it lets you change the multiplier, rounding step and subtraction (saved in
  settings).
- **Stock take** – right-hand sidebar; scan or type a code and it is matched by SKU → UPC → name.
  Unknown codes play an error sound and show a red "No item found" line. Each scanned item shows the
  database quantity, the change (red/green) and the scanned count (accent colour). Nothing is written
  to RepairDesk until **Submit** is pressed. Submit sends the *change* (scanned − database quantity)
  as a stock adjustment, re-reads the item and, if RepairDesk reports a different quantity (it handles
  decreases differently from increases), sends the remaining difference again until the item is at
  exactly the scanned count; items whose change is 0 are not sent. After submitting, only the updated
  items are re-read from RepairDesk and refreshed in the inventory list (the rest of the list is left
  untouched); items that failed to update stay in the sidebar so they can be re-submitted.
- **POS** – product grid with barcode entry, cart and invoice creation.
- **Tickets / Invoices / Customers** – paged, searchable lists; click a row to view every field.
- **Communication** – SMS inbox and send-SMS (via ticket) form.
- **Global search** – the search bar at the bottom of every screen searches products, invoices,
  tickets and customers across all fields and labels.
- **Settings** – API key / OAuth2 credentials, dark/light theme, accent colour (presets and a full
  colour picker) applied to header text, buttons, lines and fills, and the default font size (11–24 px,
  default 15; headers, icons and table column widths scale with it; applied immediately).
