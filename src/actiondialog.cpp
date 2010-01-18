#include <QProcess>
#include "actiondialog.h"
#include "ui_actiondialog.h"

static const QRegExp str("([^\\%])%s");

ActionDialog::ActionDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ActionDialog)
{
    ui->setupUi(this);
    ui->inputText->clear();
}

ActionDialog::~ActionDialog()
{
    delete ui;
}

void ActionDialog::changeEvent(QEvent *e)
{
    QDialog::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void ActionDialog::setInput(QString input)
{
    if ( input.isEmpty() ) {
        // no input
        ui->inputText->clear();
        ui->inputCheckBox->hide();
        ui->inputInfoLabel->hide();
        ui->inputText->hide();
    }
    else {
        ui->inputText->setText(input);
        ui->inputCheckBox->show();
        ui->inputInfoLabel->show();
        ui->inputText->show();
    }
}

void ActionDialog::accept()
{
    QString cmd = ui->cmdEdit->text();
    if ( cmd.isEmpty() )
        return;
    QStringList items;
    QString input = ui->inputText->text();
    QIODevice::OpenMode mode = QIODevice::NotOpen;
    QProcess proc;

    // replace %s with input
    if ( cmd.indexOf(str) != -1 ) {
        // if %s present and no input specified
        // TODO: warn user
        if ( input.isEmpty() )
            return;
        cmd.replace( str, "\\1"+input );
    }
    if ( !ui->inputCheckBox->isEnabled() )
        input.clear();

    // IO mode
    if ( !input.isEmpty() )
        mode |= QIODevice::WriteOnly;
    if ( ui->outputCheckBox->isChecked() )
        mode |= QIODevice::ReadOnly;

    // execute command (with input if needed)
    proc.start(cmd, mode);
    // TODO: warn user
    if ( !proc.waitForStarted() )
        return;

    // write input
    if (mode & QIODevice::WriteOnly) {
        // TODO: use toUtf8, toAscii or toLocal8Bit?
        proc.write( input.toLocal8Bit() );
    }
    proc.closeWriteChannel();

    // read output
    if (mode & QIODevice::ReadOnly) {
        QString output;

        while (proc.waitForReadyRead())
            // TODO: encoding
            output += proc.readAll();

        // separate items
        QRegExp sep( ui->separatorEdit->text() );
        if ( !sep.isEmpty() ) {
            items = output.split(sep);
        }
    }

    emit addItems(items);
    close();
}
