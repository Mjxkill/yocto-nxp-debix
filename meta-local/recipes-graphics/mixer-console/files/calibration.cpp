#include "calibration.h"
#include <QFile>
#include <QRegularExpression>
#include <QProcess>
#include <QDebug>

static const char *RULE_PATH = "/etc/udev/rules.d/99-goodix-calibration.rules";

/* Résout le système 3x3 A·v = b (Cramer). */
static bool solve3(const double A[3][3], const double b[3], double v[3])
{
    auto det3 = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
             - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
             + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double d = det3(A);
    if (qAbs(d) < 1e-12)
        return false;
    for (int col = 0; col < 3; col++) {
        double M[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                M[i][j] = (j == col) ? b[i] : A[i][j];
        v[col] = det3(M) / d;
    }
    return true;
}

/* LSQ affine 1D : out ≈ a·x + b·y + c sur n points. */
static bool lsqAxis(const QVector<QPointF> &in, const QVector<double> &out,
                    double coef[3])
{
    double A[3][3] = {{0}};
    double b[3] = {0};
    for (int i = 0; i < in.size(); i++) {
        const double x = in[i].x(), y = in[i].y(), t = out[i];
        A[0][0] += x * x; A[0][1] += x * y; A[0][2] += x;
        A[1][0] += x * y; A[1][1] += y * y; A[1][2] += y;
        A[2][0] += x;     A[2][1] += y;     A[2][2] += 1;
        b[0] += x * t;    b[1] += y * t;    b[2] += t;
    }
    return solve3(A, b, coef);
}

bool CalibrationHelper::finish(const QVariantList &raw, const QVariantList &targets,
                               double winW, double winH)
{
    m_active = false;
    emit activeChanged();
    if (raw.size() != targets.size() || raw.size() < 3)
        return false;

    /* points normalisés 0..1 (espace matrice libinput) */
    QVector<QPointF> rn;
    QVector<double> tx, ty;
    for (int i = 0; i < raw.size(); i++) {
        const QVariantMap r = raw.at(i).toMap();
        const QVariantMap t = targets.at(i).toMap();
        rn.append(QPointF(r.value("x").toDouble() / winW,
                          r.value("y").toDouble() / winH));
        tx.append(t.value("x").toDouble() / winW);
        ty.append(t.value("y").toDouble() / winH);
    }

    double rx[3], ry[3];
    if (!lsqAxis(rn, tx, rx) || !lsqAxis(rn, ty, ry)) {
        qWarning() << "calibration: LSQ singulier";
        return false;
    }
    /* R = résiduelle (post-matrice-courante → cible) */

    /* matrice courante M0 (identité si absente) */
    double m0[6] = {1, 0, 0, 0, 1, 0};
    QFile f(QString::fromLatin1(RULE_PATH));
    if (f.open(QIODevice::ReadOnly)) {
        const QString txt = QString::fromUtf8(f.readAll());
        QRegularExpression re(QStringLiteral(
            "LIBINPUT_CALIBRATION_MATRIX\\\"?=\\\"([-0-9.eE ]+)\\\""));
        const auto m = re.match(txt);
        if (m.hasMatch()) {
            const QStringList parts = m.captured(1).split(' ', Qt::SkipEmptyParts);
            if (parts.size() == 6)
                for (int i = 0; i < 6; i++)
                    m0[i] = parts[i].toDouble();
        }
        f.close();
    }

    /* composition M = R ∘ M0 (affines 2x3) */
    double M[6];
    M[0] = rx[0] * m0[0] + rx[1] * m0[3];
    M[1] = rx[0] * m0[1] + rx[1] * m0[4];
    M[2] = rx[0] * m0[2] + rx[1] * m0[5] + rx[2];
    M[3] = ry[0] * m0[0] + ry[1] * m0[3];
    M[4] = ry[0] * m0[1] + ry[1] * m0[4];
    M[5] = ry[0] * m0[2] + ry[1] * m0[5] + ry[2];

    const QString rule = QStringLiteral(
        "# Calibration tactile GT911 — générée par mixer-console (5 mires)\n"
        "ATTRS{name}==\"Goodix Capacitive TouchScreen\", "
        "ENV{LIBINPUT_CALIBRATION_MATRIX}=\"%1 %2 %3 %4 %5 %6\"\n")
        .arg(M[0], 0, 'f', 5).arg(M[1], 0, 'f', 5).arg(M[2], 0, 'f', 5)
        .arg(M[3], 0, 'f', 5).arg(M[4], 0, 'f', 5).arg(M[5], 0, 'f', 5);

    QFile out(QString::fromLatin1(RULE_PATH));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "calibration: écriture règle impossible";
        return false;
    }
    out.write(rule.toUtf8());
    out.close();
    qWarning() << "calibration: matrice" << M[0] << M[1] << M[2]
               << M[3] << M[4] << M[5];

    /* recharge udev puis redémarre l'app (la matrice est lue à l'open) */
    QProcess::execute("udevadm", {"control", "--reload-rules"});
    QProcess::execute("udevadm", {"trigger"});
    QProcess::startDetached("systemd-run",
        {"--unit=console-cal-restart", "--collect", "sh", "-c",
         "sleep 1; systemctl restart console-n0 mixer-console 2>/dev/null; true"});
    return true;
}
