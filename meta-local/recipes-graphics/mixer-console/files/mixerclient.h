/* V10-NATIVE N1 — MixerClient : client socket Unix direct de mixer-pro.
 * Protocole JSON ligne-par-ligne existant (/run/mixer-pro.sock) :
 * meters 30 Hz, stat 1 Hz, commandes fire-and-forget.
 * Les réponses arrivent dans l'ordre des requêtes → simple FIFO de tags. */
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QTimer>
#include <QVariantList>
#include <QJSValue>
#include <QQueue>

class QQmlEngine;

class MixerClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QVariantList inLevels READ inLevels NOTIFY metersChanged)
    Q_PROPERTY(QVariantList outLevels READ outLevels NOTIFY metersChanged)
    Q_PROPERTY(int xrun READ xrun NOTIFY statChanged)
    Q_PROPERTY(double latencyMs READ latencyMs NOTIFY statChanged)
    Q_PROPERTY(QString version READ version NOTIFY statChanged)
    Q_PROPERTY(QVariantList spectrum READ spectrum NOTIFY spectrumChanged)
    Q_PROPERTY(QVariantList mlEnvL READ mlEnvL NOTIFY insertChanged)
    Q_PROPERTY(QVariantList mlEnvR READ mlEnvR NOTIFY insertChanged)
    Q_PROPERTY(bool mlActive READ mlActive NOTIFY insertChanged)
    /* V10-N8 : page active — les pollers meters/analyzer/insert ne
     * tournent que quand une page les affiche (sinon ~45 req/s inutiles
     * qui réveillent le control thread de mixer-pro sur les cores audio) */
    Q_PROPERTY(int activePage READ activePage WRITE setActivePage NOTIFY activePageChanged)

public:
    explicit MixerClient(QObject *parent = nullptr);

    int activePage() const { return m_activePage; }
    void setActivePage(int p) {
        if (m_activePage == p) return;
        m_activePage = p;
        emit activePageChanged();
    }

    bool connected() const { return m_connected; }
    QVariantList inLevels() const { return m_in; }
    QVariantList outLevels() const { return m_out; }
    int xrun() const { return m_xrun; }
    double latencyMs() const { return m_latencyMs; }
    QString version() const { return m_version; }
    QVariantList spectrum() const { return m_spectrum; }
    QVariantList mlEnvL() const { return m_mlEnvL; }
    QVariantList mlEnvR() const { return m_mlEnvR; }
    bool mlActive() const { return m_mlActive; }

    /* commandes console — mêmes ops que la GUI web */
    Q_INVOKABLE void setMaster(int src, int out, double gainDb);
    Q_INVOKABLE void setInputGain(int src, double gainDb);
    Q_INVOKABLE void setMute(int src, bool mute);
    Q_INVOKABLE void setOutputGain(int out, double db);
    Q_INVOKABLE void setSend(int in, int bus, double gainDb);
    /* canal générique : n'importe quel op JSON du protocole mixer-pro,
     * callback JS appelé avec l'objet réponse (pages ROUTING/EFFETS/…) */
    Q_INVOKABLE void call(const QVariantMap &op, const QJSValue &cb);
    void setEngine(QQmlEngine *e) { m_engine = e; }

signals:
    void connectedChanged();
    void metersChanged();
    void statChanged();
    void spectrumChanged();
    void insertChanged();
    void activePageChanged();
    /* tick d'affichage coalescé 22 Hz (½ vsync 44) : le QML ne met à jour
     * la scène QU'ICI → 1 rendu par tick au lieu de 45 rendus/s irréguliers
     * (42 % des cycles = driver Vivante PAR frame, mesuré perf) */
    void uiTick();

private:
    enum Tag { TagMeters, TagStat, TagAnalyzer, TagInsert, TagGeneric, TagIgnore };

    void connectSocket();
    void request(const QByteArray &json, Tag tag);
    void command(const QJsonObject &obj);
    void onReadyRead();
    void handleLine(const QByteArray &line, Tag tag);
    static double dbNorm(double peak);   /* peak s32 → 0..1 (-60..0 dB) */

    QQmlEngine *m_engine = nullptr;
    QQueue<QJSValue> m_cbs;
    QLocalSocket m_sock;
    QByteArray m_buf;
    QList<Tag> m_pending;
    QTimer m_meterTimer;
    QTimer m_statTimer;
    QTimer m_analyzerTimer;
    QTimer m_insertTimer;
    QTimer m_uiTick;
    QTimer m_reconnect;

    bool m_connected = false;
    QVariantList m_in, m_out;
    int m_activePage = 0;
    int m_xrun = 0;
    double m_latencyMs = 0;
    QString m_version;
    QVariantList m_spectrum;      /* 64 bins 0..1 */
    bool m_dirty = false;
    QVariantList m_mlEnvL, m_mlEnvR;   /* 64 gains dB */
    bool m_mlActive = false;
};
