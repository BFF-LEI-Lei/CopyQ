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

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ui_aboutdialog.h"
#include <QtGui/QDesktopWidget>
#include <QDebug>
#include <QDataStream>
#include <QFile>
#include <QDomDocument>
#include <QDomElement>
#include <QDomAttr>
#include <QSettings>
#include <QCloseEvent>
#include <QMenu>
#include <qtlocalpeer.h>
#include "clipboardmodel.h"
#include <iostream>


void Client::handleMessage(const QString &message)
{
    // empty message tells client to quit
    if (message.isEmpty())
        QApplication::exit();
    else {
        std::cout << message.toLocal8Bit().constData();
    }
}

MainWindow::MainWindow(const QString &css, QWidget *parent)
: QMainWindow(parent), ui(new Ui::MainWindow), aboutDialog(NULL)
{
    // global stylesheet
    setStyleSheet(css);

    ui->setupUi(this);

    ClipboardBrowser *c = ui->clipboardBrowser;
    c->readSettings(css);
    c->startMonitoring();

    // main window: icon & title
    this->setWindowTitle("CopyQ");
    m_icon = QIcon(":images/icon.svg");
    setWindowIcon(m_icon);

    // tray
    tray = new QSystemTrayIcon(this);
    tray->setIcon(m_icon);
    tray->setToolTip(
            tr("left click to show or hide, middle click to quit") );

    // menu
    QMenu *menu = new QMenu(this);
    QAction *act;
    // - show/hide
    act = new QAction( tr("&Show/Hide"), this );
    act->setWhatsThis( tr("Show or hide main window") );
    connect( act, SIGNAL(triggered()), this, SLOT(toggleVisible()) );
    menu->addAction(act);
    // - action dialog
    act = new QAction( tr("&Action..."), this );
    act->setWhatsThis( tr("Open action dialog") );
    connect( act, SIGNAL(triggered()), c, SLOT(openActionDialog()) );
    menu->addAction(act);
    // - exit
    act = new QAction( tr("E&xit..."), this );
    connect( act, SIGNAL(triggered()), this, SLOT(exit()) );
    menu->addAction(act);

    tray->setContextMenu(menu);

    // signals & slots
    connect( c, SIGNAL(requestSearch(QEvent*)),
            this, SLOT(enterSearchMode(QEvent*)) );
    connect( c, SIGNAL(hideSearch()),
            this, SLOT(enterBrowseMode()) );
    connect( c, SIGNAL(error(QString)),
            this, SLOT(showError(QString)) );
    connect( c, SIGNAL(message(QString,QString)),
            this, SLOT(showMessage(QString,QString)) );
    connect( c, SIGNAL(addMenuItem(QAction*)),
            this, SLOT(addMenuItem(QAction*)) );
    connect( c, SIGNAL(removeMenuItem(QAction*)),
            this, SLOT(removeMenuItem(QAction*)) );
    connect( tray, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(trayActivated(QSystemTrayIcon::ActivationReason)) );

    // settings
    readSettings();

    // browse mode by default
    m_browsemode = false;
    enterBrowseMode();

    tray->show();
}

void MainWindow::exit()
{
    close();
    QApplication::exit();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    showMinimized();
    hide();
    event->ignore();
}

void MainWindow::showMessage(const QString &title, const QString &msg,
                             QSystemTrayIcon::MessageIcon icon, int msec)
{
    tray->showMessage(title, msg, icon, msec);
}

void MainWindow::showError(const QString &msg)
{
    tray->showMessage(QString("Error"), msg, QSystemTrayIcon::Critical);
}

void MainWindow::addMenuItem(QAction *menuItem)
{
    tray->contextMenu()->addAction(menuItem);
}

