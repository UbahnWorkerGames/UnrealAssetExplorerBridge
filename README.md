# Unreal Asset Explorer Bridge

Bridge plugin between **AssetExplorerForUnreal** and a running **Unreal Editor** session.

Explorer repository:
`https://github.com/UbahnWorkerGames/AssetExplorerForUnreal`

**Important: this bridge is intended more as a practical pattern/reference than a fully polished production product. If someone wants to implement a cleaner, more complete version, that is explicitly welcome.**

## What This Bridge Does

- Exports snapshot builds from `/Game/...` via the `aeb` console command.
- Writes one ZIP per asset with:
- `meta.json`
- preview images (`*.webp`)
- asset metadata (hashes, object path, disk file list, capture data)
- Runs a local HTTP listener in the editor for import/select operations.
- Can download snapshot ZIPs from the Explorer backend and import into the project.
- Can optionally upload exported ZIPs back to the backend (server-driven behavior).

## Requirements

- Unreal Engine `5.7.x` (as set in `AssetMetaExplorerBridge.uplugin`)
- Editor plugin module
- Reachable backend for import/upload (default: `127.0.0.1:9090`)

## Installation

1. Place this repository as a plugin in your Unreal project (for example `Plugins/AssetMetaExplorerBridge`).
2. Start the project/editor.
3. Open Project Settings:
   `Editor > Plugins > Asset Meta Explorer Bridge`
4. Verify backend URL and listen port.

## Plugin Settings

`Asset Meta Explorer Bridge`:

- `ImportBaseUrl` (default: `127.0.0.1:9090`)
- `ImportListenPort` (default: `8008`)

`ImportBaseUrl` is normalized to `http://...` when no scheme is provided.

## Console Export (`aeb`)

Registered at module startup:

```text
aeb <AssetOrFolderPath> [TypeFilter] [-i ExcludeTypes] [-exit]
```

Examples:

```text
aeb /Game/
aeb /Game/byHans1
aeb /Game/Props/SM_Box.SM_Box
aeb /Game -t "mesh,material"
aeb /Game -i "Material,MaterialInstance"
aeb /Game --exclude=material
aeb /Game --type=staticmesh --exit
```

Supported include/exclude tokens:

- `animation`, `anim`, `animsequence`
- `mesh`, `staticmesh`, `skeletalmesh`
- `material`, `materialinstance`
- `blueprint`, `bp`
- `niagara`

Exportable classes:

- `StaticMesh`
- `SkeletalMesh`
- `Blueprint`
- `NiagaraSystem`
- `AnimSequence`
- `Material`
- `MaterialInstance`
- `MaterialInstanceConstant`

## Export Output Layout

Default root:

```text
<ProjectRoot>/export
```

Per-asset file:

```text
<ProjectRoot>/export/<TopFolderOrPack>/<hash_main_blake3>.zip
```

Notes:

- For paths like `/Game/byHans1/<Pack>/...`, export subfolder becomes `<Pack>`.
- `Texture2D` assets are intentionally skipped.

## Vendor and /Game Path Convention (`byHans1`)

The backend/project mapping currently relies on the first path segment after `/Game/`.

For example:

```text
/Game/byHans1/MyPack/SM_Crate.SM_Crate
```

Behavior in this bridge:

- `vendor` in `meta.json` = `byHans1` (first segment after `/Game/`)
- `source_folder` in `meta.json` = `byHans1`
- Project resolution for upload uses:
  `<Project>/Content/byHans1` via `/projects/resolve?source_path=...&auto_create=1`
- Export ZIP output subfolder is special-cased to use `MyPack` (second segment) when vendor is `byHans1`

So yes: for the backend flow, `byHans1` is treated as the vendor namespace directly under `/Game/`, and that namespace is used for project association.

## Export ZIP Contents

ZIPs are written as store (no compression) and contain at least:

- `meta.json`
- `0.webp` (or multiple frames: `0.webp`, `1.webp`, ...)

Important `meta.json` fields:

- `hash_main_blake3`
- `hash_main_sha256`
- `hash_full_blake3`
- `package`
- `vendor`
- `source_path`
- `source_folder`
- `object_path`
- `class`
- `files_on_disk`
- `disk_bytes_total`
- `preview_files`
- `no_pic`
- `low_quality`
- `capture_resolution`
- `capture_fov`
- `capture_distance`
- animation-only: `frame_count`, `frames[]`, `animation_length_seconds`

## HTTP Endpoints (Editor Listener)

Listener port: `ImportListenPort` (default `8008`).

### `GET /asset-import?id=<snapshotId>&mode=<override|skip>`

- Downloads `/download/{id}.zip` from backend
- Imports into `<Project>/Content`
- Returns:
- `{"ok": true}` on success
- `{"ok": false, "error": "..."}`

`mode`:

- `override` (default): overwrite existing files
- `skip`: keep existing files

### `OPTIONS /asset-import`

CORS preflight handler.

### `GET /asset-select?path=<ObjectPath>`

Finds and selects an asset in Content Browser (`SyncBrowserToAssets`).

Example:

```text
/asset-select?path=/Game/Props/SM_Box.SM_Box
```

## Import Details (ZIP -> Content)

Only these file types are written:

- `.uasset`
- `.uexp`
- `.ubulk`
- `.uptnl`
- `.umap`

Safety checks:

- no unsafe/absolute paths
- no `..` or `.` traversal segments
- optional skip for existing files (`SkipExisting`)
- synchronous AssetRegistry scan for imported `.uasset`/`.umap`

## Backend Integration

The bridge can read backend settings from `/settings`, including:

- export include/exclude (`export_include_types`, `export_exclude_types`)
- skip export if hash already exists (`skip_export_if_on_server`)
- upload-after-export (`export_upload_after_export`)
- upload/check path templates
- per-type image/capture counts

Upload flow:

1. Resolve project id with `/projects/resolve?source_path=...&auto_create=1`
2. Upload ZIP (multipart) to `/assets/upload` (or configured path)
3. Send progress event to `/events/notify`

## Blueprint/C++ API

`UAssetSnapshotBPLibrary`:

- `GetDefaultExportRoot()`
- `ExportPathBuilds(...)`
- `ExportAssetBuild(...)`
- `ImportSnapshotZip(...)`
- `DownloadAndImportSnapshot(...)`
- `DownloadAndImportSnapshotNative(...)`

Import modes (`EAssetSnapshotImportMode`):

- `OverrideExisting`
- `SkipExisting`

## License

See `LICENSE`.
