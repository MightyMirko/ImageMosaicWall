#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include "progressbutton.h"
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

class TestMainWindow : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configDir;

    template <typename T> static T *child(MainWindow &window, const char *name)
    {
        T *widget = window.findChild<T *>(name);
        if (!widget)
            qFatal("widget %s not found", name);
        return widget;
    }

    QString settingsPath() const
    {
        return m_configDir.filePath("ImageMosaicWall/ImageMosaicWall.ini");
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configDir.isValid());

        // Keeps the settings out of the user's real configuration directory.
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_configDir.path());
    }

    void init()
    {
        QFile::remove(settingsPath());
    }

    // Issue #16: the options are the same the next time the window is opened.
    void settingsAreRemembered()
    {
        {
            MainWindow window;
            child<QComboBox>(window, "cbRawFiles")->setCurrentIndex(2);
            child<QSlider>(window, "slHistory")->setValue(3);
            child<QSpinBox>(window, "sbMaxUses")->setValue(4);
            child<QCheckBox>(window, "cbEveryPhoto")->setChecked(true);
            child<QSpinBox>(window, "sbTint")->setValue(20);
            child<QComboBox>(window, "cbMatching")->setCurrentIndex(1);
            child<QSpinBox>(window, "sbRows")->setValue(150);
            child<QSpinBox>(window, "sbCols")->setValue(80);
            child<QComboBox>(window, "cbTileShape")->setCurrentIndex(6);
            child<QComboBox>(window, "cbPrintUnit")->setCurrentIndex(1);
            child<QSpinBox>(window, "sbDpi")->setValue(150);
            child<QSpinBox>(window, "sbPartColumns")->setValue(3);
            child<QSpinBox>(window, "sbPartRows")->setValue(2);
            child<QDoubleSpinBox>(window, "sbPartOverlap")->setValue(0.5);
        }

        MainWindow window;
        QCOMPARE(child<QComboBox>(window, "cbRawFiles")->currentIndex(), 2);
        QCOMPARE(child<QSlider>(window, "slHistory")->value(), 3);
        QCOMPARE(child<QSpinBox>(window, "sbMaxUses")->value(), 4);
        QVERIFY(child<QCheckBox>(window, "cbEveryPhoto")->isChecked());
        QCOMPARE(child<QSpinBox>(window, "sbTint")->value(), 20);
        QCOMPARE(child<QComboBox>(window, "cbMatching")->currentIndex(), 1);
        QCOMPARE(child<QSpinBox>(window, "sbRows")->value(), 150);
        QCOMPARE(child<QSpinBox>(window, "sbCols")->value(), 80);
        QCOMPARE(child<QComboBox>(window, "cbTileShape")->currentIndex(), 6);
        QCOMPARE(child<QComboBox>(window, "cbPrintUnit")->currentIndex(), 1);
        QCOMPARE(child<QSpinBox>(window, "sbDpi")->value(), 150);
        QCOMPARE(child<QSpinBox>(window, "sbPartColumns")->value(), 3);
        QCOMPARE(child<QSpinBox>(window, "sbPartRows")->value(), 2);
        QCOMPARE(child<QDoubleSpinBox>(window, "sbPartOverlap")->value(), 0.5);
        QVERIFY(child<QLabel>(window, "lblOutputInfo")->text().contains("6 parts"));
    }

    // Issue #19: with a fixed tile shape, the rows follow from the columns and can't be
    // edited; with "Free" they can.
    void tileShapeSetsRows()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString path = dir.filePath("base.png");
        QImage base(300, 200, QImage::Format_RGB32);
        base.fill(Qt::gray);
        QVERIFY(base.save(path));

        MainWindow window;
        QComboBox *shape = child<QComboBox>(window, "cbTileShape");
        QSpinBox *rows = child<QSpinBox>(window, "sbRows");
        QSpinBox *cols = child<QSpinBox>(window, "sbCols");

        window.onImageDropped(path);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();

        shape->setCurrentIndex(6);   // 1:1
        cols->setValue(30);
        QCOMPARE(rows->value(), 20);
        QVERIFY(!rows->isEnabled());

        shape->setCurrentIndex(1);   // 3:2
        QCOMPARE(rows->value(), 30);

        shape->setCurrentIndex(7);   // mixed: upright slots of 1 : sqrt(2)
        QCOMPARE(rows->value(), qRound(200 * 30 * ParquetSlotAspect / 300));
        QVERIFY(!rows->isEnabled());

        shape->setCurrentIndex(0);   // free
        QVERIFY(rows->isEnabled());
        rows->setValue(55);
        QCOMPARE(rows->value(), 55);
    }

    // Issue #17: the output size can be set as print size and resolution.
    void printSizeAndResolution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString path = dir.filePath("base.png");
        QImage base(300, 200, QImage::Format_RGB32);
        base.fill(Qt::gray);
        QVERIFY(base.save(path));

        MainWindow window;
        window.onImageDropped(path);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();

        QSpinBox *width = child<QSpinBox>(window, "sbWidth");
        QSpinBox *height = child<QSpinBox>(window, "sbHeight");
        QDoubleSpinBox *printWidth = child<QDoubleSpinBox>(window, "sbPrintWidth");
        QDoubleSpinBox *printHeight = child<QDoubleSpinBox>(window, "sbPrintHeight");
        QSpinBox *dpi = child<QSpinBox>(window, "sbDpi");
        QComboBox *unit = child<QComboBox>(window, "cbPrintUnit");

        // The output starts at the base image's size.
        QCOMPARE(width->value(), 300);
        QCOMPARE(height->value(), 200);

        dpi->setValue(300);
        unit->setCurrentIndex(0);   // cm

        // 30 x 20 cm at 300 dpi; the locked aspect ratio sets the height.
        printWidth->setValue(30.0);
        QCOMPARE(width->value(), 3543);
        QCOMPARE(height->value(), 2362);
        QCOMPARE(printHeight->value(), 20.0);

        // Half the resolution, same print size: half the pixels.
        dpi->setValue(150);
        QCOMPARE(width->value(), 1772);
        QCOMPARE(height->value(), 1181);
        QCOMPARE(printWidth->value(), 30.0);

        // In inches.
        unit->setCurrentIndex(1);
        QCOMPARE(printWidth->value(), 11.81);

        // Pixels change the print size too.
        width->setValue(1500);
        QCOMPARE(printWidth->value(), 10.0);
        QCOMPARE(height->value(), 1000);

        QVERIFY(child<QLabel>(window, "lblOutputInfo")->text().contains("1500 x 1000 px"));
    }

    // Settings from a newer or broken version must not leave the window unusable.
    void invalidSettingsAreIgnored()
    {
        int rawDefault;
        int matchingDefault;
        {
            MainWindow window;
            rawDefault = child<QComboBox>(window, "cbRawFiles")->currentIndex();
            matchingDefault = child<QComboBox>(window, "cbMatching")->currentIndex();
        }

        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("RawFiles", 99);
            settings.setValue("Matching", -5);
            settings.setValue("GridRows", 1000000);
        }

        MainWindow window;
        QCOMPARE(child<QComboBox>(window, "cbRawFiles")->currentIndex(), rawDefault);
        QCOMPARE(child<QComboBox>(window, "cbMatching")->currentIndex(), matchingDefault);
        QSpinBox *rows = child<QSpinBox>(window, "sbRows");
        QCOMPARE(rows->value(), rows->maximum());
    }

    // Debug mode: the last base image, output size and tile folder come back on the next
    // start. Without the environment variable nothing is reopened.
    void debugSessionRestoresLastRun()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString base = dir.filePath("base.png");
        QImage image(300, 200, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(base));

        QString tiles = dir.filePath("tiles");
        QVERIFY(QDir().mkpath(tiles));
        for (int i = 0; i < 3; ++i)
        {
            QImage photo(40, 30, QImage::Format_RGB32);
            photo.fill(QColor::fromHsv(i * 100, 200, 200));
            QVERIFY(photo.save(QDir(tiles).filePath(QString("photo%1.png").arg(i))));
        }

        {
            MainWindow window;
            window.onImageDropped(base);
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            child<QSpinBox>(window, "sbWidth")->setValue(1500);
            child<QSpinBox>(window, "sbHeight")->setValue(1000);
        }

        // The tile folder last used, as the previous run left it.
        {
            QSettings settings(settingsPath(), QSettings::IniFormat);
            settings.setValue("LastTileFolder", tiles);
        }

        qputenv("IMAGEMOSAICWALL_DEBUG_SESSION", "1");
        {
            MainWindow window;
            QThreadPool::globalInstance()->waitForDone();
            QVERIFY(QTest::qWaitFor([&] { return child<ProgressButton>(window, "btnSetImageFolder")->maxValue() == 3; },
                                    10000));
            // The analysis runs in the background; the window must not go before it.
            QThreadPool::globalInstance()->waitForDone();
            QCOMPARE(child<QSpinBox>(window, "sbWidth")->value(), 1500);
            QCOMPARE(child<QSpinBox>(window, "sbHeight")->value(), 1000);
            QVERIFY(child<QLabel>(window, "lblOutputInfo")->text().contains("1500 x 1000 px"));
        }

        // Without the variable the window starts empty, as before.
        qunsetenv("IMAGEMOSAICWALL_DEBUG_SESSION");
        {
            MainWindow window;
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            QVERIFY(child<QSpinBox>(window, "sbWidth")->value() != 1500);
            // The folder is not scanned: its 3 photos never become the count.
            QVERIFY(child<ProgressButton>(window, "btnSetImageFolder")->maxValue() != 3);
        }
    }

    // The tiles chosen last time can be used again with one button, and debug mode uses them
    // on start, also when the host (digiKam) has already handed over its image.
    void rememberedTilesAreUsedAgain()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString base = dir.filePath("base.png");
        QImage image(300, 200, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(base));

        QList<QString> photos;
        for (int i = 0; i < 3; ++i)
        {
            QImage photo(40, 30, QImage::Format_RGB32);
            photo.fill(QColor::fromHsv(i * 100, 200, 200));
            QString path = dir.filePath(QString("tag%1.png").arg(i));
            QVERIFY(photo.save(path));
            photos << path;
        }

        auto remembered = [&]() { return photos; };

        // The window before: the host hands over an image, the output is set, and the
        // window is closed; the settings keep the image and the output size.
        {
            MainWindow window;
            window.setRememberedTileSource("Tiles from last selection", remembered);
            window.onImageDropped(base);
            QThreadPool::globalInstance()->waitForDone();
            QCoreApplication::processEvents();
            child<QSpinBox>(window, "sbWidth")->setValue(1500);
            child<QSpinBox>(window, "sbHeight")->setValue(1000);
        }

        // The button uses the remembered files.
        {
            MainWindow window;
            window.setRememberedTileSource("Tiles from last selection", remembered);
            QPushButton *last = child<QPushButton>(window, "btnTilesLast");
            QVERIFY(!last->isHidden());
            last->click();
            QVERIFY(QTest::qWaitFor([&] { return child<ProgressButton>(window, "btnSetImageFolder")->maxValue() == 3; },
                                    10000));
            // The analysis runs in the background; the window must not go before it.
            QThreadPool::globalInstance()->waitForDone();
        }

        // Debug mode, the host hands over the same image: the tiles and the output size come back.
        qputenv("IMAGEMOSAICWALL_DEBUG_SESSION", "1");
        {
            MainWindow window;
            window.setRememberedTileSource("Tiles from last selection", remembered);
            window.onImageDropped(base);
            QThreadPool::globalInstance()->waitForDone();
            QVERIFY(QTest::qWaitFor([&] { return child<ProgressButton>(window, "btnSetImageFolder")->maxValue() == 3; },
                                    10000));
            // The analysis runs in the background; the window must not go before it.
            QThreadPool::globalInstance()->waitForDone();
            QCOMPARE(child<QSpinBox>(window, "sbWidth")->value(), 1500);
            QCOMPARE(child<QSpinBox>(window, "sbHeight")->value(), 1000);
        }
        qunsetenv("IMAGEMOSAICWALL_DEBUG_SESSION");

        // Nothing remembered: the button says so and loads nothing.
        {
            MainWindow window;
            window.setRememberedTileSource("Tiles from last selection", [] { return QList<QString>(); });
            child<QPushButton>(window, "btnTilesLast")->click();
            QVERIFY(child<QLabel>(window, "lblStatus")->text().contains("remembered"));
            QVERIFY(child<ProgressButton>(window, "btnSetImageFolder")->maxValue() != 3);
        }
    }

    // A click on a tile of the mosaic names its photo; excluding the photo keeps it out.
    void tilePanelShowsAndExcludes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString base = dir.filePath("base.png");
        QImage image(120, 80, QImage::Format_RGB32);
        image.fill(Qt::gray);
        QVERIFY(image.save(base));

        QList<QString> photos;
        for (int i = 0; i < 3; ++i)
        {
            QImage photo(30, 20, QImage::Format_RGB32);
            photo.fill(QColor::fromHsv(i * 120, 200, 200));
            QString path = dir.filePath(QString("pick%1.png").arg(i));
            QVERIFY(photo.save(path));
            photos << path;
        }

        MainWindow window;
        window.setRememberedTileSource("Tiles from last selection", [photos]() { return photos; });
        window.onImageDropped(base);
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();

        child<QComboBox>(window, "cbTileShape")->setCurrentIndex(0);   // free rows and columns
        child<QSpinBox>(window, "sbCols")->setValue(6);
        child<QSpinBox>(window, "sbRows")->setValue(4);
        child<QPushButton>(window, "btnTilesLast")->click();
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();

        child<QPushButton>(window, "btnGenerate")->click();
        QVERIFY(QTest::qWaitFor([&] { return child<QPushButton>(window, "btnSave")->isEnabled(); }, 20000));
        QThreadPool::globalInstance()->waitForDone();
        QVERIFY(child<QPushButton>(window, "btnExportList")->isEnabled());

        // The mosaic is 120 x 80 here, the view shows it at the same size.
        window.onTileClicked(QPointF(5, 5));
        QVERIFY(!child<QFrame>(window, "tileInfo")->isHidden());
        QVERIFY(child<QLabel>(window, "tileInfoText")->text().contains("Row 1, column 1"));
        QString before = child<QLabel>(window, "tileInfoText")->toolTip();

        // Excluded: the tile shows another photo now, and the status says so.
        child<QPushButton>(window, "btnExcludeTile")->click();
        QVERIFY(child<QLabel>(window, "lblStatus")->text().contains("left out"));
        QVERIFY(!child<QFrame>(window, "tileInfo")->isHidden());
        QVERIFY(child<QLabel>(window, "tileInfoText")->toolTip() != before);

        // A point outside the mosaic hides the panel.
        window.onTileClicked(QPointF(-50, -50));
        QVERIFY(child<QFrame>(window, "tileInfo")->isHidden());
    }

    // The step bar shows the step in progress and how far it has got.
    void stepProgressShowsTheBar()
    {
        MainWindow window;
        QProgressBar *bar = child<QProgressBar>(window, "barStep");

        window.onStepProgress("Placing photos", 120, 2120);
        QCOMPARE(bar->maximum(), 2120);
        QCOMPARE(bar->value(), 120);
        QVERIFY(bar->format().contains("Placing photos"));

        // Progress reported past the total stays at the end of the bar.
        window.onStepProgress("Placing photos", 3000, 2120);
        QCOMPARE(bar->value(), 2120);
    }
};

QTEST_MAIN(TestMainWindow)

#include "test_mainwindow.moc"
