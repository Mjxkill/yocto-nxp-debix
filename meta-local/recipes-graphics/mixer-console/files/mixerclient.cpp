#include "mixerclient.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtMath>

static const char *SOCK_PATH = "/run/mixer-pro.sock";

MixerClient::MixerClient(QObject *parent) : QObject(parent)
{
    connect(&m_sock, &QLocalSocket::readyRead, this, &MixerClient::onReadyRead);
    connect(&m_sock, &QLocalSocket::connected, this, [this] {
        m_connected = true;
        m_pending.clear();
        m_buf.clear();
        emit connectedChanged();
        /* tap analyzer 3 = sorties master 0/1 (spectre post-mastering),
         * idempotent — la GUI web pose le même */
        command({{"op", "set_tap"}, {"tap", 3}, {"kind", 3}, {"a", 0}, {"b", 1}});
    });
    connect(&m_sock, &QLocalSocket::disconnected, this, [this] {
        m_connected = false;
        emit connectedChanged();
        m_reconnect.start();
    });
    connect(&m_sock, &QLocalSocket::errorOccurred, this, [this](auto) {
        if (!m_connected)
            m_reconnect.start();
    });

    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(1500);
    connect(&m_reconnect, &QTimer::timeout, this, &MixerClient::connectSocket);

    /* meters 30 Hz — skip si le socket a du retard (pas d'empilement) */
    m_meterTimer.setInterval(33);
    connect(&m_meterTimer, &QTimer::timeout, this, [this] {
        if (m_connected && m_pending.size() < 3)
            request("{\"op\":\"get_meters_lite\"}\n", TagMeters);
    });
    m_meterTimer.start();

    m_statTimer.setInterval(1000);
    connect(&m_statTimer, &QTimer::timeout, this, [this] {
        if (m_connected && m_pending.size() < 3)
            request("{\"op\":\"get_state\"}\n", TagStat);
    });
    m_statTimer.start();

    /* spectre : get_meters COMPLET (payload analyzer) à 10 Hz seulement */
    m_analyzerTimer.setInterval(100);
    connect(&m_analyzerTimer, &QTimer::timeout, this, [this] {
        if (m_connected && m_pending.size() < 3)
            request("{\"op\":\"get_meters\"}\n", TagAnalyzer);
    });
    m_analyzerTimer.start();

    /* enveloppe ML (insert chain slot 0) à 5 Hz */
    m_insertTimer.setInterval(200);
    connect(&m_insertTimer, &QTimer::timeout, this, [this] {
        if (m_connected && m_pending.size() < 3)
            request("{\"op\":\"get_insert\"}\n", TagInsert);
    });
    m_insertTimer.start();

    /* tick d'affichage : coalesce meters/spectre/ML en UN rendu à 22 Hz */
    m_uiTick.setInterval(45);
    connect(&m_uiTick, &QTimer::timeout, this, [this] {
        if (m_dirty) {
            m_dirty = false;
            emit uiTick();
        }
    });
    m_uiTick.start();

    connectSocket();
}

void MixerClient::connectSocket()
{
    if (m_sock.state() != QLocalSocket::UnconnectedState)
        return;
    m_sock.connectToServer(QString::fromLatin1(SOCK_PATH));
}

void MixerClient::request(const QByteArray &json, Tag tag)
{
    m_pending.append(tag);
    m_sock.write(json);
}

void MixerClient::command(const QJsonObject &obj)
{
    if (!m_connected)
        return;
    request(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n", TagIgnore);
}

void MixerClient::onReadyRead()
{
    m_buf += m_sock.readAll();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        const Tag tag = m_pending.isEmpty() ? TagIgnore : m_pending.takeFirst();
        if (tag != TagIgnore && !line.isEmpty())
            handleLine(line, tag);
    }
}

double MixerClient::dbNorm(double peak)
{
    if (peak <= 0)
        return 0.0;
    const double db = 20.0 * std::log10(peak / 2147483648.0);
    return qBound(0.0, (db + 60.0) / 60.0, 1.0);
}

