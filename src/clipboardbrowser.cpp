/*
    Copyright (c) 2009, Lukas Holecek <hluk@email.cz>

    This file is part of CopyQ.

    CopyQ is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    CopyQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with CopyQ.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "clipboardbrowser.h"
#include <QDebug>
#include <QKeyEvent>
#include <QApplication>
#ifndef WIN32
#include <X11/extensions/XInput.h>
#endif
#include <unistd.h> //usleep
#include <actiondialog.h>
#include "itemdelegate.h"
#include "clipboardmodel.h"

ClipboardBrowser::ClipboardBrowser(QWidget *parent) : QListView(parent),
    m_msec(1000), actionDialog(NULL)
{
    // delegate for rendering and editing items
    d = new ItemDelegate(this);
    setItemDelegate(d);

    // set new model
    m = new ClipboardModel();
    QItemSelectionModel *old_model = selectionModel();
    setModel(m);
    delete old_model;

    connect( m, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
             d, SLOT(dataChanged(QModelIndex,QModelIndex)) );
    connect( m, SIGNAL(rowsRemoved(QModelIndex,int,int)),
             d, SLOT(rowsRemoved(QModelIndex,int,int)) );
    connect( m, SIGNAL(rowsInserted(QModelIndex, int, int)),
             d, SLOT(rowsInserted(QModelIndex, int, int)) );
    connect( m, SIGNAL(rowsMoved(QModelIndex, int, int, QModelIndex, int)),
             d, SLOT(rowsMoved(QModelIndex, int, int, QModelIndex, int)) );

    connect( this, SIGNAL(doubleClicked(QModelIndex)),
            SLOT(moveToClipboard(QModelIndex)));

    // ScrollPerItem doesn't work well with hidden items
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    setAttribute(Qt::WA_MacShowFocusRect, 0);
}

void ClipboardBrowser::startMonitoring()
{
    // slot/signals method to monitor
    // X11 clipboard is unreliable:
    // - X11 will notify app only if it is
    //   current or previous owner of clipboard
    // - getting clipboard contents from GTK apps
    //   is slow in some cases
    timer.start(m_msec, this);
}

void ClipboardBrowser::stopMonitoring()
{
    timer.stop();
}

ClipboardBrowser::~ClipboardBrowser()
{
    emit closeAllEditors();
}

void ClipboardBrowser::closeEditor(QEditor *editor)
{
    // check if file was modified before closing
    if ( editor->fileModified() )
        itemModified( editor->getHash(), editor->getText() );

    editor->disconnect(this);
    disconnect(editor);
    delete editor;
}

void ClipboardBrowser::openEditor()
{
    QEditor *editor = new QEditor(itemText(), m_editor);

    connect( editor, SIGNAL(fileModified(uint, const QString &)),
            this, SLOT(itemModified(uint, const QString &)) );

    connect( editor, SIGNAL(closed(QEditor *)),
            this, SLOT(closeEditor(QEditor *)) );

    connect( this, SIGNAL(closeAllEditors()), editor, SLOT(close()) );

    if ( !editor->start() )
        closeEditor(editor);
}

void ClipboardBrowser::itemModified(uint hash, const QString &str)
{
    // find item with qHash == hash and remove it
    for(int i = 0; i < m->rowCount(); ++i) {
        if ( qHash(itemText(i)) == hash ) {
            m->removeRow(i);
            break;
        }
    }

    // add new item
    if ( !str.isEmpty() ) {
        add(str);
        sync();
    }
}

void ClipboardBrowser::filterItems(const QString &str)
{
    // if search string empty: all items visible
    if ( str.isEmpty() ) {
        m->setSearch();
        emit hideSearch();
    }
    else {
        // if search string is a number N: highlight Nth item
        bool ok;
        int n = str.toInt(&ok);
        if (ok) {
            m->setSearch();
            setCurrent(n);
            return;
        }
        else {
            QRegExp re(str);
            re.setCaseSensitivity(Qt::CaseInsensitive);
            m->setSearch(&re);
        }
    }

    // hide filtered items
    reset();
    int first = -1;
    for(int i = 0; i < m->rowCount(); ++i) {
        if ( m->isFiltered(i) )
            setRowHidden(i,true);
        else if (first == -1)
                first = i;
    }
    // select first visible
    setCurrentIndex( index(first) );
}

void ClipboardBrowser::moveToClipboard(const QModelIndex &ind)
{
    if ( ind.isValid() )
        moveToClipboard(ind.row());
}

void ClipboardBrowser::moveToClipboard(const QString &txt)
{
    // TODO: remove identical; How to iterate through all items?
    // insert new
    if ( m->insertRow(0) ) {
        m->setData( index(0), txt );
        sync();
    }
}

void ClipboardBrowser::moveToClipboard(int i)
{
    if ( i > 0 ) {
        m->move(i,0);
        sync();
        scrollTo( currentIndex() );
        repaint();
    }
}

void ClipboardBrowser::checkClipboard()
{
    const QClipboard *clipboard = QApplication::clipboard();
    if ( clipboard->text() != m_lastSelection )
        clipboardChanged();
    else if ( clipboard->text(QClipboard::Selection) != m_lastSelection )
        clipboardChanged(QClipboard::Selection);
}

void ClipboardBrowser::timerEvent(QTimerEvent *event)
{
    if ( event->timerId() == timer.timerId() )
        checkClipboard();
    else if ( event->timerId() == timer_save.timerId() ) {
        saveItems();
        timer_save.stop();
    }
    else
        QListView::timerEvent(event);
}

void ClipboardBrowser::createActionDialog()
{
    if (!actionDialog) {
        actionDialog = new ActionDialog(this);
        actionDialog->setAttribute(Qt::WA_QuitOnClose, false);

        connect( actionDialog, SIGNAL(addItems(QStringList)),
                 this, SLOT(addItems(QStringList)) );
        connect( actionDialog, SIGNAL(error(QString)),
                 this, SIGNAL(error(QString)) );
        connect( actionDialog, SIGNAL(message(QString,QString)),
                 this, SIGNAL(message(QString,QString)) );
        connect( actionDialog, SIGNAL(addMenuItem(QAction*)),
                 this, SIGNAL(addMenuItem(QAction*)) );
        connect( actionDialog, SIGNAL(removeMenuItem(QAction*)),
                 this, SIGNAL(removeMenuItem(QAction*)) );
    }
}

void ClipboardBrowser::keyPressEvent(QKeyEvent *event)
{
    QModelIndex ind;

    if ( state() == QAbstractItemView::EditingState ) {
        QListView::keyPressEvent(event);
        return;
    }

    // CTRL
    if ( event->modifiers() == Qt::ControlModifier ) {
        QModelIndexList list;

        int key = event->key();
        switch ( key ) {
        // CTRL-E: open external editor
        case Qt::Key_E:
            openEditor();
            break;

        // CTRL-N: create new item
        case Qt::Key_N:
            // new text will allocate more space (lines) for editor
            add( QString("---NEW---\n\n\n\n\n\n\n\n---NEW---"), false );
            selectionModel()->clearSelection();
            setCurrent(0);
            edit( index(0) );
            break;

        // CTRL-Up/Down: move items
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_End:
        case Qt::Key_Home:
            if ( m->moveItems(selectedIndexes(), key) )
                sync();
            scrollTo( currentIndex() );
            repaint();
            break;
        default:
            QListView::keyPressEvent(event);
            break;
        }
    }
    else {
        switch ( event->key() ) {
        case Qt::Key_Delete:
            remove();
            break;

        // navigation
        case Qt::Key_Up:
            if ( selectedIndexes().isEmpty() )
                setCurrent(-1, true);
            else
                setCurrent( currentIndex().row()-1, true,
                            event->modifiers() == Qt::ShiftModifier );
            break;
        case Qt::Key_Down:
            if ( selectedIndexes().isEmpty() )
                setCurrent(0);
            else
                setCurrent( currentIndex().row()+1, true,
                            event->modifiers() == Qt::ShiftModifier );
            break;
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_Home:
        case Qt::Key_End:
        case Qt::Key_Escape:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QListView::keyPressEvent(event);
            break;

        // F2: edit
        case Qt::Key_F2:
            QListView::keyPressEvent(event);
            break;

        // F5: action
        case Qt::Key_F5:
            openActionDialog();
            break;

        default:
            emit requestSearch(event);
            break;
        }
    }
}

void ClipboardBrowser::action(int row, const QString &cmd,
                              const QString &sep, bool input, bool output)
{
    createActionDialog();

    if (input)
        actionDialog->setInput( row >= 0 ? itemText(row) : selectedText() );
    actionDialog->setCommand(cmd);
    actionDialog->setSeparator(sep);
    actionDialog->setInput(input);
    actionDialog->setOutput(output);
    actionDialog->accept();
}

void ClipboardBrowser::openActionDialog(int row, bool modal)
{
    createActionDialog();
    actionDialog->setInput( row >= 0 ? itemText(row) : selectedText() );
    if (modal)
        actionDialog->exec();
    else
        actionDialog->show();
}

void ClipboardBrowser::dataChanged(const QModelIndex &first, const QModelIndex &last)
{
    if ( first.row() == 0 )
        sync();
    QListView::dataChanged(first, last);
}

void ClipboardBrowser::setCurrent(int row, bool cycle, bool selection)
{
    // direction
    int cur = currentIndex().row();
    int dir = cur < row ? 1 : -1;

    // select first visible
    int i = m->getRowNumber(row, cycle);
    cur = i;
    while ( isRowHidden(i) ) {
        i = m->getRowNumber(i+dir, cycle);
        if ( (!cycle && (i==0 || i==m->rowCount()-1)) || i == cur)
            break;
    }

    QModelIndex ind = index(i);
    if (selection) {
        QItemSelectionModel *sel = selectionModel();
        if ( sel->isSelected(ind) && sel->isSelected(currentIndex()) )
            sel->setCurrentIndex(currentIndex(),
                QItemSelectionModel::Deselect);
        sel->setCurrentIndex(ind,
            QItemSelectionModel::Select);
    }
    else
        setCurrentIndex(ind);

    scrollTo(ind); // ensure visible
}

void ClipboardBrowser::remove()
{
    QItemSelectionModel *sel = selectionModel();
    QModelIndexList list = sel->selectedIndexes();

    if ( !list.isEmpty() ) {
        int i;
        bool need_sync = false;

        do {
            i = list.first().row();
            if ( i == 0 )
                need_sync = true;
            m->removeRow(i);
            list = sel->selectedIndexes();
        } while( !list.isEmpty() );

        // select next
        setCurrent(i);

        if (need_sync)
            sync();
    }
}

bool ClipboardBrowser::add(const QImage &image)
{
    m->insertRow(0);
    QModelIndex ind = index(0);
    m->setData(ind, image);
    return true;
}

bool ClipboardBrowser::add(const QString &txt, bool ignore_empty)
{
    // ignore empty
    if ( ignore_empty && txt.indexOf( QRegExp("^\\s*$") ) != -1 )
        return false;

    // find same item
    int i = 0;
    for(; i < m->rowCount(); i++) {
        if( txt == itemText(i) )
           break;
    }
    // found identical item - remove
    if ( i < m->rowCount() ) {
        // nothing to do
        if ( i == 0 )
            return true;
        else
            m->removeRow(i);
    }

    // create new item
    m->insertRow(0);
    QModelIndex ind = index(0);
    m->setData(ind, txt);

    // filter item
    if ( m->isFiltered(0) )
        setRowHidden(0,true);

    // list size limit
    if ( m->rowCount() > m_maxitems )
        m->removeRow( m->rowCount() - 1 );

    // save
    if ( !timer_save.isActive() )
        timer_save.start(30000, this);
    
    return true;
}

bool ClipboardBrowser::add(const QVariant &value)
{
    if( value.type() == QVariant::Image )
        return add( value.value<QImage>() );
    else
        return add( value.toString() );
}

void ClipboardBrowser::addItems(const QStringList &items) {
    QStringList::const_iterator it( items.end()-1 );
    for(; it != items.begin()-1; --it)
        add(*it);
}

void ClipboardBrowser::readSettings(const QString &css)
{
    QSettings settings;

    setStyleSheet(css);
    d->setStyleSheet(css);

    // restore configuration
    m_msec = settings.value("inteval", 1000).toInt();
    m_maxitems = settings.value("maxitems", 400).toInt();
    m_editor = settings.value("editor", "gvim -f %1").toString();
    d->setItemFormat( settings.value("format",
        "<div class=\"item\"><div class=\"number\">%1</div><div class=\"text\">%2</div></div>"
        ).toString()
    );

    // restore items
    m->clear();
    m->setMaxItems( settings.value("maxitems", 400).toInt() );

    QFile file( dataFilename() );
    file.open(QIODevice::ReadOnly);
    QDataStream in(&file);
    QVariant v;
    while( !in.atEnd() ) {
        in >> v;
        if ( v.type() == QVariant::Image )
            add( v.value<QImage>() );
        else
            add( v.toString() );
    }

    // performance:
    // force the delegate to calculate the size of each item
    // -- this will save some time when drawing the list for the fist time
    sizeHintForColumn(0);

    timer_save.stop();

    sync(false);
}

void ClipboardBrowser::writeSettings()
{
    QSettings settings;

    settings.setValue( "interval", m_msec );
    settings.setValue( "maxitems", m_maxitems );
    settings.setValue( "editor", m_editor );
    settings.setValue( "format", d->itemFormat() );
}

const QString ClipboardBrowser::dataFilename() const
{
    // do not use registry in windows
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QCoreApplication::organizationName(),
                       QCoreApplication::applicationName());
    // ini -> dat
    QString datfilename = settings.fileName();
    datfilename.replace( QRegExp("ini$"),QString("dat") );

    return datfilename;
}

void ClipboardBrowser::saveItems()
{
    QFile file( dataFilename() );
    file.open(QIODevice::WriteOnly);
    QDataStream out(&file);

    // save in reverse order so the items
    // can be later restored in right order
    for(int i = m->rowCount()-1; i >= 0; --i)
        out << itemData(i);
}

const QString ClipboardBrowser::selectedText() const
{
    QString result;
    QItemSelectionModel *sel = selectionModel();

    QModelIndexList list = sel->selectedIndexes();
    foreach(QModelIndex ind, list) {
        if ( !result.isEmpty() )
            result += QString('\n');
        result += itemText(ind);
    }

    if ( result.isEmpty() )
        result = itemText(0);

    return result;
}

QString ClipboardBrowser::itemText(int i) const
{
    if ( i >= m->rowCount() )
        return QString();
    return itemText( (i==-1) ? currentIndex() : index(i) );
}

QString ClipboardBrowser::itemText(QModelIndex ind) const
{
    return ind.isValid() ? ind.data(Qt::EditRole).toString() : QString();
}

void ClipboardBrowser::clipboardChanged(QClipboard::Mode mode)
{
    #ifndef WIN32
    if ( mode == QClipboard::Selection )
    {
        // don't handle selections in own window
        if ( hasFocus() ) return;

        Display *dsp = XOpenDisplay( NULL );
        Window root = DefaultRootWindow(dsp);
        XEvent event;

        // wait while mouse button, shift or ctrl keys are pressed
        // (i.e. selection is incomplete)
        while(true) {
            XQueryPointer(dsp, root,
                    &event.xbutton.root, &event.xbutton.window,
                    &event.xbutton.x_root, &event.xbutton.y_root,
                    &event.xbutton.x, &event.xbutton.y,
                    &event.xbutton.state);
            if( !(event.xbutton.state &
                  (Button1Mask | ShiftMask | ControlMask)) )
                break;
            usleep(m_msec/2);
        };
        
        XCloseDisplay(dsp);
    }
    #endif

    sync(false, mode);
}

void ClipboardBrowser::sync(bool list_to_clipboard, QClipboard::Mode mode)
{
    // synchronize data/text
    QVariant data;
    QString text;
    QClipboard *clip = QApplication::clipboard();

    stopMonitoring();
    // first item -> clipboard
    if (list_to_clipboard) {
        if ( m->rowCount() == 0 )
            return;
        data = itemData(0);

        // sync clipboard
        if ( data.type() == QVariant::Image ) {
            clip->setImage( data.value<QImage>() );
            clip->setImage( data.value<QImage>(), QClipboard::Selection );
            m_lastSelection.clear();
        }
        else {
            text = data.toString();
            if ( text != clip->text() )
                clip->setText(text);
            if ( text != clip->text(QClipboard::Selection) )
                clip->setText(text,QClipboard::Selection);
            m_lastSelection = text;
        }
    }
    // clipboard -> first item
    else {
        text = clip->text(mode);
        if ( !text.isEmpty() ) {
            text = clip->text(mode);
            QString current = itemText(0);
            if( text.isEmpty() )
                text = current;
            else if( text != current )
                add(text);

            clip->setText(text);
            // set selection only if it's different
            // - this avoids clearing selection in
            //   e.g. terminal apps
            if ( text != clip->text(QClipboard::Selection) )
                clip->setText(text, QClipboard::Selection);
            m_lastSelection = text;
        }
        else {
            const QMimeData *mime = clip->mimeData(mode);
            if (mime) {
                if ( mime->formats().isEmpty() )
                    clip->setText(text, mode);
                else if ( mime->hasImage() ) {
                    data = clip->image(mode);
                    if( data != itemData(0) )
                        add(data);
                }
            }
        }
    }
    startMonitoring();
}
