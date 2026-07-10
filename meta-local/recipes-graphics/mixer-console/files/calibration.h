/* V10-N4.4 — Calibration tactile intégrée : 5 mires, moindres carrés,
 * composition avec la matrice libinput ACTIVE (les événements reçus sont
 * déjà transformés par elle), écriture de la règle udev, redémarrage.
 * V13.2 — étape de CONFIRMATION post-restart : 2 mires de vérification ;
 * échec ou timeout → retour automatique à la matrice précédente (.bak).
 * Une calibration ratée ne peut plus verrouiller la console. */
#pragma once
#include <QObject>
#include <QVariantList>
#include <QPointF>

class CalibrationHelper : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool verifyMode READ verifyMode NOTIFY verifyModeChanged)
public:
    explicit CalibrationHelper(QObject *parent = nullptr) : QObject(parent) {}

    bool active() const { return m_active; }
    bool verifyMode() const { return m_verify; }
    Q_INVOKABLE void start() { m_active = true; emit activeChanged(); }
    Q_INVOKABLE void cancel() { m_active = false; emit activeChanged(); }

    /* raw = taps mesurés (coords fenêtre, POST matrice courante),
     * targets = positions exactes des mires (coords fenêtre).
     * Calcule l'affine résiduelle par LSQ, la compose avec la matrice
     * courante, sauvegarde l'ancienne règle en .bak + pose le drapeau
     * « en attente de confirmation », écrit la règle udev et programme
     * un restart. */
    Q_INVOKABLE bool finish(const QVariantList &raw, const QVariantList &targets,
                            double winW, double winH);

    /* Au démarrage : drapeau pending présent → passe en mode vérification
     * (l'overlay QML affiche 2 mires de confirmation). */
    void checkPending();
    /* Verdict de l'overlay : ok = garde la nouvelle matrice ;
     * fail/timeout = restaure la .bak, recharge udev, restart. */
    Q_INVOKABLE void verifyOk();
    Q_INVOKABLE void verifyFail();

    /* reçu du TouchLogger (coords fenêtre) */
    void feedTouch(double x, double y) {
        if (m_active || m_verify) emit rawTouch(x, y);
    }

signals:
    void activeChanged();
    void verifyModeChanged();
    void rawTouch(double x, double y);

private:
    bool m_active = false;
    bool m_verify = false;
};
