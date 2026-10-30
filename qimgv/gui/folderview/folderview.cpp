#include "folderview.h"
#include "ui_folderview.h"
#include "utils/virtualfs.h"

#include <algorithm>

namespace {
QStandardItem *findItemByPath(QStandardItem *item, const QString &path) {
    if(!item)
        return nullptr;
    if(item->data(Qt::UserRole + 1).toString() == path)
        return item;
    for(int i = 0; i < item->rowCount(); ++i) {
        if(QStandardItem *found = findItemByPath(item->child(i), path))
            return found;
    }
    return nullptr;
}
} // namespace

FolderView::FolderView(QWidget *parent) :
    FloatingWidgetContainer(parent),
    ui(new Ui::FolderView)
{
    ui->setupUi(this);

    // ------- filesystem view --------
    QString style = "font: %1pt;";
    style = style.arg(QApplication::font().pointSize());
    ui->dirTreeView->setStyleSheet(style);

    optionsPopup = new FVOptionsPopup();
    popupTimerClutch.start();

    dirModel = new FileSystemModelCustom(this);
    dirModel->setFilter(QDir::NoDotAndDotDot | QDir::AllDirs);
    ui->dirTreeView->setModel(dirModel);

    archiveModel = new QStandardItemModel(this);

    setupFilesystemTree();

#ifdef _WIN32
    dirModel->setRootPath("");
#else
    dirModel->setRootPath(QDir::homePath());
    QModelIndex idx = dirModel->index(dirModel->rootPath());
    ui->dirTreeView->setRootIndex(idx);
#endif
    // -------------------------------
    ui->upButton->setAction("goUp");
    ui->upButton->setIconPath(":res/icons/common/buttons/panel/up16.png");
    ui->upButton->setTriggerMode(TriggerMode::ClickTrigger);
    ui->settingsButton->setAction("openSettings");
    ui->settingsButton->setIconPath(":res/icons/common/buttons/panel/settings16.png");
    ui->exitButton->setAction("exit");
    ui->exitButton->setIconPath(":res/icons/common/buttons/panel/close16.png");
    ui->exitButton->setIconOffset(-1, 0);
    ui->docViewButton->setAction("documentView");
    ui->docViewButton->setIconPath(":res/icons/common/buttons/panel/document-view20.png");
    ui->togglePlacesPanelButton->setCheckable(true);
    ui->togglePlacesPanelButton->setIconPath(":res/icons/common/buttons/panel/toggle-panel20.png");
    ui->togglePlacesPanelButton->setIconOffset(1, 0);

    ui->optionsPopupButton->setCheckable(true);
    ui->optionsPopupButton->setIconPath(":res/icons/common/buttons/panel/folderview20.png");

    ui->sortingComboBox->setIconPath(":res/icons/common/other/sorting-mode16.png");

    ui->newBookmarkButton->setIconPath(":res/icons/common/buttons/panel-small/add-new12.png");
    ui->homeButton->setIconPath(":res/icons/common/buttons/panel-small/home12.png");
    ui->rootButton->setIconPath(":res/icons/common/buttons/panel-small/root12.png");

    int min = ui->thumbnailGrid->THUMBNAIL_SIZE_MIN;
    int max = ui->thumbnailGrid->THUMBNAIL_SIZE_MAX;
    int step = ui->thumbnailGrid->ZOOM_STEP;

    ui->zoomSlider->setMinimum(min / step);
    ui->zoomSlider->setMaximum(max / step);
    ui->zoomSlider->setSingleStep(1);
    ui->zoomSlider->setPageStep(1);

    ui->splitter->setStretchFactor(1, 50);

    connect(ui->thumbnailGrid, &FolderGridView::thumbnailsRequested,  this, &FolderView::thumbnailsRequested);
    connect(ui->thumbnailGrid, &FolderGridView::thumbnailSizeChanged, this, &FolderView::onThumbnailSizeChanged);
    connect(ui->thumbnailGrid, &FolderGridView::itemActivated,   this, &FolderView::itemActivated);
    connect(ui->thumbnailGrid, &FolderGridView::draggedOut,      this, &FolderView::draggedOut);
    connect(ui->thumbnailGrid, &FolderGridView::draggedOver,     this, &FolderView::draggedOver);
    connect(ui->thumbnailGrid, &FolderGridView::droppedInto,     this, &FolderView::droppedInto);

    connect(ui->bookmarksWidget, &BookmarksWidget::bookmarkClicked, this, &FolderView::onBookmarkClicked);

    connect(ui->newBookmarkButton, &IconButton::clicked, this, &FolderView::newBookmark);
    connect(ui->homeButton, &IconButton::clicked, this, &FolderView::onHomeBtn);
    connect(ui->rootButton, &IconButton::clicked, this, &FolderView::onRootBtn);

    connect(ui->zoomSlider, &QSlider::valueChanged, this, &FolderView::onZoomSliderValueChanged);
    connect(ui->sortingComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this, &FolderView::onSortingSelected);
    connect(ui->togglePlacesPanelButton, &ActionButton::toggled, this, &FolderView::onPlacesPanelButtonChecked);

    connect(ui->optionsPopupButton, &IconButton::toggled, this, &FolderView::onOptionsPopupButtonToggled);
    connect(optionsPopup, &FVOptionsPopup::dismissed, this, &FolderView::onOptionsPopupDismissed);
    connect(optionsPopup, &FVOptionsPopup::viewModeSelected, this, &FolderView::onViewModeSelected);

    connect(ui->dirTreeView, &TreeViewCustom::droppedIn, this, &FolderView::onDroppedInByIndex);
    connect(ui->dirTreeView, &TreeViewCustom::tabbedOut, this, &FolderView::onTreeViewTabOut);
    connect(ui->bookmarksWidget, &BookmarksWidget::droppedIn, this, &FolderView::moveUrlsRequested); // ask what to do via popup? copy or move

    ui->sortingComboBox->setItemDelegate(new QStyledItemDelegate(ui->sortingComboBox));
    ui->sortingComboBox->view()->setTextElideMode(Qt::ElideNone);

    connect(ui->splitter, &QSplitter::splitterMoved, this, &FolderView::onSplitterMoved);

    readSettings();

    QSizePolicy sp_retain = sizePolicy();
    sp_retain.setRetainSizeWhenHidden(true);
    setSizePolicy(sp_retain);
    connect(settings, &Settings::settingsChanged, this, &FolderView::readSettings);

    hide();
}

