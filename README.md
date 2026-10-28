# Image Mosaic Wall

> **Maintained fork.** This is a maintained continuation of [scheckmedia/ImageMosaicWall](https://github.com/scheckmedia/ImageMosaicWall) by Tobias Scheck, which has had no activity since 2023. The tool is shipped as the Mosaic Wall plugin in digiKam's AppImage, Windows and macOS bundles.
>
> Since the fork, `master` contains fixes for the crash after changing the grid size ([KDE bug 488061](https://bugs.kde.org/show_bug.cgi?id=488061)), data races between the generation threads, empty cells, Exif orientation, tile cropping and dark edge cells. They are also offered upstream in [#25](https://github.com/scheckmedia/ImageMosaicWall/pull/25) and [#26](https://github.com/scheckmedia/ImageMosaicWall/pull/26).
>
> Bugs and ideas: [issues](https://github.com/MightyMirko/ImageMosaicWall/issues).

This application allows you to create an image based on a bunch of other images. It looks like a mosaic effect. But on detail you can check the following video.

[![](http://i3.ytimg.com/vi/FyXyUtVnVyQ/maxresdefault.jpg)](https://www.youtube.com/watch?v=FyXyUtVnVyQ "")

# Dependencies

- Qt 6.5 or later (Qt 5 still builds), with the Linguist tools for the translations.
- CMake 3.16 or later.
- Exiv2 0.27 or later.
- For the digiKam plugin: digiKam 8 or later and its development files.

On Fedora:

```sh
sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qttools-devel exiv2-devel
sudo dnf install digikam-devel   # only for the digiKam plugin
```

On Ubuntu 24.04 with Qt 5:

```sh
sudo apt install cmake g++ qtbase5-dev qttools5-dev qttools5-dev-tools libexiv2-dev
```

The Windows (MSVC, Qt from the Qt installer, Exiv2 from vcpkg) and macOS (Homebrew) builds are set up in [`.github/workflows/build.yml`](.github/workflows/build.yml).

# Building

## Standalone app

```sh
cmake -S . -B build -DBUILD_WITH_QT6=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/ImageMosaicWall
```

Use `-DBUILD_WITH_QT6=OFF` to build with Qt 5.

## digiKam plugin

`-DENABLE_DPLUGIN=ON` builds only the plugin, not the standalone app:

```sh
cmake -S . -B build -DBUILD_WITH_QT6=ON -DENABLE_DPLUGIN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

To try it without installing, start digiKam with `DK_PLUGIN_PATH`. The variable replaces digiKam's plugin folder instead of adding to it, so list the system folder as well (the path below is Fedora's; check where your distribution puts `Generic_*_Plugin.so`):

```sh
DK_PLUGIN_PATH="/usr/lib64/qt6/plugins/digikam/;$PWD/build/" digikam
```

The tool then appears in the Tools menu as "Image Mosaic Wall". To install it for good:

```sh
sudo cmake --install build
```

## Tests

```sh
cmake -S . -B build -DBUILD_WITH_QT6=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests run without a display.

# Tile cache

The analysis of the tile images is cached in a small SQLite database, so setting the same folder again only reads new or changed files. Inside digiKam it is stored in `~/.cache/digikam/imagemosaicwall/tiles.sqlite` on Linux (the standalone app uses its own cache folder). It is safe to delete; it is rebuilt on the next run.

# Settings

The options (RAW files, variation, max. uses, every photo, tint, matching, grid, tile shape, print unit and resolution) and the last used folders are remembered between sessions, in `~/.config/ImageMosaicWall/ImageMosaicWall.ini` on Linux and the matching user configuration folder on Windows and macOS. Delete the file to go back to the defaults.

# Roadmap

Planned work is organised in [milestones](https://github.com/MightyMirko/ImageMosaicWall/milestones). Bug reports and ideas are welcome in the [issues](https://github.com/MightyMirko/ImageMosaicWall/issues).
