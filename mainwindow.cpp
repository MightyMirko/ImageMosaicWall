
#include "mainwindow.h"
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QSettings>
#include <QString>
#include "asyncdirectoryscanner.h"
#include "progressbutton.h"
#include "ui_mainwindow.h"
#include "i18nutils.h"

using namespace QtConcurrent;

namespace {

// Settings are kept in an INI file on every platform (also instead of the Windows
// registry), e.g. ~/.config/ImageMosaicWall/ImageMosaicWall.ini.
const QSettings::Format SettingsFormat = QSettings::IniFormat;
const char SettingsOrganization[] = "ImageMosaicWall";
const char SettingsApplication[] = "ImageMosaicWall";

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    s_initI18nResource();
    s_loadI18n(QLatin1String("ImageMosaicWall"));

    QFile file(":/styles/default.qss");
    if (file.open(QFile::ReadOnly))
        setStyleSheet(QLatin1String(file.readAll()));
    else
        qWarning() << "Could not load the style sheet" << file.fileName();

    ui->btnCancel->hide();
    ui->wImageContainer->layout()->addWidget(&m_imageView);

    qRegisterMetaType<GridPoint>("GridPoint");
    connect(ui->sbRows, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onGridPropsValueChanged);
    connect(ui->sbCols, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::onGridPropsValueChanged);
    connect(ui->btnSetResolution, SIGNAL(clicked(bool)), this, SLOT(onOutputResolutionChanged()));

    // The combo box entries are in the order of the RawFiles values.
    connect(ui->cbRawFiles, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        if (!m_tileFiles.isEmpty())
            loadTileImages(m_tileFiles);
    });

    // Only shown when the host application (the digiKam plugin) provides a tile source.
    ui->btnTilesFromHost->hide();

    connect(ui->btnCancel, &QPushButton::clicked, [&]() {
        qDebug() << "Cancel!";
        m_imageProcessing.processCanceled(true);
    });

    connect(&m_imageProcessing, &ImageProcessing::mosaicGenerated, &m_imageView, &ImageViewer::setLoadingMosaicAt);
    connect(&m_imageProcessing, &ImageProcessing::imageProcessed, this, &MainWindow::onImageLoadPorgress);

    connect(&m_imageProcessing, &ImageProcessing::cellProcessed, this, &MainWindow::onImageCellProgress);

    connect(&m_imageView, &ImageViewer::folderDropped, this, &MainWindow::onFolderDropped);
    connect(&m_imageView, &ImageViewer::imageDropped, this, &MainWindow::onImageDropped);
    connect(ui->btnLockRatio, &QPushButton::clicked, [=]() {
        m_lockedResolution.setWidth(ui->sbWidth->value());
        m_lockedResolution.setHeight(ui->sbHeight->value());

        if (ui->btnLockRatio->isChecked())
            ui->btnLockRatio->setIcon(QIcon(":/icons/assets/locked.png"));
        else
            ui->btnLockRatio->setIcon(QIcon(":/icons/assets/opened.png"));
    });
    connect(ui->sbWidth, QOverload<int>::of(&QSpinBox::valueChanged), [=]() {
        if (ui->btnLockRatio->isChecked())
        {
            ui->sbHeight->blockSignals(true);
            scaleLockedImageSize(true);
            ui->sbHeight->blockSignals(false);
        }
    });
    connect(ui->sbHeight, QOverload<int>::of(&QSpinBox::valueChanged), [=]() {
        if (ui->btnLockRatio->isChecked())
        {
            ui->sbWidth->blockSignals(true);
            scaleLockedImageSize(false);
            ui->sbWidth->blockSignals(false);
        }
    });

    connect(ui->slHistory, QOverload<int>::of(&QSlider::valueChanged), [=]() {
        QString val;
        switch (ui->slHistory->value())
        {
            case 0:
                val = tr("disabled");
                break;
            case 1:
                val = tr("very low");
                break;
            case 2:
                val = tr("low");
                break;
            case 3:
                val = tr("medium");
                break;
            case 4:
                val = tr("high");
                break;
            case 5:
                val = tr("very high");
                break;
            default:
                break;
        }

        ui->lblVariation->setText(val);
    });

    // After the connections, so the restored slider position also updates its label.
    loadSettings();
}