void FolderView::readSettings() {
    ui->thumbnailGrid->setThumbnailSize(settings->folderViewIconSize());
    ui->thumbnailGrid->setShowLabels((settings->folderViewMode() != FV_SIMPLE));
    ui->togglePlacesPanelButton->setChecked(settings->placesPanel());

    setPlacesPanel(settings->placesPanel());
    ui->bookmarksWidget->setVisible(settings->placesPanelBookmarksExpanded());
    ui->dirTreeView->setVisible(settings->placesPanelTreeExpanded());

    QList<int> sizes;
    sizes << settings->placesPanelWidth() << 1;
    ui->splitter->setSizes(sizes);
}

void FolderView::onSplitterMoved() {
    settings->setPlacesPanelWidth(ui->placesPanel->width());
}

void FolderView::onPlacesPanelButtonChecked(bool mode) {
    setPlacesPanel(mode);
    settings->setPlacesPanel(mode);
}

void FolderView::setPlacesPanel(bool mode) {
    if(width() >= 600)
        ui->placesPanel->setVisible(mode);
}

void FolderView::toggleBookmarks() {
    if(ui->bookmarksWidget->isVisible())
        ui->bookmarksWidget->hide();
    else
        ui->bookmarksWidget->show();
    settings->setPlacesPanelBookmarksExpanded(ui->bookmarksWidget->isVisible());
}

void FolderView::toggleFilesystemView() {
    if(ui->dirTreeView->isVisible())
        ui->dirTreeView->hide();
    else
        ui->dirTreeView->show();
    settings->setPlacesPanelTreeExpanded(ui->dirTreeView->isVisible());
}

void FolderView::onTreeViewTabOut() {
    ui->thumbnailGrid->setFocus();
    // TODO: maybe add a focus change indication? a border blink or something
}

// TODO: ask what to do
void FolderView::onDroppedInByIndex(QList<QString> paths, QModelIndex index) {
    // archive entries are read-only
    if(index.model() != dirModel)
        return;
    emit moveUrlsRequested(paths, dirModel->filePath(index));
}

