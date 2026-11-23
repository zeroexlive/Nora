# obj (vendored)

- Upstream: https://github.com/schmarta/obj, "Insanely fast obj importer written from scratch in ANSI C"
- License: MIT, Copyright (c) 2024 Lucas Marta (see `LICENSE` in this directory)
- Vendored from upstream `master` (files `obj.h`, `obj.c`; `main.c` and the upstream CMake file are not used)

## Why vendored rather than FetchContent

Upstream is two files with no tags. Its loader only accepted triangles written as `f v/vt/vn`, read past the
end of its index buffer, left `name` uninitialised when a file had no `o` line, included the non-portable
`<malloc.h>` (fails on macOS) and ignored materials. Nora needs fixes in the code itself, so the files are
copied here under their MIT license and changed in place.

## Local changes

- `<stdlib.h>` instead of `<malloc.h>`; line buffer 4096 bytes instead of 256.
- Faces with any number of corners are fan-triangulated; every index form (`v`, `v/vt`, `v//vn`,
  `v/vt/vn`) and negative (relative) indices are accepted.
- `usemtl`, `mtllib`, `o` and `g` are recorded: `face_materials`, `face_groups`, `material_names`,
  `group_names`, `mtllibs`.
- `vt` lines with a single coordinate are accepted.
- Fixed: uninitialised `name`, the out-of-bounds write when building `indices`, memory leaks on parse
  errors, and `obj_free` not freeing `faces`, `complete_vertices` and `indices`.

MTL parsing lives in Nora (`src/core/obj_import.c`), which maps MTL statements to Nora materials.