MainWindow::~MainWindow()
{
    saveSettings();
    delete ui;

    s_cleanupI18nResource();
}

void MainWindow::loadSettings()
{
    QSettings settings(SettingsFormat, QSettings::UserScope, SettingsOrganization, SettingsApplication);

    // Spin boxes and the slider clamp out of range values themselves; combo box
    // indexes are checked, since an invalid one would select nothing.
    auto comboIndex = [&](QComboBox *box, const QString &key) {
        int index = settings.value(key, box->currentIndex()).toInt();
        if (index >= 0 && index < box->count())
            box->setCurrentIndex(index);
    };

    comboIndex(ui->cbRawFiles, "RawFiles");
    ui->slHistory->setValue(settings.value("Variation", ui->slHistory->value()).toInt());
    ui->sbMaxUses->setValue(settings.value("MaxUses", ui->sbMaxUses->value()).toInt());
    ui->cbEveryPhoto->setChecked(settings.value("EveryPhoto", ui->cbEveryPhoto->isChecked()).toBool());
    ui->sbTint->setValue(settings.value("Tint", ui->sbTint->value()).toInt());
    comboIndex(ui->cbMatching, "Matching");
    ui->sbRows->setValue(settings.value("GridRows", ui->sbRows->value()).toInt());
    ui->sbCols->setValue(settings.value("GridColumns", ui->sbCols->value()).toInt());

    m_lastImageFolder = settings.value("LastImageFolder").toString();
    m_lastTileFolder = settings.value("LastTileFolder").toString();
    m_lastSaveFolder = settings.value("LastSaveFolder").toString();
}

void MainWindow::saveSettings() const
{
    QSettings settings(SettingsFormat, QSettings::UserScope, SettingsOrganization, SettingsApplication);

    settings.setValue("RawFiles", ui->cbRawFiles->currentIndex());
    settings.setValue("Variation", ui->slHistory->value());
    settings.setValue("MaxUses", ui->sbMaxUses->value());
    settings.setValue("EveryPhoto", ui->cbEveryPhoto->isChecked());
    settings.setValue("Tint", ui->sbTint->value());
    settings.setValue("Matching", ui->cbMatching->currentIndex());
    settings.setValue("GridRows", ui->sbRows->value());
    settings.setValue("GridColumns", ui->sbCols->value());

    settings.setValue("LastImageFolder", m_lastImageFolder);
    settings.setValue("LastTileFolder", m_lastTileFolder);
    settings.setValue("LastSaveFolder", m_lastSaveFolder);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // The digiKam plugin only hides the window on close, so the destructor may run
    // much later (or never, if digiKam crashes); save here as well.
    saveSettings();
    QMainWindow::closeEvent(event);
}

QString MainWindow::startFolder(const QString &lastFolder) const
{
    // The folder the host application opened the tool from comes first.
    if (!m_currentFolder.isEmpty())
        return m_currentFolder;

    if (!lastFolder.isEmpty() && QDir(lastFolder).exists())
        return lastFolder;

    return QDir::homePath();
}

void MainWindow::updateStatus()
{
    QString imageInfo = QString("<b style=\"text-indent: 100px\">%1</b> %2x%3")
                            .arg(tr("Image Resolution:"))
                            .arg(m_baseImage.width())
                            .arg(m_baseImage.height());

    QString folderInfo = QString("<b style=\"text-indent: 100px\">%1</b> %2")
                             .arg(tr("Images in Folder:"))
                             .arg(m_imageProcessing.getImageMeanMap().size());

    ui->lblStatus->setText(QString("%1<br/>%2").arg(imageInfo).arg(folderInfo));

    if (!m_baseImage.isNull())
        ui->btnSetResolution->setEnabled(true);

    ui->btnCancel->hide();
}

