# PipeWireAO Aravis plugin

This repository provides the experimental `api.aravis.source` camera plugin.
It supports Aravis GenTL and native GigE Vision or USB3 Vision transports, with
complete-frame and selected row-block output modes.

The repository is an independent build unit. It depends on PipeWireAO and the
public headers installed by
[`pipewireao-spa-plugins-core`](https://github.com/DarrylGamroth/pipewireao-spa-plugins-core).
Meson uses an installed Aravis 0.10 library when available and can otherwise
build the pinned fallback in `subprojects/aravis.wrap`.

## Build and stage

```console
meson setup build --prefix=/usr -Daravis=enabled
meson compile -C build
meson test -C build 'spa-aravis*' --print-errorlogs
DESTDIR="$PWD/stage" meson install -C build
```

Use `--wrap-mode=forcefallback` during setup to require the pinned Aravis fork.
See [`spa/plugins/aravis/README.md`](spa/plugins/aravis/README.md) for transport
selection, fake-camera isolation, row-block constraints, and qualification
status.

## Container and package recipes

The Docker Bake file offers `debian-13-deploy` and `ubuntu-26-04-deploy` image
targets. `debian-13-package` and `ubuntu-26-04-package` export `.deb` files when
that deployment format is wanted. The supplied image recipes use the package
artifact internally; Meson builds are not restricted to Debian or Ubuntu.

The container build must be given authorized sources for PipeWireAO and the
core development files through its build arguments or BuildKit secrets.
