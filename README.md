# Image Mosaic Wall

> **Maintained fork.** This is a maintained continuation of [scheckmedia/ImageMosaicWall](https://github.com/scheckmedia/ImageMosaicWall) by Tobias Scheck, which has had no activity since 2023. The tool is shipped as the Mosaic Wall plugin in digiKam's AppImage, Windows and macOS bundles.
>
> Since the fork, `master` contains fixes for the crash after changing the grid size ([KDE bug 488061](https://bugs.kde.org/show_bug.cgi?id=488061)), data races between the generation threads, empty cells, Exif orientation, tile cropping and dark edge cells. They are also offered upstream in [#25](https://github.com/scheckmedia/ImageMosaicWall/pull/25) and [#26](https://github.com/scheckmedia/ImageMosaicWall/pull/26).
>
> Bugs and ideas: [issues](https://github.com/MightyMirko/ImageMosaicWall/issues).

This application allows you to create an image based on a bunch of other images. It looks like a mosaic effect. But on detail you can check the following video.

[![](http://i3.ytimg.com/vi/FyXyUtVnVyQ/maxresdefault.jpg)](https://www.youtube.com/watch?v=FyXyUtVnVyQ "")

# Dependencies

- Qt5 or later.
- CMake 3.x.
- Exiv2 0.27.x or later.
- digiKam 8.x plugin interface. <https://www.digikam.org> (optional)

# Compiling and Installing

- mdkir build
- cd build
- cmake . -DCMAKE_INSTALL_PREFIX=/usr ..                            (stand alone version only)
- cmake . -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_DPLUGIN ..           (stand alone version and digiKam plugin)
- make
- make install/fast

## TODO's

- [x] Drag & Drop for a single image
- [x] a better history algorithm (something like a kernel which checks the neighborhood in (x - n) and (y - n) direction to be unique)
- [x] floating point precision bug - there is something wrong with some grid resolutions and the output image. It contains blank pixels at the and of the image. But otherwise if I use floating point numbers there are gaps between the mosaic images)
- [x] TravisCI configuration for building this app for OS X, Linux ~~and maybe Windows~~?
- [x] loading indicator for "Load Base Image" & "Set Image Folder"
- [x] extract Thumbnails from Exif instead down sampling the whole image