void FolderView::onOptionsPopupButtonToggled(bool mode) {
    if(mode) {
        // Fixes popup being shown again right after dismissing it
        // by clicking on this toggle button.
        // This issue is only present on windows (different Qt::Popup behavior)
        if(popupTimerClutch.elapsed() < 10) {
            ui->optionsPopupButton->setChecked(false);
            return;
        }
        QPoint pos = ui->optionsPopupButton->geometry().bottomRight() -
                     QPoint(optionsPopup->width(), 0);
        optionsPopup->showAt(mapToGlobal(pos));
    }
}

void FolderView::onOptionsPopupDismissed() {
    popupTimerClutch.start();
    ui->optionsPopupButton->setChecked(false);
}

void FolderView::onViewModeSelected(FolderViewMode mode) {
    settings->setFolderViewMode(mode);
    ui->thumbnailGrid->setShowLabels((settings->folderViewMode() != FV_SIMPLE));
    emit showFoldersChanged((mode == FV_EXT_FOLDERS));
}

void FolderView::onThumbnailSizeChanged(int newSize) {
    ui->zoomSlider->setValue(newSize / ui->thumbnailGrid->ZOOM_STEP);
    settings->setFolderViewIconSize(newSize);
}

void FolderView::onZoomSliderValueChanged(int value) {
    ui->thumbnailGrid->setThumbnailSize(value * ui->thumbnailGrid->ZOOM_STEP);
}

// changed by user via combobox
void FolderView::onSortingSelected(int mode) {
    emit sortingSelected(static_cast<SortingMode>(mode));
}

void FolderView::onSortingChanged(SortingMode mode) {
    ui->sortingComboBox->blockSignals(true);
    ui->sortingComboBox->setCurrentIndex(static_cast<int>(mode));
    ui->sortingComboBox->blockSignals(false);
}

FolderView::~FolderView() {
    delete ui;
}

// probably unneeded
void FolderView::show() {
    QWidget::show();
    ui->thumbnailGrid->setFocus();
}

// probably unneeded
void FolderView::hide() {
    QWidget::hide();
    ui->thumbnailGrid->clearFocus();
}

