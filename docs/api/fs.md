---
title: fs
description: Filesystem I/O with sandboxed paths and streaming reads
---

# fs

```ts twoslash
import {
  readFile,
  writeFile,
  readStream,
  stat,
  readDir,
  mkdir,
  rmdir,
  rename,
  unlink,
  exists,
} from 'mikro/fs'
```

File I/O against the on-device LittleFS partition. Paths resolve against the app's filesystem root; `..` segments are normalized and cannot escape it.

## The `/app` read-only zone

Bundled code, assets, and `mikro.config.json` live under `/app`. Writes to any path under `/app` fail with `FSError.AccessDenied`. User data goes anywhere else: `/data/`, `/logs/`, or the root.

## Reading

### readFile(path)

Read a whole file as a `Uint8Array`.

```ts
function readFile(path: string): Result<Uint8Array, FSError>
function readFile(path: string, encoding: 'utf-8'): Result<string, FSError>
```

Passing `'utf-8'` returns a decoded string instead.

```ts twoslash
import {readFile} from 'mikro/fs'
// ---cut---
const data = readFile('/app/static/logo.pbm').orPanic('missing logo')
const config = readFile('/data/config.json', 'utf-8').orPanic('missing config')
```

Files larger than `fsReadMax` (default 64 KiB) fail with `FSError.TooLarge`. Use [`readStream`](#readstream-path-options) for bigger files.

### readStream(path, options?)

Read a file as a stream of byte chunks.

```ts
function readStream(
  path: string,
  options?: {chunkSize?: number},
): Result<AsyncIterable<Result<Uint8Array, FSError>>, FSError>
```

The outer `Result` wraps the initial `open`. Once iterating, each yielded item is itself a `Result<Uint8Array, FSError>`: ok-wrapped chunks on success, a single terminal err item if a mid-stream read fails. The underlying handle closes on EOF, error, or early consumer break.

Composes with `mikro/stream` for text and line processing:

```ts twoslash
import {readStream} from 'mikro/fs'
import {decodeUtf8, splitLines} from 'mikro/stream'
// ---cut---
const stream = readStream('/data/events.log').orPanic('log missing')
for await (const line of splitLines(decodeUtf8(stream))) {
  if (!line.ok) {
    console.error('read failed:', line.error)
    break
  }
  console.log(line.value)
}
```

Default `chunkSize` is 512 bytes.

## Writing

### writeFile(path, contents, options?)

Write a string or `Uint8Array` to a file.

```ts
function writeFile(
  path: string,
  contents: string | Uint8Array,
  options?: {create?: boolean; append?: boolean},
): Result<void, FSError>
```

**Options:**

- `create` (default `true`): when `false`, fail with `NotFound` if the file doesn't exist
- `append` (default `false`): when `true`, append instead of truncating

```ts twoslash
import {writeFile} from 'mikro/fs'
// ---cut---
writeFile('/data/config.json', JSON.stringify({count: 1})).orPanic('write')
writeFile('/data/events.log', `${Date.now()} boot\n`, {append: true})
```

## Metadata

### stat(path)

Read file metadata.

```ts
function stat(path: string): Result<StatResult, FSError>

interface StatResult {
  size: number
  isFile: boolean
  isDirectory: boolean
  mtime?: number // ms since epoch; absent when unsupported
}
```

```ts twoslash
import {stat} from 'mikro/fs'
// ---cut---
const r = stat('/data/config.json')
if (r.ok) console.log(`${r.value.size} bytes`)
```

### exists(path)

Test whether a path exists. Returns a plain boolean; unreachable paths return `false`.

```ts
function exists(path: string): boolean
```

```ts twoslash
import {exists} from 'mikro/fs'
// ---cut---
if (exists('/data/config.json')) {
  // ...
}
```

## Directories

### readDir(path)

List a directory. Each entry carries its type.

```ts
function readDir(path: string): Result<DirEntry[], FSError>

interface DirEntry {
  name: string
  isFile: boolean
  isDirectory: boolean
}
```

```ts twoslash
import {readDir} from 'mikro/fs'
// ---cut---
const r = readDir('/data')
if (r.ok) {
  for (const entry of r.value) {
    if (entry.isFile) console.log(entry.name)
  }
}
```

### mkdir(path, options?)

Create a directory.

```ts
function mkdir(path: string, options?: {recursive?: boolean}): Result<void, FSError>
```

`recursive: true` creates intermediate directories as needed.

```ts twoslash
import {mkdir} from 'mikro/fs'
// ---cut---
mkdir('/data/cache/images', {recursive: true}).orPanic('mkdir')
```

### rmdir(path)

Remove an empty directory. For non-empty directories, walk and unlink entries first.

```ts
function rmdir(path: string): Result<void, FSError>
```

## File operations

### rename(from, to)

Move or rename a file.

```ts
function rename(from: string, to: string): Result<void, FSError>
```

### unlink(path)

Delete a file.

```ts
function unlink(path: string): Result<void, FSError>
```

## FSError

All fs errors share the `FSError` discriminated union. Discriminate on `.name`.

| Variant             | Fields                 | Meaning                                  |
| ------------------- | ---------------------- | ---------------------------------------- |
| `NotFound`          | `path`                 | File or directory doesn't exist          |
| `AlreadyExists`     | `path`                 | Target already exists                    |
| `AccessDenied`      | `path`                 | Write under `/app`, or permission denied |
| `NoSpace`           | `path`                 | Filesystem full                          |
| `TooLarge`          | `path, limit?`         | `readFile` exceeded `fsReadMax`          |
| `IsDirectory`       | `path`                 | File operation on a directory            |
| `NotDirectory`      | `path`                 | Directory operation on a file            |
| `BadFileDescriptor` | `{}`                   | Operation on a closed handle             |
| `Unknown`           | `code, errno, message` | Anything not covered above               |

```ts twoslash
import {writeFile} from 'mikro/fs'
// ---cut---
const r = writeFile('/data/log.txt', 'hello')
if (!r.ok) {
  switch (r.error.name) {
    case 'NoSpace':
      console.error('disk full')
      break
    case 'AccessDenied':
      console.error('readonly:', r.error.path)
      break
    default:
      console.error('write failed:', r.error)
  }
}
```

## Configuration

### fsReadMax

Maximum size in bytes for a single `readFile` call. Files larger than this fail with `FSError.TooLarge`; use `readStream` for bigger files. Default 65536 (64 KiB).

```ts
// mikro.config.ts
import {defineConfig} from 'mikro'

export default defineConfig({
  fsReadMax: '128k',
})
```

Accepts a number (bytes) or a string with `K`/`M`/`G` suffix. Normalized to a plain number at build time.
