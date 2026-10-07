# magic.wasm

libmagic, the engine of `file`, as one pure WebAssembly module with its
compiled magic database inside: the MIME type of bytes in memory. Built
by [ix](https://github.com/pg83/ix) as `lib/magic/wasm` for the
`wasm32-none` target, from file 5.48 with its compression libraries and
its ELF reader off (sha256 of the module
cc690994ab91d51dc0724bed1a6276f9189a1f7fe197c9581212609377b9c8ff). The
build behind it proves the module imports nothing and runs it on files
of known types, twice each in one instance, requiring the type of each.

The module exports its `memory`, `malloc`, `free` and

```
magic_mime(data, len) -> type | 0
magic_mime_error() -> cause | 0
```

taking the bytes in the module's memory. The type is a C string in the
module's memory, the library's own, valid until the next call, as
`file --mime-type` names it: empty bytes are `application/x-empty`,
bytes no entry matches `application/octet-stream`, and bytes that look
like text `text/plain`. The database is loaded on an instance's first
call. 0 is a failure the library noticed itself, which
`magic_mime_error()` names; a broken file may trap the module, and the
instance is then thrown away. The stack is 8 MB.
