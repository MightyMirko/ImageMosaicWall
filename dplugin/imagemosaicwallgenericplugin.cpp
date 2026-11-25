/* ============================================================
 *
 * This file is a part of digiKam project
 * https://www.digikam.org
 *
 * Date        : 2020-05-26
 * Description : ImageMosaicWall generic plugin.
 *
 * Copyright (C) 2020-2023 by Gilles Caulier <caulier dot gilles at gmail dot com>
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation;
 * either version 2, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ============================================================ */

#include "imagemosaicwallgenericplugin.h"

// Qt includes

#include <QApplication>
#include <QMessageBox>
#include <QList>
#include <QUrl>
#include <QFontDatabase>
#include <QDialog>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QSettings>
#include <QStringList>

// digiKam includes

#include <dinfointerface.h>
#include <thumbnailinfo.h>
#include <thumbnailloadthread.h>
#include <dmessagebox.h>

// Local includes

#include "mainwindow.h"

// NOTE: need to be done outside plugin namespace.
void initImageMosaicWallResource()
{
    Q_INIT_RESOURCE(assets);
}

namespace DigikamGenericImageMosaicWallPlugin
{

namespace
{

// digiKam keeps the checked tags and albums of the chooser only when the chooser is destroyed,
// and the plugin keeps it alive. So the selection is kept here instead, in the window's own
// settings file, and can be used again without choosing it again.
const char RememberedAlbumsKey[] = "RememberedAlbumIds";

QSettings rememberedSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "ImageMosaicWall", "ImageMosaicWall");
}

void rememberAlbums(const QList<int>& albums)
{
    QStringList ids;

    for (int id : albums)
    {
        ids << QString::number(id);
    }

    rememberedSettings().setValue(QLatin1String(RememberedAlbumsKey), ids);
}

QList<int> rememberedAlbums()
{
    QList<int> albums;

    for (const QString& id : rememberedSettings().value(QLatin1String(RememberedAlbumsKey)).toStringList())
    {
        bool ok = false;
        int value = id.toInt(&ok);

        if (ok)
        {
            albums << value;
        }
    }

    return albums;
}

// digiKam's thumbnail of a photo, from its thumbnail database; digiKam makes it when there is none,
// as it does when browsing. Called from the analysis threads, like digiKam's own face pipeline
// does: a catcher per call, which gets the result directly from the loading thread.
QImage digiKamThumbnail(const QString& path, QSize size)
{
    ThumbnailLoadThread* const thread = ThumbnailLoadThread::defaultThread();

    if (!thread)
    {
        return QImage();
    }

    ThumbnailImageCatcher catcher(thread);
    catcher.setActive(true);

    thread->find(ThumbnailIdentifier(path), qMax(size.width(), size.height()));
    catcher.enqueue();

    const QList<QImage> images = catcher.waitForThumbnails();

    return images.isEmpty() ? QImage() : images.first();
}

// The local files of the given tags and albums, each photo once.
QList<QString> albumFiles(const DInfoInterface* iface, const QList<int>& albums)
{
    QList<QString> files;

    if (albums.isEmpty())
    {
        return files;
    }

    const QList<QUrl> urls = iface->albumsItems(albums);

    for (const QUrl& url : urls)
    {
        if (url.isLocalFile())
        {
            files << url.toLocalFile();
        }
    }

    files.removeDuplicates();

    return files;
}

} // namespace

ImageMosaicWallPlugin::ImageMosaicWallPlugin(QObject* const parent)
    : DPluginGeneric(parent)
{
    initImageMosaicWallResource();
}

ImageMosaicWallPlugin::~ImageMosaicWallPlugin()
{
}

QString ImageMosaicWallPlugin::name() const
{
    return tr("Image Mosaic Wall");
}

QString ImageMosaicWallPlugin::iid() const
{
    return QLatin1String(DPLUGIN_IID);
}

QIcon ImageMosaicWallPlugin::icon() const
{
    return QIcon(QLatin1String(":/icons/assets/imw-logo.png"));
}

QString ImageMosaicWallPlugin::description() const
{
    return tr("A tool to create an image based on a bunch of other images.");
}

