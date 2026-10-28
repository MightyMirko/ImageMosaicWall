#include "mainwindow.h"

#include <QComboBox>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

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
            child<QSpinBox>(window, "sbTint")->setValue(20);
            child<QComboBox>(window, "cbMatching")->setCurrentIndex(1);
            child<QSpinBox>(window, "sbRows")->setValue(150);
            child<QSpinBox>(window, "sbCols")->setValue(80);
        }

        MainWindow window;
        QCOMPARE(child<QComboBox>(window, "cbRawFiles")->currentIndex(), 2);
        QCOMPARE(child<QSlider>(window, "slHistory")->value(), 3);
        QCOMPARE(child<QSpinBox>(window, "sbMaxUses")->value(), 4);
        QCOMPARE(child<QSpinBox>(window, "sbTint")->value(), 20);
        QCOMPARE(child<QComboBox>(window, "cbMatching")->currentIndex(), 1);
        QCOMPARE(child<QSpinBox>(window, "sbRows")->value(), 150);
        QCOMPARE(child<QSpinBox>(window, "sbCols")->value(), 80);
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