void FolderView::onFullscreenModeChanged(bool mode) {
    ui->exitButton->setHidden(!mode);
    if(mode) // hide 2px spacer
        ui->panelRightEdgeSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
    else // show spacer
        ui->panelRightEdgeSpacer->changeSize(2, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
    ui->topBar->layout()->invalidate();
}

void FolderView::focusInEvent(QFocusEvent *event) {
    Q_UNUSED(event)
    ui->thumbnailGrid->setFocus();
}

void FolderView::populate(int count) {
    ui->thumbnailGrid->populate(count);
}

void FolderView::setThumbnail(int pos, std::shared_ptr<Thumbnail> thumb) {
    ui->thumbnailGrid->setThumbnail(pos, thumb);
}

void FolderView::select(QList<int> indices) {
    ui->thumbnailGrid->select(indices);
}

void FolderView::select(int index) {
    ui->thumbnailGrid->select(index);
}

QList<int> FolderView::selection() {
    return ui->thumbnailGrid->selection();
}

void FolderView::focusOn(int index) {
    ui->thumbnailGrid->focusOn(index);
}

void FolderView::focusOnSelection() {
    ui->thumbnailGrid->focusOnSelection();
}

void FolderView::onHomeBtn() {
    emit directorySelected(QDir::homePath());
}

void FolderView::onRootBtn() {
    emit directorySelected("/");
}

void FolderView::setDirectoryPath(QString path) {
    const bool archiveDir = VirtualFs::isVirtual(path) && VirtualFs::isDirectory(path);

#ifdef __linux
    if(!archiveDir) {
        if(path.startsWith(QDir::homePath())) {
            if(dirModel->rootPath() != QDir::homePath()) {
                dirModel->setRootPath(QDir::homePath());
                QModelIndex idx = dirModel->index(dirModel->rootPath());
                ui->dirTreeView->setRootIndex(idx);
            }
        } else {
            dirModel->setRootPath("/");
            QModelIndex idx = dirModel->index(dirModel->rootPath());
            ui->dirTreeView->setRootIndex(idx);
        }
    }
#endif

    if(archiveDir) {
        QString archivePath;
        QString innerPath;
        VirtualFs::split(path, archivePath, innerPath);
        ui->pathLabel->setText(VirtualFs::fileName(archivePath) +
                               (innerPath.isEmpty() ? QString() : QStringLiteral(" ▸ ") + innerPath));
        ui->bookmarksWidget->onPathChanged(path);
        refreshArchiveTree(path);
        return;
    }

    ui->pathLabel->setText(path);
    restoreFilesystemTree();

    if(ui->dirTreeView->currentIndex().data() == path)
        return;

    ui->bookmarksWidget->onPathChanged(path);

    QModelIndex targetIndex = dirModel->index(path);
    bool keepExpand = ui->dirTreeView->isExpanded(targetIndex);
    bool collapse = !ui->dirTreeView->isExpanded(ui->dirTreeView->currentIndex().parent());

    if(collapse)
        ui->dirTreeView->collapseAll();
    ui->dirTreeView->setCurrentIndex(targetIndex);

    if(keepExpand)
        ui->dirTreeView->expand(targetIndex);

    // ok, i'm done with this shit. none of the "solutions" work
    // just do scrollTo after a delay and hope that model is loaded by then
    // larger than ~150ms becomes too noticeable
    QTimer::singleShot(150, this, &FolderView::fsTreeScrollToCurrent);
}

// Archives live outside the real filesystem, so their tree is generated on the fly.
void FolderView::refreshArchiveTree(const QString &path) {
    QString archivePath;
    QString innerPath;
    if(!VirtualFs::split(path, archivePath, innerPath))
        return;

    // A nested archive ("a.zip!b.zip") keeps its place in the outer tree; the
    // tree is only rebuilt when a different top level archive is opened.
    QString topArchive = archivePath;
    QString topInner;
    VirtualFs::split(archivePath, topArchive, topInner);
    const QString rootPath = VirtualFs::join(topArchive, QString());

    if(archiveTreeRoot != rootPath) {
        archiveTreeRoot = rootPath;
        archiveModel->clear();
        archiveModel->setHorizontalHeaderLabels({tr("Name")});

        auto *rootItem = new QStandardItem(QFileInfo(topArchive).fileName());
        rootItem->setData(rootPath, Qt::UserRole + 1);
        rootItem->setIcon(iconProvider.icon(QFileIconProvider::Folder));
        rootItem->setToolTip(topArchive);
        // archives are read-only: double click must not open the rename editor
        rootItem->setFlags(rootItem->flags() & ~Qt::ItemIsEditable);
        archiveModel->appendRow(rootItem);
        fillArchiveDir(rootItem, topArchive, QString(), 0);

        ui->dirTreeView->setModel(archiveModel);
    }

    QString nodePath = path;
    while(nodePath.endsWith('!'))
        nodePath.chop(1);
    QStandardItem *target = findArchiveItem(nodePath);
    if(!target && nodePath == topArchive)
        target = archiveModel->item(0);
    if(!target)
        return;

    // archives inside archives are filled in when they are first opened
    if(target->rowCount() == 0 && target->parent() != nullptr) {
        QString nestedArchive;
        QString nestedInner;
        if(VirtualFs::split(nodePath, nestedArchive, nestedInner))
            fillArchiveDir(target, nestedArchive, nestedInner, 1);
    }

    const QModelIndex targetIndex = target->index();
    if(ui->dirTreeView->currentIndex() == targetIndex)
        return;
    ui->dirTreeView->setCurrentIndex(targetIndex);
    for(QModelIndex parent = targetIndex.parent(); parent.isValid(); parent = parent.parent())
        ui->dirTreeView->expand(parent);
    ui->dirTreeView->expand(targetIndex);
    ui->dirTreeView->scrollTo(targetIndex);
}

void FolderView::fillArchiveDir(QStandardItem *parentItem, const QString &archivePath,
                                const QString &innerPath, int depth) {
    if(depth >= 8 || parentItem->rowCount() > 2000)
        return;

    std::vector<VirtualFs::Entry> entries;
    if(!VirtualFs::listDir(VirtualFs::join(archivePath, innerPath), entries, false))
        return;

    std::sort(entries.begin(), entries.end(), [](const VirtualFs::Entry &a, const VirtualFs::Entry &b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });

    for(const VirtualFs::Entry &entry : entries) {
        if(!entry.isDir)
            continue;
        auto *item = new QStandardItem(entry.name);
        item->setData(entry.path, Qt::UserRole + 1);
        item->setIcon(iconProvider.icon(QFileIconProvider::Folder));
        // archives are read-only: double click must not open the rename editor
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        parentItem->appendRow(item);

        // nested archives are expanded on demand: unpacking all of them up
        // front would be very slow on big game packages
        if(VirtualFs::isArchivePath(entry.path))
            continue;

        QString childArchive;
        QString childInner;
        if(VirtualFs::split(entry.path, childArchive, childInner))
            fillArchiveDir(item, childArchive, childInner, depth + 1);
        if(parentItem->rowCount() > 2000)
            break;
    }
}

QStandardItem *FolderView::findArchiveItem(const QString &path) const {
    for(int i = 0; i < archiveModel->rowCount(); ++i) {
        if(QStandardItem *found = findItemByPath(archiveModel->item(i), path))
            return found;
    }
    return nullptr;
}

// QHeaderView drops hidden sections whenever the model changes, so the
// "name only" column layout has to be re-applied after every swap.
void FolderView::setupFilesystemTree() {
    QHeaderView* header = ui->dirTreeView->header();
    header->setStretchLastSection(false);
    header->hideSection(1); // size
    header->hideSection(2); // type
    header->hideSection(3); // mod date
    header->setSectionResizeMode(0, QHeaderView::Stretch);
}

void FolderView::restoreFilesystemTree() {
    if(ui->dirTreeView->model() == dirModel)
        return;
    archiveTreeRoot.clear();
    ui->dirTreeView->setModel(dirModel);
    setupFilesystemTree();
#ifdef _WIN32
    // Re-setting the root would reload the model and drop the expanded state.
    if(!dirModel->rootPath().isEmpty())
        dirModel->setRootPath(QString());
#else
    ui->dirTreeView->setRootIndex(dirModel->index(dirModel->rootPath()));
#endif
}

void FolderView::fsTreeScrollToCurrent() {
    ui->dirTreeView->scrollTo(ui->dirTreeView->currentIndex());
}

void FolderView::onTreeViewClicked(QModelIndex index) {
    if(!index.isValid())
        return;
    if(index.model() == dirModel) {
        emit directorySelected(dirModel->fileInfo(index).absoluteFilePath());
        return;
    }
    const QString virtualPath = index.data(Qt::UserRole + 1).toString();
    if(!virtualPath.isEmpty())
        emit directorySelected(virtualPath);
}

void FolderView::onBookmarkClicked(QString dirPath) {
    emit directorySelected(dirPath);
}

void FolderView::newBookmark() {
    QFileDialog dialog;
    dialog.setDirectory(QDir::homePath());
    dialog.setWindowTitle("Select directory");
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly);
    dialog.setOption(QFileDialog::DontResolveSymlinks);
    connect(&dialog, &QFileDialog::fileSelected, ui->bookmarksWidget, &BookmarksWidget::addBookmark);
    dialog.exec();
}