QString ImageMosaicWallPlugin::details() const
{
    return tr("<p>This tool allows you to create an image based on a bunch of other images. "
              "It looks like a mosaic effect.</p>");
}

QList<DPluginAuthor> ImageMosaicWallPlugin::authors() const
{
    return QList<DPluginAuthor>()
            << DPluginAuthor(QString::fromUtf8("Tobias Scheck"),
                             QString::fromUtf8("tobias at scheck-media dot de"),
                             QString::fromUtf8("(C) 2018-2023"))
            << DPluginAuthor(QString::fromUtf8("Gilles Caulier"),
                             QString::fromUtf8("caulier dot gilles at gmail dot com"),
                             QString::fromUtf8("(C) 2020-2023"))
    ;
}

void ImageMosaicWallPlugin::setup(QObject* const parent)
{
    DPluginAction* const ac = new DPluginAction(parent);
    ac->setIcon(icon());
    ac->setText(tr("Image Mosaic Wall..."));
    ac->setObjectName(QLatin1String("ImageMosaicWall"));
    ac->setActionCategory(DPluginAction::GenericTool);

    connect(ac, SIGNAL(triggered(bool)),
            this, SLOT(slotImageMosaicWall()));

    addAction(ac);
}

void ImageMosaicWallPlugin::slotImageMosaicWall()
{
    DInfoInterface* const iface = infoIface(sender());
    QList<QUrl> images          = iface->currentSelectedItems();

    if (images.isEmpty())
    {
        images  = iface->currentAlbumItems();
    }

    if (!images.isEmpty())
    {
        QString topImage   = images.first().toLocalFile();
        qDebug() << "Using Top Image:" << topImage;
        QString currFolder = images.first().adjusted(QUrl::RemoveFilename).toLocalFile();
        qDebug() << "Using Current Folder:" << currFolder;
        MainWindow* const dlg = new MainWindow();
        dlg->setCurrentFolder(currFolder);
        dlg->onImageDropped(topImage);

        if (iface->supportAlbums())
        {
            // digiKam creates the album chooser once per interface and keeps a plain
            // pointer to it, so the dialog holding it must stay alive as long as the
            // window: it is created once here and only shown and hidden afterwards.

            QDialog* const chooser = new QDialog(dlg);
            chooser->setWindowTitle(tr("Tiles from digiKam"));

            QVBoxLayout* const layout     = new QVBoxLayout(chooser);
            QDialogButtonBox* const btns  = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, chooser);
            layout->addWidget(iface->albumChooser(chooser));
            layout->addWidget(btns);
            chooser->resize(500, 600);

            connect(btns, &QDialogButtonBox::accepted, chooser, &QDialog::accept);
            connect(btns, &QDialogButtonBox::rejected, chooser, &QDialog::reject);

            dlg->setTileSource(tr("Tiles from digiKam"), [iface, chooser]()
                {
                    QList<QString> files;

                    if (chooser->exec() != QDialog::Accepted)
                    {
                        return files;
                    }

                    // Albums, tags (including people), searches and labels; a photo can be
                    // in several of the selected ones. The choice is remembered for next time.
                    const QList<int> albums = iface->albumChooserItems();
                    rememberAlbums(albums);

                    return albumFiles(iface, albums);
                }
            );

            dlg->setRememberedTileSource(tr("Tiles from last selection"), [iface]()
                {
                    return albumFiles(iface, rememberedAlbums());
                }
            );
        }

        // Opt-in while it is tested: digiKam's thumbnails for the analysis of the photos.
        if (qEnvironmentVariableIntValue("IMAGEMOSAICWALL_DIGIKAM_THUMBNAILS") != 0)
        {
            dlg->setPhotoThumbnails(digiKamThumbnail);
        }

        dlg->show();
    }
}

QString ImageMosaicWallPlugin::handbookSection() const
{
    return QLatin1String("post_processing");
}

QString ImageMosaicWallPlugin::handbookChapter() const
{
    return QLatin1String("mosaicwall_tool");
}

} // namespace DigikamGenericImageMosaicWallPlugin