void MainWindow::toggleUI(bool enabled)
{
    auto out = m_imageProcessing.getOutputImage();
    ui->btnGenerate->setEnabled(enabled && m_imageProcessing.isReady());
    ui->btnSave->setEnabled(enabled && out != nullptr);

    ui->btnLoad->setEnabled(enabled);
    ui->btnSetResolution->setEnabled(enabled);
    ui->btnSetImageFolder->setEnabled(enabled);
    ui->cbRawFiles->setEnabled(enabled);
    ui->btnTilesFromHost->setEnabled(enabled);

    ui->sbCols->setEnabled(enabled);
    ui->sbRows->setEnabled(enabled);
    ui->sbHeight->setEnabled(enabled);
    ui->sbWidth->setEnabled(enabled);
    ui->slHistory->setEnabled(enabled);
    ui->sbMaxUses->setEnabled(enabled);
    ui->cbEveryPhoto->setEnabled(enabled);
    ui->sbTint->setEnabled(enabled);
    ui->cbMatching->setEnabled(enabled);
}

void MainWindow::loadImage(QString &filename)
{
    if (filename.isEmpty())
        return;

    m_baseImage = loadOrientedImage(filename);

    if (m_baseImage.isNull())
        return;

    m_baseImagePath = filename;
    m_lastImageFolder = QFileInfo(filename).absolutePath();

    if (ui->btnLockRatio->isChecked())
        m_lockedResolution = m_baseImage.size();

    ui->sbWidth->setValue(m_baseImage.width());
    ui->sbHeight->setValue(m_baseImage.height());

    QSize gridSize = QSize(ui->sbCols->value(), ui->sbRows->value());

    ui->btnLoad->setRange(0, gridSize.width() * gridSize.height());
    ui->btnCancel->show();
    m_imageView.setImage(m_baseImage);
    m_imageView.setGrid(gridSize);

    QFutureWatcher<void> *watcher = new QFutureWatcher<void>(this);
    QFuture<void> f = QtConcurrent::run([=]() { m_imageProcessing.processGrid(m_baseImage, gridSize); });

    connect(watcher, &QFutureWatcher<void>::finished, [=]() {
        m_processedGridSize = gridSize;
        ui->btnGenerate->setEnabled(m_imageProcessing.isReady());
        watcher->deleteLater();
        updateStatus();
        toggleUI(true);
    });

    toggleUI(false);
    m_imageProcessing.processCanceled(false);
    watcher->setFuture(f);
}

void MainWindow::loadImageFolder(QString &path)
{
    if (path.isEmpty())
        return;

    m_lastTileFolder = path;

    QStringList filter = supportedImageNameFilters();

    auto scanner = new AsyncDirectoryScanner(
        path, filter, QDir::Files | QDir::Dirs | QDir::NoDot | QDir::NoDotDot, QDirIterator::Subdirectories);

    connect(scanner, &AsyncDirectoryScanner::fileScanned, this, [=](const QString file) {
        auto message = tr("%1 images scanned").arg(scanner->numFilesScanned());
        ui->lblStatus->setText(message);
    });

    connect(scanner, &AsyncDirectoryScanner::finished, this, [=]() {
        qDebug() << "finished scanning";
        loadTileImages(scanner->scannedFiles());
        scanner->deleteLater();
    });

    scanner->start();
}

void MainWindow::loadTileImages(const QList<QString> &files)
{
    // Kept so a change of the RAW choice can be applied without scanning again.
    m_tileFiles = files;

    auto imageList = applyRawFilesPolicy(files, static_cast<RawFiles>(ui->cbRawFiles->currentIndex()));

    ui->btnSetImageFolder->setRange(0, qMax(1, int(imageList.size())));
    ui->btnCancel->show();

    QFutureWatcher<void> *watcher = new QFutureWatcher<void>(this);
    QFuture<void> f = QtConcurrent::run([=]() { m_imageProcessing.processMosaicImages(imageList); });

    connect(watcher, &QFutureWatcher<void>::finished, this, [=]() {
        watcher->deleteLater();
        updateStatus();
        toggleUI(true);
    });

    toggleUI(false);
    m_imageProcessing.processCanceled(false);
    watcher->setFuture(f);
}