void MainWindow::removeMenuItem(QAction *menuItem)
{
    tray->contextMenu()->removeAction(menuItem);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if ( event->modifiers() == Qt::ControlModifier )
        if ( event->key() == Qt::Key_Q )
            exit();

    switch( event->key() ) {
        case Qt::Key_Down:
        case Qt::Key_Up:
        case Qt::Key_PageDown:
        case Qt::Key_PageUp:
            ui->clipboardBrowser->keyEvent(event);
            break;
        
        case Qt::Key_Return:
        case Qt::Key_Enter:
            close();
            // move current item to clipboard and hide window
            ui->clipboardBrowser->moveToClipboard(
                    ui->clipboardBrowser->currentIndex() );
            resetStatus();
            break;

        // show about dialog
        case Qt::Key_F1:
            if ( !aboutDialog ) {
                aboutDialog = new QDialog(this);
                aboutDialog_ui = new Ui::AboutDialog;
                aboutDialog_ui->setupUi(aboutDialog);
            }
            aboutDialog->show();
            break;

        case Qt::Key_F3:
            // focus search bar
            enterBrowseMode(false);
            break;

        case Qt::Key_Escape:
            close();
            resetStatus();
            enterBrowseMode();
            break;

        default:
            QMainWindow::keyPressEvent(event);
            break;
    }
}

void MainWindow::resetStatus()
{
    ui->searchBar->clear();
    ui->clipboardBrowser->clearFilter();
    ui->clipboardBrowser->setCurrentIndex( QModelIndex() );
    ui->clipboardBrowser->scrollToTop();
}

void MainWindow::writeSettings()
{
    QSettings settings;

    settings.beginGroup("MainWindow");
    settings.setValue("size", size());
    settings.setValue("pos", pos());
    settings.endGroup();

    ui->clipboardBrowser->writeSettings();
    ui->clipboardBrowser->saveItems();
}

void MainWindow::readSettings()
{
    QSettings settings;

    settings.beginGroup("MainWindow");
    resize(settings.value("size", QSize(400, 400)).toSize());
    move(settings.value("pos", QPoint(200, 200)).toPoint());
    settings.endGroup();
}

void MainWindow::handleMessage(const QString& message)
{
    // deserialize list of arguments from QString
    QStringList args;
    QByteArray bytes(message.toAscii());
    bytes = QByteArray::fromBase64(bytes);
    QDataStream in(&bytes, QIODevice::ReadOnly);
    in >> args;

    const QString &client_id = "CopyQclient";
    const QString &cmd = args.isEmpty() ? QString() : args.takeFirst();

    // client
    QtLocalPeer peer(NULL,client_id);
    int t = 1000;

    ClipboardBrowser *c = ui->clipboardBrowser;

    // force check clipboard (update clipboard browser)
    c->checkClipboard();

    // show/hide main window
    if ( cmd == "toggle")
        toggleVisible();

    // exit server
    else if ( cmd == "exit") {
        // close client and exit
        peer.sendMessage(QString(),t);
        this->exit();
    }

    // show menu
    else if ( cmd == "menu" )
        tray->contextMenu()->show();

    else if ( cmd == "action" ) {
        // show action dialog
        if ( args.isEmpty() )
            c->openActionDialog(0);
        // action [row] "cmd" "[sep]"
        else {
            QString arg, cmd, sep;

            arg = args.takeFirst();

            // get row
            bool ok;
            int row = arg.toInt(&ok);
            if (ok) {
                if ( args.isEmpty() )
                    goto actionError;
                arg = args.takeFirst();
            }
            else
                row = 0;

            // get command
            cmd = arg;

            // get separator
            sep = args.isEmpty() ?
                  QString('\n') : args.takeFirst();

            if ( !args.isEmpty() )
                goto actionError;

            c->action(row, cmd, sep);
            return;

            actionError:
            showError("Bad \"action\" command syntax!\n"
                  "action [row] cmd [sep]");
        }
    }

    // add new item
    else if ( cmd == "add" )
        c->add(args.join( QString(' ') ));

    // edit clipboard item
    else if ( cmd == "edit" ) {
        c->setCurrent(0);
        c->openEditor();
    }

    // create new item and edit it
    else if ( cmd == "new" ) {
        c->add(args[1], false);
        c->setCurrent(0);
        c->openEditor();
    }

    // show clipboard content or custom message
    // show [title] [row=0]
    else if ( cmd == "show" ) {
        QString title, msg;

        if ( !args.isEmpty() )
            title = args.takeFirst();

        // get row
        bool ok = false;
        int row;
        if ( !args.isEmpty() )
            row = args.takeFirst().toInt(&ok);
        if ( !ok )
            row = 0;

        msg = c->itemText(row);
        if (msg.length()>500)
            msg = msg.left(500) + QString("\n\n\n< --- CROPPED --- >");
        showMessage( title, msg, QSystemTrayIcon::Information, 2000 );
    }

    // set current item
    // select [row=1]
    else if ( cmd == "select" ) {
        bool ok = false;
        int row;
        if ( !args.isEmpty() )
            row = args.takeFirst().toInt(&ok);
        if ( !ok )
            row = 0;
        c->moveToClipboard(row);
    }

    // remove item from clipboard
    // remove [row=0]
    else if ( cmd == "remove" ) {
        bool ok = false;
        int row;
        if ( !args.isEmpty() )
            row = args.takeFirst().toInt(&ok);
        if ( !ok )
            row = 0;
        c->setCurrent(row);
        c->remove();
    }

    else if ( cmd == "length" || cmd == "count" || cmd == "size" )
        peer.sendMessage( QString("%1\n").arg(c->length()), t );

    // print items in given rows, format can have two arguments %1:item %2:row
    // list [format="%1\n"|row=0]
    else if ( cmd == "list" ) {
        if ( args.isEmpty() )
            peer.sendMessage( c->itemText(0), t );
        else {
            int row;
            bool ok = false;
            QString fmt("%1\n");
            QString arg;
            do {
                arg = args.takeFirst();
                row = arg.toInt(&ok);

                if (ok) {
                    // number
                    arg = c->itemText(row);
                    if (arg.isEmpty())
                        arg = QString(' ');
                    peer.sendMessage(fmt.arg(arg).arg(row), t);
                }
                else {
                    // format
                    fmt = arg;
                    fmt.replace(QString("\\n"),QString('\n'));
                }
            } while( !args.isEmpty() );
        }
    }

    // TODO: move item

    else
        showError("Unknown command");

    // empty message tells client to quit
    peer.sendMessage(QString(),t);
}

