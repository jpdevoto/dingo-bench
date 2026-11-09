# DingoBench workspace design

DingoBench uses a native Qt Widgets workspace. The approved DINGO identity appears in the navigation rail, window icon, and About dialog. The interface is built for bench testing and diagnostics.

## Visual tokens

| Role | Value | Use |
| --- | --- | --- |
| Canvas | `#141d28` | Scrollable workspaces |
| Rail and table base | `#101822` | Navigation, tables, status bar |
| Raised surface | `#182432` | Toolbar, forms, diagnostic console |
| Borders | `#2c3c4d` to `#617f99` | Separators and control boundaries |
| Text | `#ecf2f8` | Labels, input, data |
| Secondary text | `#acbed0` | Descriptions and contextual help |
| Navigation selection | `#ffbd8d` on `#342b29`, border `#775640` | Selected navigation |
| Table selection and focus | `#314b63`, `#ffa86c` | Selected rows and keyboard focus |
| Success | `#86dfaf` | Successful operations |
| Warning | `#ffc582` | Timeout and caution |
| Error | `#ff9ba5` | Rejection and failure |
| In progress | `#a9d3ff` | Active operation |

Use the system UI sans-serif at the platform size for controls and data. Workspace headings use 27 logical px, toolbar titles 16 px, and section headings 15 px. Use a 168 px navigation rail, 18 px page gutters, 10 px between work areas, and 8 px between related controls. Keep corners small and use a single border to separate forms from the canvas. Tables use alternating rows, compact headers, and horizontal scrolling for identifiers.

Buttons and fields have 4 px corners, form sections 5 px, and progress bars 3 px. The primary action uses a brighter blue surface. Disabled controls use muted text and icons. Keyboard focus uses the orange outline.

## Workspace conventions

- Keep the active tool, and endpoint profiles visible across all workspaces. Orange denotes navigation selection and keyboard focus. A visible label accompanies each operation state.
- Use an input section, a short action row, and a result or inspection surface. Use the same connection selectors throughout the app. Endpoint edits affect the session selectors immediately.
- Use Material Symbols Outlined for interface actions. `ui::Symbol` maps semantic identifiers to the local SVG resources. The icon engine renders at the requested density, caches the result, and applies selected, disabled, or semantic status tints. The approved DingoBench logo is separate from this icon set.
- Let users select and copy read-only values. Keep long protocol or association details behind a disclosure control. Use an explicit message for empty results and preserve row selection affordances.
- Let the diagnostic console resize independently. On first launch, open it in taller windows and leave more room for the tool in shorter windows. Search, severity filters, and follow mode operate on the shared session log. Closing the console leaves the main log workspace available.
- Show operation progress, results, and cancellation in the relevant tool. The Dashboard summarizes completed DICOM operations retained in the current session.
- Save window geometry, splitter sizes, console visibility, and the last workspace. Connection profiles and active selections persist locally.

The palette and widget rules live in `src/ui/workspace.qss`.
