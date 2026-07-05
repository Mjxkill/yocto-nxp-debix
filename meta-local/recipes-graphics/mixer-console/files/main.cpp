/* V10-NATIVE N0 — mixer-console : squelette Qt6/QML eglfs (sans wayland).
 * Objectif N0 : fenêtre plein écran DRM/KMS tournée, châssis, compteur FPS
 * réel (frameSwapped) → mesure CPU GO/NO-GO avant toute suite. */
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QTimer>
#include <QFile>
#include <QEvent>
#include <QPointerEvent>
#include <QDebug>
#include "mixerclient.h"
#include "calibration.h"

/* diag tactile (blocage au changement de page) : trace CHAQUE événement
 * pointeur brut reçu par la fenêtre — permet de distinguer « événement
 * jamais arrivé » (driver/eglfs) de « arrivé mais pas délivré » (grab). */
class TouchLogger : public QObject {
public:
    CalibrationHelper *calib = nullptr;
    bool eventFilter(QObject *obj, QEvent *ev) override {
        switch (ev->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchEnd:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: {
            auto *pe = static_cast<QPointerEvent *>(ev);
            const QPointF p = pe->points().isEmpty()
                              ? QPointF() : pe->points().first().position();
            QString ids;
            for (const auto &pt : pe->points())
                ids += QString::number(pt.id()) + ":"
                       + QString::number(int(pt.state())) + " ";
            qWarning() << "TOUCH-EV" << ev->type()
                       << int(p.x()) << int(p.y())
                       << "npts" << pe->points().size() << ids;
            if (calib && ev->type() == QEvent::TouchBegin)
                calib->feedTouch(p.x(), p.y());
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }
};

class FpsMeter : public QObject {
    Q_OBJECT
    Q_PROPERTY(int fps READ fps NOTIFY fpsChanged)
public:
    explicit FpsMeter(QObject *parent = nullptr) : QObject(parent) {
        m_clock.start();
        m_tick.setInterval(1000);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            m_fps = m_frames;
            m_frames = 0;
            emit fpsChanged();
        });
        m_tick.start();
    }
    int fps() const { return m_fps; }
    void attach(QQuickWindow *w) {
        connect(w, &QQuickWindow::frameSwapped, this,
                [this] { ++m_frames; }, Qt::DirectConnection);
        /* V10-N6d — luminosité : le modeset de CETTE app re-initialise le
         * panneau (retour à son défaut 12/255) APRÈS toutes les écritures
         * udev/ExecStartPre. La seule séquence sûre : écrire une fois la
         * première frame affichée. */
        connect(w, &QQuickWindow::frameSwapped, this, [this] {
            if (m_blDone)
                return;
            m_blDone = true;
            QTimer::singleShot(300, this, [] {
                QFile f(QStringLiteral(
                    "/sys/class/backlight/32e60000.mipi_dsi.0/brightness"));
                if (f.open(QIODevice::WriteOnly))
                    f.write("255");
            });
        });
    }
signals:
    void fpsChanged();
private:
    QElapsedTimer m_clock;
    QTimer m_tick;
    bool m_blDone = false;
    int m_frames = 0;
    int m_fps = 0;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    FpsMeter fps;
    MixerClient mixer;
    CalibrationHelper calibHelper;
    mixer.setEngine(&engine);
    engine.rootContext()->setContextProperty("fpsMeter", &fps);
    engine.rootContext()->setContextProperty("mixer", &mixer);
    engine.rootContext()->setContextProperty("calib", &calibHelper);
    engine.load(QUrl(QStringLiteral("qrc:/MixerConsole/main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    TouchLogger tlog;
    tlog.calib = &calibHelper;
    if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        fps.attach(w);
        w->installEventFilter(&tlog);
    }
    return app.exec();
}

#include "main.moc"