void FolderView::addItem() {
    ui->thumbnailGrid->addItem();
}

void FolderView::insertItem(int index) {
    ui->thumbnailGrid->insertItem(index);
}

void FolderView::removeItem(int index) {
    ui->thumbnailGrid->removeItem(index);
}

void FolderView::reloadItem(int index) {
    ui->thumbnailGrid->reloadItem(index);
}

void FolderView::setDragHover(int index) {
    ui->thumbnailGrid->setDragHover(index);
}

// prevent passthrough to parent
void FolderView::wheelEvent(QWheelEvent *event) {
    event->accept();
}

void FolderView::paintEvent(QPaintEvent *) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void FolderView::resizeEvent(QResizeEvent *event) {
    Q_UNUSED(event)
    if(width() < 600)
        ui->placesPanel->setVisible(false);
    else if (ui->togglePlacesPanelButton->isChecked())
        ui->placesPanel->setVisible(true);

    if(width() < 510) {
        ui->zoomSlider->setVisible(false);
        ui->zoomSliderSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        ui->pathbarSpacer->changeSize(0, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        ui->topBar->layout()->invalidate();
    } else {
        ui->zoomSlider->setVisible(true);
        ui->zoomSliderSpacer->changeSize(3, 20, QSizePolicy::Fixed, QSizePolicy::Fixed);
        ui->pathbarSpacer->changeSize(12, 20, QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        ui->topBar->layout()->invalidate();
    }
}
