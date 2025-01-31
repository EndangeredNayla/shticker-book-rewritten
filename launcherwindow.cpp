/*
 * Copyright (c) 2015-2016 Joshua Snyder
 * Distributed under the GNU GPL v3. For full terms see the file LICENSE
 *
 * This file is part of Shticker Book Rewritten.
 *
 * Shticker Book Rewritten is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shticker Book Rewritten is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shticker Book Rewritten.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "launcherwindow.h"
#include "ui_launcherwindow.h"
#include "globaldefines.h"
#include "updateworker.h"
#include "filelocationchooser.h"

#include <QDir>
#include <QMessageBox>
#include <QCloseEvent>
#include <QProcess>
#include <QThread>
#include <QSettings>
#include <QEventLoop>

LauncherWindow::LauncherWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::LauncherWindow)
{
    ui->setupUi(this);
    connect(this, SIGNAL(enableUpdate(bool)), ui->updateButton, SLOT(setEnabled(bool)));
    connect(ui->updatesCheckBox, SIGNAL(toggled(bool)), this, SLOT(toggleAutoUpdates()));
    connect(ui->updateButton, SIGNAL(clicked(bool)), this, SLOT(updateFiles()));
    connect(ui->keepAliveCheckBox, SIGNAL(toggled(bool)), this, SLOT(toggleKeepAlive()));

    //read the previous settings
    readSettings();

    //check if the user has already chosen a file location for the game files
    while(filePath == "/")
    {
        setFilePath();
    }

    //setup saved toons
    ui->savedToonsBox->addItem("Saved logins");

    for(const LauncherUser &user : savedUsers)
    {
        ui->savedToonsBox->addItem(user.getUsername());
    }

    connect(ui->savedToonsBox, SIGNAL(currentIndexChanged(int)), this, SLOT(fillCredentials(int)));

    //setup the webviews
    ui->newsWebview->setUrl(QUrl("https://www.toontownrewritten.com/news/launcher"));
    ui->fishWebview->setUrl(QUrl("http://siggen.toontown-click.de/fishadvisor/en/fishes.html"));
    ui->groupsWebview->setUrl(QUrl("http://toonhq.org/groups/"));
    ui->bossRunsWebview->setUrl(QUrl("http://toonhq.org/ccg/"));
    ui->officesWebview->setUrl(QUrl("https://toonhq.org/field-offices/"));
    ui->sillyTeamsWebview->setUrl(QUrl("https://toon.plus/sillymeter"));

    //change news view to a dark background since text is white
    connect(ui->newsWebview->page(), SIGNAL(loadFinished(bool)), this, SLOT(newsViewLoaded()));

    //disable login until files are updated
    emit enableLogin(false);
    loginIsReady = false;

    if(autoUpdate)
    {
        updateFiles();
    }
    else
    {
        loginReady();
        ui->progressBar->hide();
    }
}

LauncherWindow::~LauncherWindow()
{
    delete ui;
}

void LauncherWindow::relayMessage(QString message)
{
    emit sendMessage(message);
}

void LauncherWindow::relayProgressBarReceived(int receivedBytes)
{
    emit sendProgressBarReceived(receivedBytes);
}

void LauncherWindow::relayShowProgressBar()
{
    emit showProgressBar();
}

void LauncherWindow::relayHideProgressBar()
{
    emit hideProgressBar();
}

void LauncherWindow::updatesReady()
{
    if(gameInstances.empty())
    {
        emit enableUpdate(true);
    }
}

void LauncherWindow::loginReady()
{
    emit enableLogin(true);
    loginIsReady = true;

    emit sendMessage("Logins are ready!");
}

void LauncherWindow::initiateLogin()
{
    if(loginIsReady)
    {
        qDebug() << "Initiating login sequence\n";

        //disable login again to prevent duplicate logins
        emit enableLogin(false);
        loginIsReady = false;

        loginWorker = new LoginWorker(this);

        connect(loginWorker, SIGNAL(sendMessage(QString)), this, SLOT(relayMessage(QString)));
        connect(loginWorker, SIGNAL(gameStarted(qint64)), this, SLOT(gameHasStarted(qint64)));
        connect(loginWorker, SIGNAL(gameFinished(int,qint64,QByteArray)), this, SLOT(gameHasFinished(int,qint64,QByteArray)));
        connect(loginWorker, SIGNAL(authenticationFailed()), this, SLOT(authenticationFailed()));

        //start login and then the game
        loginWorker->initiateLogin(ui->usernameBox->text(), ui->passwordBox->text(), ui->twofactorBox->text());
    }
    else
    {
        qDebug() << "Login isn't ready, ignoring login event\n";
    }
}

void LauncherWindow::gameHasStarted(qint64 processId)
{
    //disable updates while an instance is running
    emit enableUpdate(false);

    //check whether to save the credentials or not
    if(ui->saveCredentialsBox->isChecked())
    {
        QString username = ui->usernameBox->text();
        bool edited = false;

        // check to see if we can edit an existing user
        for(LauncherUser &user : savedUsers)
        {
            if(user.getUsername() != username)
            {
                continue;
            }

            user.setPassword(ui->passwordBox->text());
            user.setSecret(ui->twofactorBox->text().trimmed());
            edited = true;
            break;
        }

        // if not, add this user as a new one
        if(!edited)
        {
            savedUsers.append(LauncherUser(username, ui->passwordBox->text(), ui->twofactorBox->text().trimmed()));
            ui->savedToonsBox->addItem(username);
        }

        //uncheck the box now
        ui->saveCredentialsBox->setChecked(false);
        writeSettings();
    }

    //clear the username and password boxes to prevent accidental relaunching of the game and to be ready to launch another
    ui->usernameBox->clear();
    ui->passwordBox->clear();
    ui->twofactorBox->clear();
    ui->savedToonsBox->setCurrentIndex(0);

    //add process id to running instances
    gameInstances.push_back(processId);
    qDebug() << "New game instance, there are now" << gameInstances.length();

    //update keep alive
    updateKeepAliveTimer();

    //enable login again now that the game has finished starting
    loginReady();
}

void LauncherWindow::runKeepAlive() {
#if defined(Q_OS_WIN)
    windowsKeptAlive = 0;

    //begin enumerating all windows
    EnumWindows(keepAliveWindowReceived, (LPARAM) this);
#elif defined(Q_OS_LINUX)
    xdo_search_t search;
    Window *windows;
    unsigned int windowCount;

    memset(&search, 0, sizeof(xdo_search_t));
    search.max_depth = -1;
    search.require = xdo_search::SEARCH_ANY;
    search.searchmask = SEARCH_NAME;
    search.winname = "Toontown Rewritten";

    //look for all toontown windows
    if(xdo_search_windows(xdo, &search, &windows, &windowCount) != XDO_SUCCESS)
    {
        return;
    }

    //press end button for a split second on all toontown windows
    for(unsigned int i = 0; i < windowCount; ++i)
    {
        xdo_send_keysequence_window(xdo, windows[i], "End", 0);
    }

    //free memory allocated by xdo
    free(windows);
#elif defined(Q_OS_MAC)
    //use applescript to send an end key
    QStringList arguments;
    arguments << "-e" << "tell application \"Toontown Rewritten\" to key code 119";

    QProcess process;
    process.start("/usr/bin/osascript", arguments);
#endif
}

#if defined(Q_OS_WIN)
BOOL CALLBACK LauncherWindow::keepAliveWindowReceived(HWND handle, LPARAM lParam) {
    //get the process id of this window
    unsigned long process_id = 0;
    GetWindowThreadProcessId(handle, &process_id);

    LauncherWindow *window = (LauncherWindow *) lParam;

    if(!window->gameInstances.contains(process_id))
    {
        //game window not found, keep searching
        return true;
    }

    //process found
    PostMessage(handle, WM_KEYDOWN, VK_END, 0);
    PostMessage(handle, WM_KEYUP, VK_END, 0);

    //continue enumerating windows if not all windows are found
    return (++window->windowsKeptAlive) != window->gameInstances.size();
}
#endif

void LauncherWindow::gameHasFinished(int exitCode, qint64 processId, QByteArray gameOutput)
{
    //remove process id from running instances
    gameInstances.remove(processId);

    //update keep alive
    updateKeepAliveTimer();

    qDebug() << "Game instance has closed, there are now" << gameInstances.length() << "Exit code is:" << exitCode;

    if(exitCode != 0)
    {
        qDebug() << "TTR has crashed. Output from the engine is:" << gameOutput;
        emit sendMessage("Looks like Toontown Rewritten has crashed.");

        QMessageBox::warning(this, "Toontown Rewritten has crashed.",
                             "Looks like Toontown Rewritten has crashed. The engine's error message is:\n" + gameOutput,
                             QMessageBox::Ok);
    }

    //re-enable updates (checks to see if no other instances are running as well)
    updatesReady();
}

void LauncherWindow::authenticationFailed()
{
    emit enableLogin(true);
    loginIsReady = true;
}

//confirm closing of the launcher to alert of closing all running games
void LauncherWindow::closeEvent(QCloseEvent *event)
{
    //check if any game instances are running
    if(!gameInstances.empty())
    {
        //create a dialog box to confirm exit and warn about running instances
        QMessageBox::StandardButton dialog;
        dialog = QMessageBox::warning(this,
                                      "Please confirm closing.",
                                      "Are you sure you would like to close?  Closing the launcher when any game instance is running will cause it to close.  There are currently "
                                      + QString::number(gameInstances.length()) + " instances running.",
                                      QMessageBox::Yes | QMessageBox::No);

        if( dialog == QMessageBox::Yes)
        {
            //save the current settings before quitting
            writeSettings();
            exit(0);
        }
        else
        {
            event->ignore();
        }
    }

    //no running instances so we can just close
    else
    {
        //save the current settings before quitting
        writeSettings();

        exit(0);
    }
}

void LauncherWindow::newsViewLoaded()
{
    ui->newsWebview->page()->runJavaScript(QString("document.getElementsByTagName(\"ul\")[0].style.color = \"black\";document.getElementsByTagName(\"a\")[0].style.color = \"black\";document.getElementsByTagName(\"a\")[1].style.color = \"black\";document.getElementsByTagName(\"a\")[2].style.color = \"black\";document.getElementsByTagName(\"a\")[3].style.color = \"black\";document.getElementsByTagName(\"a\")[4].style.color = \"black\";document.getElementsByTagName(\"a\")[5].style.color = \"black\";document.getElementsByTagName(\"a\")[6].style.color = \"black\";document.getElementsByTagName(\"a\")[7].style.color = \"black\";document.getElementsByTagName(\"a\")[8].style.color = \"black\";document.getElementsByTagName(\"a\")[9].style.color = \"black\";document.getElementsByTagName(\"a\")[10].style.color = \"black\""));
}

void LauncherWindow::toggleAutoUpdates()
{
    autoUpdate = ui->updatesCheckBox->isChecked();
    writeSettings();
}

void LauncherWindow::toggleKeepAlive()
{
    keepAlive = ui->keepAliveCheckBox->isChecked();
    updateKeepAliveTimer();
    writeSettings();
}

void LauncherWindow::updateKeepAliveTimer()
{
    bool runKeepAlive = keepAlive && !gameInstances.empty();

#if defined(Q_OS_LINUX)
    if(runKeepAlive)
    {
        //create new xdo class to help iterate windows
        if(!xdo)
        {
            xdo = xdo_new(nullptr);
        }

        //keep alive can only run if we have created xdo successfully
        runKeepAlive = xdo != nullptr;
    }
#endif

    if(runKeepAlive)
    {
        //only create timer if not running already
        if(!keepAliveTimer)
        {
            qDebug() << "Starting keep alive...";
            keepAliveTimer = new QTimer(this);
            keepAliveTimer->setTimerType(Qt::VeryCoarseTimer);
            connect(keepAliveTimer, SIGNAL(timeout()), this, SLOT(runKeepAlive()));
            keepAliveTimer->start(60000);
        }
    }
    else
    {
        //only destroy timer if currently running
        if(keepAliveTimer)
        {
            qDebug() << "Stopping keep alive...";
            keepAliveTimer->stop();
            keepAliveTimer->deleteLater();
            keepAliveTimer = nullptr;
        }

#if defined(Q_OS_LINUX)
        //free xdo class since we no longer need it
        if(xdo)
        {
            xdo_free(xdo);
            xdo = nullptr;
        }
#endif
    }
}

void LauncherWindow::writeSettings()
{
    QSettings settings("Shticker-Book-Rewritten", "Shticker-Book-Rewritten");

    settings.beginGroup("LauncherWindow");
    settings.setValue("size", size());
    settings.setValue("pos", pos());
    settings.setValue("update", autoUpdate);
    settings.setValue("keepalive", keepAlive);
    settings.endGroup();

    settings.beginGroup("Logins");

    for(const LauncherUser &user : savedUsers)
    {
        settings.beginGroup(user.getUsername());
        settings.setValue("password", user.getPassword());
        settings.setValue("secret", user.getSecret());
        settings.endGroup();
    }

    //remove legacy users
    settings.remove("username");
    settings.remove("pass");

    settings.endGroup();
}

void LauncherWindow::readSettings()
{
    QSettings settings("Shticker-Book-Rewritten", "Shticker-Book-Rewritten");

    settings.beginGroup("LauncherWindow");
    resize(settings.value("size", QSize(400, 400)).toSize());
    move(settings.value("pos", QPoint(200, 200)).toPoint());
    autoUpdate = settings.value("update", true).toBool();
    keepAlive = settings.value("keepalive", false).toBool();
    settings.endGroup();

    savedUsers.clear();
    settings.beginGroup("Logins");

    //load all users from settings
    for(const QString &username : settings.childGroups())
    {
        settings.beginGroup(username);
        savedUsers.append(LauncherUser(username, settings.value("password").toString(), settings.value("secret").toString()));
        settings.endGroup();
    }

    //load legacy users from settings
    QStringList legacyUsernames = settings.value("username").toStringList();
    QStringList legacyPasses = settings.value("pass").toStringList();

    for(int i = 0; i < legacyUsernames.length() && i < legacyPasses.length(); ++i)
    {
        savedUsers.append(LauncherUser(legacyUsernames[i], legacyPasses[i], ""));
    }

    settings.endGroup();

    ui->updatesCheckBox->setChecked(autoUpdate);
    ui->keepAliveCheckBox->setChecked(keepAlive);

    readSettingsPath();
}

void LauncherWindow::readSettingsPath()
{
    QSettings settings("Shticker-Book-Rewritten", "Shticker-Book-Rewritten");

    settings.beginGroup("FilesPath");
    filePath = settings.value("path").toString();
    settings.endGroup();

    filePath = filePath + QString("/");
    cachePath = filePath + QString(".cache/");
}

void LauncherWindow::fillCredentials(int index) {
    if(index == 0)
    {
        ui->usernameBox->clear();
        ui->passwordBox->clear();
        ui->twofactorBox->clear();
        return;
    }

    LauncherUser user = savedUsers.at(index - 1);
    ui->usernameBox->setText(user.getUsername());
    ui->passwordBox->setText(user.getPassword());
    ui->twofactorBox->setText(user.getSecret());
}

void LauncherWindow::updateFiles()
{
    ui->progressBar->show();

    emit enableUpdate(false);

    //check to make sure the cache directory exists and make it if it doesn't
    if(!QDir(filePath).exists())
    {
        QDir().mkdir(filePath);
    }
    if(!QDir(cachePath).exists())
    {
        QDir().mkdir(cachePath);
    }

    //Begin updating the game files
    updateThread = new QThread(this);
    UpdateWorker *updateWorker = new UpdateWorker();
    //make a new thread for the updating process since it can bog down the main thread and make the window unresponsive
    updateWorker->moveToThread(updateThread);

    connect(updateThread, SIGNAL(started()), updateWorker, SLOT(startUpdating()));
    connect(updateWorker, SIGNAL(updateComplete()), updateThread, SLOT(quit()));

    //allow the update worker to communicate with the main window
    connect(updateWorker, SIGNAL(sendMessage(QString)), this, SLOT(relayMessage(QString)));
    connect(updateWorker, SIGNAL(sendProgressBarReceived(int)), this, SLOT(relayProgressBarReceived(int)));
    connect(updateWorker, SIGNAL(showProgressBar()), this, SLOT(relayShowProgressBar()));
    connect(updateWorker, SIGNAL(hideProgressBar()), this, SLOT(relayHideProgressBar()));
    connect(updateWorker, SIGNAL(updateComplete()), this, SLOT(loginReady()));
    connect(updateWorker, SIGNAL(updateComplete()), this, SLOT(updatesReady()));

    updateThread->start();
}

void LauncherWindow::changeFilePath()
{
    //disable login until files are updated
    emit enableLogin(false);
    loginIsReady = false;

    setFilePath();
    updateFiles();
}

void LauncherWindow::setFilePath()
{
    FileLocationChooser *chooser = new FileLocationChooser;
    chooser->show();
    chooser->activateWindow();

    //wait until a path is chosen
    QEventLoop waitForPath;
    connect(chooser, SIGNAL(finished()), &waitForPath, SLOT(quit()));
    connect(chooser, SIGNAL(rejected()), &waitForPath, SLOT(quit()));
    waitForPath.exec();

    chooser->deleteLater();

    readSettingsPath();
}
