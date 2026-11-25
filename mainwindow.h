#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "imageprocessing.h"
#include "imageviewer.h"
#include <QFrame>
#include <QFutureWatcher>
#include <QLabel>
#include <QImage>
#include <QMainWindow>
#include <QMap>
#include <QMovie>
#include <QPushButton>
#include <QtConcurrent>
#include <functional>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = 0);
  ~MainWindow();

  void setCurrentFolder(const QString &folder);

  // Adds a button that loads tile images from the host application, e.g. digiKam's
  // albums and tags. chooseFiles returns the chosen files, or nothing if cancelled.
  void setTileSource(const QString &text, std::function<QList<QString>()> chooseFiles);
  // Thumbnails of the photos from the host, used for the analysis instead of reading the photos.
  void setPhotoThumbnails(ImageProcessing::ThumbnailProvider provider);
  // A button that uses the tiles chosen last time, without asking again; the function returns them.
  void setRememberedTileSource(const QString &text, std::function<QList<QString>()> rememberedFiles);

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void loadSettings();
  void saveSettings() const;
  // Debug mode (IMAGEMOSAICWALL_DEBUG_SESSION=1): reopens the last base image, tile folder
  // and output size, so a test run starts where the last one ended.
  void restoreDebugSession();
  QString startFolder(const QString &lastFolder) const;
  void updateStatus();
  void toggleUI(bool enabled);
  void loadImage(QString &);
  void loadImageFolder(QString &);
  void loadTileImages(const QList<QString> &files);
  void scaleLockedImageSize(bool senderIsWidth);
  double tileAspect() const;
  void applyTileShape();
  QSize outputSize() const;
  double printUnitsPerInch() const;
  void outputSizeChanged();
  void updatePrintSize();
  void setPixelsFromPrintSize(bool fromWidth);
  void updateOutputInfo();
  // The regions of the poster parts of a mosaic of size, row by row.
  QList<QRect> posterParts(QSize size) const;

signals:
  void mosaicCalculationFinished(QMap<GridPoint, QImage> &);

public slots:
  void on_btnLoad_clicked();
  void onGridPropsValueChanged();
  void onMosaicCreationFinished();
  void onImageDropped(QString image);
  void onFolderDropped(QString folder);
  void onImageCellProgress();
  void onImageLoadPorgress();
  void onStepProgress(const QString &step, int done, int total);
  // A click on the mosaic: the tile under it gets the tile panel.
  void onTileClicked(QPointF scenePos);
  void swapSelectedTile();
  void excludeSelectedTile();
  void resetExclusions();
  void exportTileList();

private slots:
  void on_btnGenerate_clicked();
  void on_btnSave_clicked();
  void on_btnSetImageFolder_clicked();
  void on_btnTilesFromHost_clicked();
  void on_btnTilesLast_clicked();

private:
  Ui::MainWindow *ui;
  QImage m_baseImage;
  ImageProcessing m_imageProcessing;
  ImageViewer m_imageView;
  QFutureWatcher<QRgb> *m_imageScaling;
  QThread *m_mosaicGeneration;
  QMap<QString, QIcon> m_activeLoadingButtons;
  QSize m_lockedResolution;
  QSize m_processedGridSize;
  QImage m_largePreview;   // shown for mosaics too large to be kept in memory

  // The tile panel over the image, shown after a click on a tile, and its buttons.
  void setupTileInfo();
  void setupExport();
  void showTileInfo();
  void hideTileInfo();
  void refreshMosaic();
  int m_selectedTile = -1;
  QFrame *m_tileInfo = nullptr;
  QLabel *m_tileInfoText = nullptr;
  QPushButton *m_btnSwapTile = nullptr;
  QPushButton *m_btnExcludeTile = nullptr;
  QPushButton *m_btnCopyPath = nullptr;
  QPushButton *m_btnOpenPhoto = nullptr;
  QPushButton *m_btnResetExclusions = nullptr;
  QPushButton *m_btnCloseTile = nullptr;
  QPushButton *m_btnExportList = nullptr;
  int m_dpi = 300;   // the resolution the output size was last scaled for

  const ushort m_variations[6] = {0, 1, 3, 9, 11, 15};
  // Width / height for the entries of the tile shape combo box; 0 is "Free", the last
  // one is the slot of the parquet pattern ("Mixed").
  const double m_tileAspects[8] = {0.0, 3.0 / 2.0, 2.0 / 3.0, 4.0 / 3.0, 3.0 / 4.0, 16.0 / 9.0, 1.0,
                                   ParquetSlotAspect};
  static const int MixedTileShape = 7;
  QString m_currentFolder;
  QString m_lastImageFolder;
  QString m_lastTileFolder;
  QString m_lastSaveFolder;
  QString m_baseImagePath;
  QList<QString> m_tileFiles;
  std::function<QList<QString>()> m_chooseTileFiles;
  std::function<QList<QString>()> m_rememberedTileFiles;
};

#endif // MAINWINDOW_H