void MainWindow::setTileSource(const QString &text, std::function<QList<QString>()> chooseFiles)
{
    m_chooseTileFiles = chooseFiles;
    ui->btnTilesFromHost->setText(text);
    ui->btnTilesFromHost->setVisible(bool(chooseFiles));
}

void MainWindow::on_btnTilesFromHost_clicked()
{
    if (!m_chooseTileFiles)
        return;

    QList<QString> files = m_chooseTileFiles();

    if (!files.isEmpty())
        loadTileImages(files);
}

void MainWindow::scaleLockedImageSize(bool senderIsWidth)
{
    double n = qMax(m_lockedResolution.width(), m_lockedResolution.height());
    double d = qMin(m_lockedResolution.width(), m_lockedResolution.height());
    double ratio = n / d;

    if (ui->sbWidth->value() >= ui->sbHeight->value())
    {
        if (senderIsWidth)
            ui->sbHeight->setValue(ui->sbWidth->value() / ratio);
        else
            ui->sbWidth->setValue(ui->sbHeight->value() * ratio);
    }
    else
    {
        if (senderIsWidth)
            ui->sbHeight->setValue(ui->sbWidth->value() * ratio);
        else
            ui->sbWidth->setValue(ui->sbHeight->value() / ratio);
    }
}

void MainWindow::on_btnLoad_clicked()
{
    QString filename = QFileDialog::getOpenFileName(this,
                                                    tr("Open Image"),
                                                    startFolder(m_lastImageFolder),
                                                    tr("Images (%1)").arg(supportedImageNameFilters().join(' ')));
    loadImage(filename);
}

void MainWindow::onGridPropsValueChanged()
{
    if (m_baseImage.width() == 0 || m_baseImage.height() == 0)
        return;

    // Only update the view here. The grid cell colors are recalculated when the mosaic
    // is generated, so a spin box change cannot race with a running generation.
    QSize gridSize(ui->sbCols->value(), ui->sbRows->value());
    m_imageView.setGrid(gridSize);
    ui->btnLoad->setRange(0, gridSize.width() * gridSize.height());
}

void MainWindow::onOutputResolutionChanged()
{
    QSize res = QSize(ui->sbWidth->value(), ui->sbHeight->value());

    if (res.width() <= 0 || res.height() <= 0 || m_baseImage.isNull())
        return;

    m_baseImage = m_baseImage.scaled(res);
    m_processedGridSize = QSize();
    m_imageView.setImage(m_baseImage);
    onGridPropsValueChanged();
    ui->btnSave->setEnabled(false);
}

void MainWindow::onMosaicCreationFinished()
{
    m_imageView.setMosaicLoadingDone();

    auto out = m_imageProcessing.getOutputImage();
    if (out != nullptr)
        m_imageView.setPreview(*out);

    toggleUI(true);
    ui->btnCancel->hide();
    delete m_mosaicGeneration;
}

void MainWindow::onImageDropped(QString image)
{
    loadImage(image);
}

void MainWindow::onFolderDropped(QString folder)
{
    loadImageFolder(folder);
}

void MainWindow::onImageCellProgress()
{
    ui->btnLoad->increment();

    auto current = ui->btnLoad->currentValue();
    auto max = ui->btnLoad->maxValue();
    auto message = tr("%1 of %2 cells processed").arg(current).arg(max);

    ui->lblStatus->setText(message);
}

void MainWindow::onImageLoadPorgress()
{
    ui->btnSetImageFolder->increment();

    auto current = ui->btnSetImageFolder->currentValue();
    auto max = ui->btnSetImageFolder->maxValue();
    auto message = tr("%1 of %2 images processed").arg(current).arg(max);

    ui->lblStatus->setText(message);
}

