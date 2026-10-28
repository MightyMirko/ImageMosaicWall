#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSettings>
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
};

QTEST_MAIN(TestMainWindow)

#include "test_mainwindow.moc"
