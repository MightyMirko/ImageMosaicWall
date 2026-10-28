#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "imageprocessing.h"
#include "imageviewer.h"
#include <QFutureWatcher>
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

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void loadSettings();
  void saveSettings() const;
  QString startFolder(const QString &lastFolder) const;
  void updateStatus();
  void toggleUI(bool enabled);
  void loadImage(QString &);
  void loadImageFolder(QString &);
  void loadTileImages(const QList<QString> &files);
  void scaleLockedImageSize(bool senderIsWidth);
  double tileAspect() const;
  void applyTileShape();

signals:
  void mosaicCalculationFinished(QMap<GridPoint, QImage> &);

public slots:
  void on_btnLoad_clicked();
  void onGridPropsValueChanged();
  void onOutputResolutionChanged();
  void onMosaicCreationFinished();
  void onImageDropped(QString image);
  void onFolderDropped(QString folder);
  void onImageCellProgress();
  void onImageLoadPorgress();

private slots:
  void on_btnGenerate_clicked();
  void on_btnSave_clicked();
  void on_btnSetImageFolder_clicked();
  void on_btnTilesFromHost_clicked();

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

  const ushort m_variations[6] = {0, 1, 3, 9, 11, 15};
  // Width / height for the entries of the tile shape combo box; 0 is "Free".
  const double m_tileAspects[7] = {0.0, 3.0 / 2.0, 2.0 / 3.0, 4.0 / 3.0, 3.0 / 4.0, 16.0 / 9.0, 1.0};
  QString m_currentFolder;
  QString m_lastImageFolder;
  QString m_lastTileFolder;
  QString m_lastSaveFolder;
  QString m_baseImagePath;
  QList<QString> m_tileFiles;
  std::function<QList<QString>()> m_chooseTileFiles;
};

#endif // MAINWINDOW_H