void MainWindow::on_btnGenerate_clicked()
{
    toggleUI(false);
    m_imageProcessing.processCanceled(false);
    m_imageView.clearPreview();
    ui->btnCancel->show();

    // Read all widget state here on the GUI thread; the generation thread must not touch the UI.
    QSize gridSize = QSize(ui->sbCols->value(), ui->sbRows->value());
    int history = m_variations[ui->slHistory->value()];
    int maxUses = ui->sbMaxUses->value();
    bool everyPhoto = ui->cbEveryPhoto->isChecked();
    double tint = ui->sbTint->value() / 100.0;
    // The combo box entries are in the order of the Matching values.
    Matching matching = static_cast<Matching>(ui->cbMatching->currentIndex());
    bool gridChanged = gridSize != m_processedGridSize;
    QImage baseImage = m_baseImage;
    m_processedGridSize = gridSize;
    saveSettings();

    m_mosaicGeneration = QThread::create([=] {
        if (gridChanged)
            m_imageProcessing.processGrid(baseImage, gridSize);

        m_imageProcessing.generateImage(baseImage.size(), gridSize, history, maxUses, tint, matching, everyPhoto);
    });
    connect(m_mosaicGeneration, SIGNAL(finished()), this, SLOT(onMosaicCreationFinished()));
    m_mosaicGeneration->start();
}

void MainWindow::on_btnSave_clicked()
{
    const QImage *out = m_imageProcessing.getOutputImage();
    if (out == nullptr)
        return;

    QString pngFilter = tr("PNG (*.png)");
    QString jpegFilter = tr("JPEG (*.jpg)");
    QString selectedFilter = pngFilter;

    // Suggested next to the base image, or in the folder the last mosaic was saved to.
    bool hasSaveFolder = !m_lastSaveFolder.isEmpty() && QDir(m_lastSaveFolder).exists();
    QString suggestion = suggestedMosaicFileName(m_baseImagePath, hasSaveFolder ? m_lastSaveFolder : QString());

    QString filename = QFileDialog::getSaveFileName(this,
                                                    tr("Save Mosaic Image"),
                                                    suggestion,
                                                    pngFilter + ";;" + jpegFilter,
                                                    &selectedFilter);

    if (filename.isEmpty())
        return;

    // QImage::save() picks the format from the suffix and fails without one.
    if (QFileInfo(filename).suffix().isEmpty())
        filename += (selectedFilter == jpegFilter) ? ".jpg" : ".png";

    // Overwriting the base image would replace the original photo and lose its metadata.
    if (!m_baseImagePath.isEmpty() && QFileInfo(filename) == QFileInfo(m_baseImagePath))
    {
        QMessageBox::warning(this,
                             tr("Save Mosaic Image"),
                             tr("The mosaic can't be saved over its base image. Please choose another file name."));
        return;
    }

    m_lastSaveFolder = QFileInfo(filename).absolutePath();

    // Save in the background: encoding a large mosaic, or waiting for a sleeping
    // disk to spin up, would otherwise freeze the whole window (and digiKam).
    QImage image = *out;
    QString nativeName = QDir::toNativeSeparators(filename);

    toggleUI(false);
    ui->lblStatus->setText(tr("Saving %1 ...").arg(nativeName));

    auto watcher = new QFutureWatcher<bool>(this);

    connect(watcher, &QFutureWatcher<bool>::finished, this, [=]() {
        bool saved = watcher->result();
        watcher->deleteLater();
        toggleUI(true);

        if (saved)
        {
            ui->lblStatus->setText(tr("Saved to %1").arg(nativeName));
        }
        else
        {
            ui->lblStatus->clear();
            QMessageBox::warning(this,
                                 tr("Save Mosaic Image"),
                                 tr("The mosaic could not be saved to %1.").arg(nativeName));
        }
    });

    watcher->setFuture(QtConcurrent::run([image, filename]() { return image.save(filename); }));
}

void MainWindow::on_btnSetImageFolder_clicked()
{
    // Not startFolder(): the tile folder is usually a different, larger one than the
    // album the tool was opened from.
    QString start = QDir(m_lastTileFolder).exists() && !m_lastTileFolder.isEmpty() ? m_lastTileFolder
                                                                                  : startFolder(QString());
    QString path = QFileDialog::getExistingDirectory(this, tr("Load Images Folder"), start);
    loadImageFolder(path);
}

void MainWindow::setCurrentFolder(const QString &folder)
{
    m_currentFolder = folder;
}