void MainWindow::toggleVisible()
{
    if ( isVisible() )
        close();
    else {
        showNormal();
        activateWindow();
    }
}

void MainWindow::trayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if ( reason == QSystemTrayIcon::MiddleClick )
        exit();
    else if ( reason == QSystemTrayIcon::Trigger )
        toggleVisible();
}

void MainWindow::enterSearchMode(QEvent *event)
{
    enterBrowseMode(false);
    ui->searchBar->event(event);
    if ( ui->searchBar->text().isEmpty() )
        enterBrowseMode(true);
}

void MainWindow::enterBrowseMode(bool browsemode)
{
    QLineEdit *l = ui->searchBar;

    if (m_browsemode == browsemode) return;
    m_browsemode = browsemode;

    if(m_browsemode){
        // browse mode
        if ( l->text().isEmpty() )
            l->hide();
        ui->clipboardBrowser->setFocus();
    }
    else {
        // search mode
        l->show();
        l->setFocus(Qt::ShortcutFocusReason);
        l->selectAll();
    }
}

void MainWindow::center() {
    int x, y;
    int screenWidth, screenHeight;
    int width, height;
    QSize windowSize;

    QDesktopWidget *desktop = QApplication::desktop();

    width = frameGeometry().width();
    height = frameGeometry().height();

    screenWidth = desktop->width();
    screenHeight = desktop->height();

    x = (screenWidth - width) / 2;
    y = (screenHeight - height) / 2;

    move( x, y );
}

MainWindow::~MainWindow()
{
    writeSettings();
    delete ui;
}

void MainWindow::on_searchBar_textEdited(const QString &)
{
    timer_search.start(100,this);
}

void MainWindow::timerEvent(QTimerEvent *event)
{
    if ( event->timerId() == timer_search.timerId() ) {
        ui->clipboardBrowser->filterItems( ui->searchBar->text() );
        timer_search.stop();
    }
    else
        QMainWindow::timerEvent(event);
}
