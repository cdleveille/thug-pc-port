# Vendored third-party code

Both projects are committed here as ordinary files, not git submodules, so a
plain clone builds. Each keeps its own licence.

| Directory | Project | Upstream base | Licence |
|---|---|---|---|
| `xboxrecomp/` | [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): the static recompilation toolkit (lifter, kernel shim, NV2A/APU runtime) | v0.17.0, commit `343b6d8901723b37ce568b2418c5df4e1fe1d223` | MIT; parts from xemu are LGPL-2.1-or-later (see `xboxrecomp/NOTICE`, `xboxrecomp/LICENSES/`) |
| `imgui/` | [Dear ImGui](https://github.com/ocornut/imgui): the F1 settings overlay | v1.91.9b, commit `f5befd2d29e66809cd1110a152e375a7f1981f06` | MIT (`imgui/LICENSE.txt`) |

## xboxrecomp

The whole upstream tree, with this port's changes made in place: fixes and
features in the runtime (POSIX support, the GPU back-end interface, the
pushbuffer walk, MSAA-era render state, volume and rumble controls, ...) and
the tests that cover them. Each change is listed in `docs/NOTES.md`
("Toolkit changes").

The lifting pipeline writes its output under `xboxrecomp/tools/*/output/`.
That output is derived from the game's executable, and the toolkit's own
`.gitignore` keeps it out of the repository. Keep it that way.

To see everything this port changed, or to offer a fix upstream, compare
against the base commit:

```
git clone https://github.com/sp00nznet/xboxrecomp.git /tmp/xboxrecomp
git -C /tmp/xboxrecomp checkout 343b6d8901723b37ce568b2418c5df4e1fe1d223
diff -ruN -x .git -x __pycache__ -x output /tmp/xboxrecomp third_party/xboxrecomp
```

To move to a newer upstream release, apply that diff to the new release
rather than replacing the directory, and update the base commit above.

## Dear ImGui

Only what the build compiles: the core (`imgui*.cpp/h`, `imconfig.h`,
`imstb_*.h`), the OpenGL 3 backend (`backends/imgui_impl_opengl3*`), and the
licence. Unmodified. To update, copy the same files from a newer release and
change the version above.
