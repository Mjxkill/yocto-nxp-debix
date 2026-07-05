/* V10-NATIVE N0 — mixer-console : squelette Qt6/QML eglfs (sans wayland).
 * Objectif N0 : fenêtre plein écran DRM/KMS tournée, châssis, compteur FPS
 * réel (frameSwapped) → mesure CPU GO/NO-GO avant toute suite. */
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QTimer>

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
    }
signals:
    void fpsChanged();
private:
    QElapsedTimer m_clock;
    QTimer m_tick;
    int m_frames = 0;
    int m_fps = 0;
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;
    FpsMeter fps;
    engine.rootContext()->setContextProperty("fpsMeter", &fps);
    engine.load(QUrl(QStringLiteral("qrc:/MixerConsole/main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first()))
        fps.attach(w);
    return app.exec();
}

#include "main.moc"