void MixerClient::handleLine(const QByteArray &line, Tag tag)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return;
    const QJsonObject o = doc.object();

    if (tag == TagMeters) {
        /* Ballistique ICI (attack ~30 ms, release ~110 ms à pas de 33 ms) :
         * le QML reçoit des valeurs déjà lissées → aucune animation continue
         * côté scenegraph (les Behaviors 30 Hz coûtaient ~40 % CPU). */
        static constexpr double K_ATK = 0.67, K_REL = 0.26;
        const QJsonArray in = o.value(QLatin1String("in")).toArray();
        const QJsonArray out = o.value(QLatin1String("out")).toArray();
        auto smooth = [](QVariantList &cur, const QJsonArray &raw) {
            if (cur.size() != raw.size()) {
                cur.clear();
                for (int i = 0; i < raw.size(); ++i)
                    cur.append(0.0);
            }
            for (int i = 0; i < raw.size(); ++i) {
                const double tgt = dbNorm(raw.at(i).toDouble());
                const double prev = cur.at(i).toDouble();
                const double k = tgt > prev ? K_ATK : K_REL;
                cur[i] = prev + (tgt - prev) * k;
            }
        };
        smooth(m_in, in);
        smooth(m_out, out);
        m_dirty = true;
        emit metersChanged();
    } else if (tag == TagAnalyzer) {
        /* analyzer[3].s = 128 bins int8 dB → 64 bins 0..1 (max de paires) */
        const QJsonArray taps = o.value(QLatin1String("analyzer")).toArray();
        if (taps.size() > 3) {
            const QJsonArray sp = taps.at(3).toObject()
                                      .value(QLatin1String("s")).toArray();
            if (sp.size() >= 128) {
                if (m_spectrum.size() != 64) {
                    m_spectrum.clear();
                    for (int i = 0; i < 64; ++i)
                        m_spectrum.append(0.0);
                }
                for (int i = 0; i < 64; ++i) {
                    const double a = sp.at(i * 2).toDouble();
                    const double b = sp.at(i * 2 + 1).toDouble();
                    const double db = qMax(a, b);
                    m_spectrum[i] = qBound(0.0, (db + 90.0) / 90.0, 1.0);
                }
                m_dirty = true;
                emit spectrumChanged();
            }
        }
    } else if (tag == TagInsert) {
        /* chain[0] spectral_env : l[64]/r[64] gains dB (enveloppe NPU) */
        const QJsonArray chain = o.value(QLatin1String("chain")).toArray();
        bool act = o.value(QLatin1String("active")).toBool();
        QVariantList l, r;
        if (!chain.isEmpty()) {
            const QJsonObject c0 = chain.at(0).toObject();
            for (const auto &v : c0.value(QLatin1String("l")).toArray())
                l.append(v.toDouble());
            for (const auto &v : c0.value(QLatin1String("r")).toArray())
                r.append(v.toDouble());
        }
        m_mlActive = act && l.size() == 64;
        m_mlEnvL = l;
        m_mlEnvR = r;
        emit insertChanged();
    } else if (tag == TagStat) {
        m_xrun = o.value(QLatin1String("xrun")).toInt();
        m_latencyMs = o.value(QLatin1String("latency_us_one_way")).toDouble() / 1000.0;
        m_version = o.value(QLatin1String("version")).toString();
        emit statChanged();
    }
}

static double dbToGain(double db) { return db <= -71.0 ? 0.0 : qPow(10.0, db / 20.0); }

void MixerClient::setMaster(int src, int out, double gainDb)
{
    command({{"op", "set_master"}, {"src", src}, {"out", out},
             {"gain", dbToGain(gainDb)}});
}

void MixerClient::setInputGain(int src, double gainDb)
{
    command({{"op", "set_input_gain"}, {"src", src},
             {"gain", qPow(10.0, gainDb / 20.0)}});
}

void MixerClient::setMute(int src, bool mute)
{
    command({{"op", "set_mute"}, {"src", src}, {"mute", mute ? 1 : 0}});
}

void MixerClient::setOutputGain(int out, double db)
{
    command({{"op", "set_output_gain"}, {"out", out}, {"db", db}});
}

void MixerClient::setSend(int in, int bus, double gainDb)
{
    command({{"op", "set_send"}, {"in", in}, {"bus", bus},
             {"gain", dbToGain(gainDb)}});
}
