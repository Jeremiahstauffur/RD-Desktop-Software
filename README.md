# RepairDesk Desktop

A Windows 11–styled desktop client for [RepairDesk](https://www.repairdesk.co/) built with C++20,
Win32 + DirectX 11 and Dear ImGui. No external SDKs are required: Dear ImGui and nlohmann/json are
downloaded automatically by CMake, and networking uses the built-in WinHTTP library.

## Building

Open the folder in CLion (bundled MinGW + Ninja work out of the box) and build the
`RD_Desktop_Software` target, or from a terminal:

```
cmake -S . -B build -G Ninja
cmake --build build
build\RD_Desktop_Software.exe
```

The first configure downloads Dear ImGui and nlohmann/json, so an internet connection is needed once.

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
   redirect URL and paste it into **Or paste an authorization code**.

Settings (including tokens) are stored in `%APPDATA%\RD Desktop Software\settings.json`.

## Features

- **Inventory** – list view with row dividers only, selectable columns (gear icon at the right of the
  header shows a checkbox list of every field RepairDesk returns), drag-to-reorder and drag-to-resize
  columns, sortable headers, multi-select checkboxes, bulk actions (delete, edit cost/price/quantity/
  category), click-to-edit cells, and a product page (click a row) with name, quantity, cost, price,
  SKU, UPC and description at the top.
- **Stock take** – right-hand sidebar; scan or type a code and it is matched by SKU → UPC → name.
  Unknown codes play an error sound and show a red "No item found" line. Each scanned item shows the
  database quantity, the change (red/green) and the scanned count (accent colour). Nothing is written
  to RepairDesk until **Submit** is pressed.
- **POS** – product grid with barcode entry, cart and invoice creation.
- **Tickets / Invoices / Customers** – paged, searchable lists; click a row to view every field.
- **Communication** – SMS inbox and send-SMS (via ticket) form.
- **Global search** – the search bar at the bottom of every screen searches products, invoices,
  tickets and customers across all fields and labels.
- **Settings** – API key / OAuth2 credentials, dark/light theme, accent colour (presets and a full
  colour picker) applied to header text, buttons, lines and fills.
