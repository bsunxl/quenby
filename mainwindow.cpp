/***
  Copyright (c) 2017 Nepos GmbH

  Authors: Daniel Mack <daniel@nepos.io>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
***/

#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QQuickItem>
#include <QQuickWidget>
#include <QVBoxLayout>
#include <QApplication>
#include <QTemporaryFile>
#include <QTimer>

#include "mainwindow.h"
#include "webenginepage.h"

MainWindow::MainWindow(QUrl mainViewUrl, int mainViewWidth, int mainViewHeight, QWidget *parent) :
    QMainWindow(parent),
    frame(new QWidget(this)),
    browserWidget(new QWidget(this)),
    windowLayout(new QVBoxLayout(this)),
    quickWidget(new QQuickWidget(this)),
    inputPanel(nullptr),
    controlChannel(),
    controlInterface(),
    views()
{
    setCentralWidget(frame);
    frame->setLayout(windowLayout);

    setAttribute(Qt::WA_TranslucentBackground);

    quickWidget->setFocusPolicy(Qt::NoFocus);
    quickWidget->setSource(QUrl("qrc:/inputpanel.qml"));
    quickWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    quickWidget->setVisible(false);

    windowLayout->addWidget(browserWidget);
    windowLayout->addWidget(quickWidget);
    windowLayout->setSpacing(0);
    windowLayout->setMargin(0);

    inputPanel = quickWidget->rootObject();
    if (inputPanel)
        inputPanel->setProperty("width", size().width());

    connect(inputPanel, SIGNAL(activated(bool)), this, SLOT(onActiveChanged(bool)));
    connect(inputPanel, SIGNAL(heightChanged(int)), this, SLOT(onHeightChanged(int)));

    createControlInterface();

    auto key = nextKey();
    auto *view = addWebView(key);
    auto *page = view->page();
    view->page()->setWebChannel(&controlChannel);
    view->setUrl(mainViewUrl);
    view->setGeometry(0, 0, mainViewWidth, mainViewHeight);
    view->setAutoFillBackground(false);

    QObject::connect(page, &WebEnginePage::featurePermissionRequested, [this, page](const QUrl &securityOrigin, WebEnginePage::Feature feature) {
        enum WebEnginePage::PermissionPolicy verdict =
                (securityOrigin.host() == "localhost") ?
                            WebEnginePage::PermissionGrantedByUser :
                            WebEnginePage::PermissionDeniedByUser;

                qInfo() << "WebEnginePage::featurePermissionRequested: " << feature << "verdict " << verdict;
                page->setFeaturePermission(securityOrigin, feature, verdict);
    });

    QObject::connect(view, &QWebEngineView::titleChanged, [this](const QString &title) {
        setWindowTitle(title);
    });
}

MainWindow::~MainWindow()
{
    if (quickWidget)
        delete quickWidget;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    if (inputPanel)
        inputPanel->setProperty("width", event->size().width());
}

void MainWindow::onActiveChanged(bool a)
{
    quickWidget->setVisible(a);
}

void MainWindow::onWidthChanged(int w)
{
    qDebug() << __PRETTY_FUNCTION__ << " :" << w;
}

void MainWindow::onHeightChanged(int h)
{
    auto newSize = quickWidget->size();
    newSize.setHeight(h);
    quickWidget->resize(newSize);

    qDebug() << __PRETTY_FUNCTION__ << " :" << h;
}

void MainWindow::createControlInterface()
{
    QObject::connect(&controlInterface, &ControlInterface::onCreateWebViewRequested, [this]() {
        auto key = nextKey();
        QWebEngineView *view = addWebView(key);

        QObject::connect(view, &QWebEngineView::urlChanged, [this, key](const QUrl &url) {
            emit controlInterface.onWebViewURLChanged(key, url.url());
        });

        QObject::connect(view, &QWebEngineView::titleChanged, [this, key](const QString &title) {
            emit controlInterface.onWebViewTitleChanged(key, title);
        });

        QObject::connect(view, &QWebEngineView::loadProgress, [this, key](int progress) {
            emit controlInterface.onWebViewLoadProgressChanged(key, progress);
        });

        return key;
    });

    QObject::connect(&controlInterface, &ControlInterface::onDestroyWebViewRequested, [this](uint64_t key) {
        QWebEngineView *view = lookupWebView(key);
        if (view) {
            view->setVisible(false);
            views.remove(key);
            delete view;
        }
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewURLChangeRequested, [this](uint64_t key, const QString &url) {
        QWebEngineView *view = lookupWebView(key);
        if (view)
            view->setUrl(QUrl(url));
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewGeometryChangeRequested, [this](uint64_t key, int x, int y, int w, int h) {
        QWebEngineView *view = lookupWebView(key);
        if (view)
            view->setGeometry(x, y, w, h);
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewVisibleChangeRequested, [this](uint64_t key, bool value) {
        QWebEngineView *view = lookupWebView(key);
        if (view)
            view->setVisible(value);
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewTransparentBackgroundChangeRequested, [this](uint64_t key, bool value) {

        QWebEngineView *view = lookupWebView(key);
        if (view) {
            if (value) {
                view->setAutoFillBackground(false);
                view->page()->setBackgroundColor(Qt::transparent);
            } else {
                view->setAutoFillBackground(true);
                view->page()->setBackgroundColor(Qt::white);
            }
        }
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewStackUnder, [this](uint64_t topKey, uint64_t underKey) {

        auto wTop = lookupWebView(topKey);
        auto wUnder = lookupWebView(underKey);
        if (wTop && wUnder)
            wTop->stackUnder(wUnder);
    });

    QObject::connect(&controlInterface, &ControlInterface::onWebViewStackOnTop, [this](uint64_t key) {

        auto w = lookupWebView(key);
        if (w)
            w->raise();
    });


    controlChannel.registerObject(QStringLiteral("main"), &controlInterface);
}

uint64_t MainWindow::nextKey()
{
    static uint64_t nextKey = 0;
    return nextKey++;
}


QWebEngineView *MainWindow::addWebView(uint64_t key)
{
    QWebEngineView *view = new QWebEngineView(browserWidget);

    view->setAutoFillBackground(true);

    WebEnginePage *page = new WebEnginePage(QWebEngineProfile::defaultProfile(), view);
    view->setPage(page);

    views.insert(key, view);

    return view;
}

QWebEngineView *MainWindow::lookupWebView(uint64_t key)
{
    if (views.contains(key))
        return views.value(key);

    return Q_NULLPTR;
}

QWebEngineView *MainWindow::lookupVisibleWebView()
{
    for (int i=0; i<views.size(); i++)
        if (views[i]->isVisible())
            return views[i];

    return Q_NULLPTR;
}
