set shell := ["zsh", "-cu"]
build: 
    # digiKam plugin (build/ uses Makefiles, Qt 6)
    cmake --build build --target Generic_ImageMosaicWall_Plugin -j$(nproc)

test:
    # tests (build-tests/ uses Ninja)
    cmake --build build-tests
    cd build-tests && ctest --output-on-failure

start:
    IMAGEMOSAICWALL_DEBUG_SESSION=1 DK_PLUGIN_PATH="/usr/lib64/qt6/plugins/digikam/;$HOME/codespace/50_Foss/ImageMosaicWall/build/" digikam

