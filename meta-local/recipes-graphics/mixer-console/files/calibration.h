/* V10-N4.4 — Calibration tactile intégrée : 5 mires, moindres carrés,
 * composition avec la matrice libinput ACTIVE (les événements reçus sont
 * déjà transformés par elle), écriture de la règle udev, redémarrage. */
#pragma once
#include <QObject>
#include <QVariantList>
#include <QPointF>

class CalibrationHelper : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
public:
    explicit CalibrationHelper(QObject *parent = nullptr) : QObject(parent) {}

    bool active() const { return m_active; }
    Q_INVOKABLE void start() { m_active = true; emit activeChanged(); }
    Q_INVOKABLE void cancel() { m_active = false; emit activeChanged(); }

    /* raw = taps mesurés (coords fenêtre, POST matrice courante),
     * targets = positions exactes des mires (coords fenêtre).
     * Calcule l'affine résiduelle par LSQ, la compose avec la matrice
     * courante, écrit la règle udev et programme un restart. */
    Q_INVOKABLE bool finish(const QVariantList &raw, const QVariantList &targets,
                            double winW, double winH);

    /* reçu du TouchLogger (coords fenêtre) */
    void feedTouch(double x, double y) { if (m_active) emit rawTouch(x, y); }

signals:
    void activeChanged();
    void rawTouch(double x, double y);

private:
    bool m_active = false;
};
