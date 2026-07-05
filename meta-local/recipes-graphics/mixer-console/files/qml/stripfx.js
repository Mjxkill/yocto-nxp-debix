// V10-N5 — Bibliothèque effets par piste : PORT QUASI-VERBATIM du module
// éprouvé de beta.html (P2d-P2j). Source de vérité des formules DRC :
// sof/tools/tune/drc/drc_gen_coefs.m. Toute évolution doit être reportée
// dans les DEUX fichiers (web beta.html + ici).
.pragma library

// ============ parse amixer contents ============
function parseAmixer(text) {
    const out = {};
    const blocks = text.split(/^numid=/m).filter(b => b.trim());
    for (const blk of blocks) {
        const lines = blk.split("\n");
        const m = lines[0].match(/^(\d+),iface=MIXER,name='([^']+)'/);
        if (!m) continue;
        const c = { numid: parseInt(m[1]), name: m[2], items: [], type: "?", value: "" };
        for (let i = 1; i < lines.length; i++) {
            const t = lines[i].trim();
            if (t.indexOf("; type=") === 0) {
                const tm = t.match(/type=([A-Z]+)/), mn = t.match(/min=(-?\d+)/),
                      mx = t.match(/max=(-?\d+)/), cnt = t.match(/values=(\d+)/);
                if (tm) c.type = tm[1];
                if (mn) c.min = parseInt(mn[1]);
                if (mx) c.max = parseInt(mx[1]);
                if (cnt) c.count = parseInt(cnt[1]);
            } else if (t.indexOf("; Item #") === 0) {
                const im = t.match(/Item #(\d+) '([^']*)'/);
                if (im) c.items[parseInt(im[1])] = im[2];
            } else if (t.indexOf(": values=") === 0) {
                c.value = t.substring(9);
            } else if (t.indexOf("| dB") === 0) {
                const dm = t.match(/min=(-?[\d.]+)\s*dB/), dx = t.match(/max=(-?[\d.]+)\s*dB/),
                      ds = t.match(/step=(-?[\d.]+)\s*dB/);
                if (dm) c.dbMin = parseFloat(dm[1]);
                if (dx) c.dbMax = parseFloat(dx[1]);
                if (ds) c.dbStep = parseFloat(ds[1]);
            }
        }
        out[c.name] = c;
    }
    return out;
}

function shortLbl(n) {
    /* garder la lettre A/B des sorties DAC (deux drivers de sortie
     * physiques par canal — OUTxA/OUTxB du TAC5212), sinon les deux
     * volumes du canal deviennent indistinguables */
    return n.replace(/^TAC\d+\s+(?:ADC|DAC|OUT|CH|MICBIAS|VAD|VREF)(?:\d([A-Z])?)?\s+/,
                     function(m, l) { return l ? "OUT " + l + " · " : ""; })
            .replace(/^TAC\d+\s+/, "").replace(/^PGA\d\.\d \d /, "")
            .replace(/^(MULTIBAND_DRC|DRC)\d\.\d /, "");
}

// ============ groupes par tranche (port alsaGroupsForStrip) ============
function groupsFor(idx, isOut, all) {
    const groups = [], names = Object.keys(all);
    const mk = (n) => Object.assign({}, all[n], { short: shortLbl(n), fullName: n });
    const isBq = (n) => /^TAC\d+ (ADC|DAC) BQ\d+ Coefs$/.test(n);
    if (idx < 8) {
        const dsp = names.filter(n => isOut ? n.indexOf("MULTIBAND_DRC2.0 ") === 0
            : (n.indexOf("MULTIBAND_DRC1.0 ") === 0 || n.indexOf("DRC1.0 ") === 0)).map(mk);
        if (dsp.length)
            groups.push({ title: isOut ? "CHAÎNE DSP (sortie)" : "CHAÎNE DSP (entrée)",
                          subtitle: "réglages par canal — " + (isOut ? "S" : "M") + ((idx % 8) + 1),
                          controls: dsp });
    }
    const tac = Math.floor(idx / 2), ch = (idx % 2) + 1, P = "TAC" + tac;
    if (!isOut && idx < 8) {
        const perCh = names.filter(n => n.indexOf(P + " ADC" + ch) === 0
                                      || n === P + " CH" + ch + " Input Mux").map(mk);
        const shared = names.filter(n => n.indexOf(P + " ADC ") === 0
                                       && !n.match(/ADC\d/) && !isBq(n)).map(mk);
        const bq = names.filter(n => n.indexOf(P + " ADC BQ") === 0 && isBq(n))
            .sort((a, b) => +a.match(/BQ(\d+)/)[1] - +b.match(/BQ(\d+)/)[1]).map(mk);
        const misc = names.filter(n => n.indexOf(P + " MICBIAS") === 0
                                     || n.indexOf(P + " VAD") === 0
                                     || n.indexOf(P + " VREF") === 0).map(mk);
        const pgaN = "PGA1.0 1 Strip" + (idx + 1) + " Volume";
        if (perCh.length) groups.push({ title: "TAC ADC " + ch, subtitle: perCh.length + " contrôles canal", controls: perCh });
        if (shared.length) groups.push({ title: "TAC ADC COMMUN", subtitle: "partagé CH1+CH2", controls: shared });
        if (bq.length) groups.push({ title: "TAC BIQUADS", subtitle: "12 filtres RBJ", controls: bq, biquads: true });
        if (misc.length) groups.push({ title: "TAC DIVERS", subtitle: "bias / VAD / VREF", controls: misc });
        if (all[pgaN]) groups.push({ title: "DSP PGA", subtitle: "volume strip SOF", controls: [mk(pgaN)] });
    } else if (!isOut && idx < 18) {
        const pgaN = "PGA1.0 1 Strip" + (idx + 1) + " Volume";
        if (all[pgaN]) groups.push({ title: "DSP PGA", subtitle: "volume strip SOF", controls: [mk(pgaN)] });
    } else if (isOut && idx < 8) {
        const perCh = names.filter(n => n.indexOf(P + " DAC" + ch) === 0
                                      || n.indexOf(P + " OUT" + ch) === 0).map(mk);
        const shared = names.filter(n => n.indexOf(P + " DAC ") === 0
                                       && !n.match(/DAC\d/) && !isBq(n)).map(mk);
        const bq = names.filter(n => n.indexOf(P + " DAC BQ") === 0 && isBq(n))
            .sort((a, b) => +a.match(/BQ(\d+)/)[1] - +b.match(/BQ(\d+)/)[1]).map(mk);
        const pgaN = "PGA2.0 2 Out Strip" + (idx + 1) + " Volume";
        if (perCh.length) groups.push({ title: "TAC DAC " + ch, subtitle: perCh.length + " contrôles canal", controls: perCh });
        if (shared.length) groups.push({ title: "TAC DAC COMMUN", subtitle: "partagé CH1+CH2", controls: shared });
        if (bq.length) groups.push({ title: "TAC BIQUADS", subtitle: "12 filtres RBJ", controls: bq, biquads: true });
        if (all[pgaN]) groups.push({ title: "DSP PGA", subtitle: "volume strip SOF", controls: [mk(pgaN)] });
    }
    return groups;
}

function intVal(c) {
    if (!c.value) return c.min || 0;
    const n = parseInt(String(c.value).split(",")[0].trim(), 10);
    return isNaN(n) ? (c.min || 0) : n;
}
function isOn(c) {
    const v = String(c.value || "").toLowerCase();
    return v.indexOf("on") >= 0 || v === "1" || v.indexOf("true") >= 0;
}

// ============ biquads RBJ (coefs Q1.31 BE, convention TI) ============
var BIQUAD_TYPES = ["Bypass", "LPF", "HPF", "BPF", "Notch", "Peak", "LowShelf", "HighShelf", "AllPass"];

function rbjBlob(type, fHz, q, gainDb, fs) {
    fs = fs || 48000;
    fHz = Math.max(20, Math.min(+fHz || 1000, fs / 2 - 100));
    q = Math.max(0.1, Math.min(+q || 0.707, 16));
    gainDb = Math.max(-24, Math.min(+gainDb || 0, 24));
    let b0, b1, b2, a0, a1, a2;
    const w0 = 2 * Math.PI * fHz / fs, cw = Math.cos(w0), sw = Math.sin(w0), al = sw / (2 * q);
    const A = Math.pow(10, gainDb / 40), be = Math.sqrt(A) / q;
    switch (+type | 0) {
    case 0: return [0x7f, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    case 1: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case 2: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case 3: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case 4: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case 5: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
    case 6: b0 = A * ((A + 1) - (A - 1) * cw + be * sw); b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - be * sw); a0 = (A + 1) + (A - 1) * cw + be * sw;
        a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - be * sw; break;
    case 7: b0 = A * ((A + 1) + (A - 1) * cw + be * sw); b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - be * sw); a0 = (A + 1) - (A - 1) * cw + be * sw;
        a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - be * sw; break;
    case 8: b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    default: return [0x7f, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    }
    const N0 = b0 / a0, N1 = b1 / a0, N2 = b2 / a0, D1 = -a1 / a0, D2 = -a2 / a0;
    const q31 = (x) => {
        let v = Math.round(Math.max(-1, Math.min(0.9999999995, x)) * 0x80000000);
        if (v < -0x80000000) v = -0x80000000;
        if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
        if (v < 0) v += 0x100000000;
        return [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
    };
    const out = [];
    [N0, N1, N2, D1, D2].forEach(c => { const b = q31(c); out.push(b[0], b[1], b[2], b[3]); });
    return out;
}

// ============ blobs DRC / MULTIBAND (hex ↔ objets) ============
var DRC_PARAMS_SIZE = 88, DRC_CFG_HDR = 20, MBDRC_HDR = 324, ABI_HDR = 32;
function q24f(v) { return v / (1 << 24); }
function q30f(v) { return v / (1 << 30); }
function fq24(v) { return Math.max(-2147483648, Math.min(2147483647, Math.round(v * (1 << 24)))); }
function fq30(v) { return Math.max(-2147483648, Math.min(2147483647, Math.round(v * (1 << 30)))); }
function db2mag(db) { return Math.pow(10, db / 20); }
function mag2db(m) { return 20 * Math.log10(Math.max(m, 1e-30)); }

function hex2bytes(h) {
    const n = h.length >> 1, out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
    return out;
}
function bytes2hex(a) {
    let s = "";
    for (let i = 0; i < a.length; i++) s += (a[i] < 16 ? "0" : "") + a[i].toString(16);
    return s;
}
function isMb(name) { return /^MULTIBAND_DRC/.test(name || ""); }
function isDrcBlob(name) { return /^(MULTIBAND_DRC|DRC)\d+\.\d+ /.test(name || ""); }

var DRC_SLIDERS = [
    { key: "threshold_dB", label: "Threshold", unit: "dB", min: -60, max: 0, step: 0.5 },
    { key: "knee_dB", label: "Knee", unit: "dB", min: 0, max: 40, step: 0.5 },
    { key: "ratio", label: "Ratio", unit: ":1", min: 1, max: 30, step: 0.1 },
    { key: "pre_delay_ms", label: "Pre-delay", unit: "ms", min: 0, max: 50, step: 0.1 },
    { key: "attack_ms", label: "Attack", unit: "ms", min: 1, max: 500, step: 0.5 },
    { key: "master_gain_dB", label: "Make-up", unit: "dB", min: -24, max: 24, step: 0.1 },
    { key: "release_spacing_dB", label: "Rel.spacing", unit: "dB", min: 0, max: 20, step: 0.5 }
];

function parseDrcParams(view, off) {
    const r = (i) => view.getInt32(off + i * 4, true);
    const inv = r(13), att = (inv ? 1 / q30f(inv) : 0) / 48000;
    const raw = new Uint8Array(88);
    for (let i = 0; i < 88; i++) raw[i] = view.getUint8(off + i);
    return { enabled: r(0), threshold_dB: q24f(r(1)), knee_dB: q24f(r(2)), ratio: q24f(r(3)),
             pre_delay_ms: q30f(r(4)) * 1000, attack_ms: att * 1000,
             master_gain_dB: mag2db(q24f(r(12))), release_spacing_dB: r(16), _raw: raw };
}

function packDrcParams(p) {
    const Fs = 48000;
    const buf = new Uint8Array(88);
    buf.set(p._raw);
    const dv = new DataView(buf.buffer);
    const w = (i, v) => dv.setInt32(i * 4, v | 0, true);
    const thr = +p.threshold_dB || 0, knee = +p.knee_dB || 0, ratio = Math.max(1.0001, +p.ratio || 1);
    /* clamp attack à 0.05 ms (anti div/0) — l'ancien plancher 1 ms
     * ALTÉRAIT les blobs réels (DRC1.0 board : attack 0.919 ms), attrapé
     * par l'autotest round-trip. Fix miroir dans beta.html. */
    const pre = (+p.pre_delay_ms || 0) / 1000, att = Math.max(0.00005, (+p.attack_ms || 0) / 1000);
    const mlin = db2mag(+p.master_gain_dB || 0);
    const lt = db2mag(thr), slope = 1 / ratio, kt = db2mag(thr + knee);
    const slopeAt = (x, k) => {
        if (x < lt) return 1;
        const kc = (xv) => lt + (1 - Math.exp(-k * (xv - lt))) / k;
        const x2 = x * 1.001;
        return (mag2db(kc(x2)) - mag2db(kc(x))) / (mag2db(x2) - mag2db(x));
    };
    let mn = 0.1, mx = 10000, K = 5;
    for (let it = 0; it < 15; it++) { const sl = slopeAt(kt, K); if (sl < slope) mx = K; else mn = K; K = Math.sqrt(mn * mx); }
    const ka = lt + 1 / K, kb = -Math.exp(K * lt) / K;
    const kc = (x) => x < lt ? x : lt + (1 - Math.exp(-K * (x - lt))) / K;
    const rbase = kc(kt) * Math.pow(kt, -slope);
    w(0, p.enabled ? 1 : 0); w(1, fq24(thr)); w(2, fq24(knee)); w(3, fq24(ratio));
    w(4, fq30(pre)); w(5, fq30(lt)); w(6, fq30(slope));
    w(7, Math.round(K * (1 << 20)) | 0); w(8, fq24(ka)); w(9, fq24(kb)); w(10, fq24(kt));
    w(11, fq30(rbase)); w(12, fq24(mlin)); w(13, fq30(1 / (att * Fs)));
    w(16, Math.round(+p.release_spacing_dB || 0));
    return buf;
}

function crossBq(fs, fc, hp) {
    let cut = fc / (fs / 2);
    if (cut < 0) cut = 0; if (cut > 1) cut = 1;
    if (cut === 0 || cut === 1) { const b0 = hp ? (1 - cut) : cut; return { a1: 0, a2: 0, b0: b0, b1: 0, b2: 0 }; }
    const d = Math.SQRT2, th = Math.PI * cut, sn = 0.5 * d * Math.sin(th);
    const beta = 0.5 * (1 - sn) / (1 + sn), gamma = (0.5 + beta) * Math.cos(th);
    const alpha = 0.25 * (0.5 + beta + (hp ? gamma : -gamma));
    return { b0: 2 * alpha, b1: hp ? -4 * alpha : 4 * alpha, b2: 2 * alpha, a1: -2 * gamma, a2: 2 * beta };
}
function bqBytes(bq) {
    const buf = new Uint8Array(28), dv = new DataView(buf.buffer);
    dv.setInt32(0, fq30(-bq.a2), true); dv.setInt32(4, fq30(-bq.a1), true);
    dv.setInt32(8, fq30(bq.b2), true); dv.setInt32(12, fq30(bq.b1), true);
    dv.setInt32(16, fq30(bq.b0), true); dv.setInt32(20, 0, true); dv.setInt32(24, 16384, true);
    return buf;
}
function flatBq() {
    const buf = new Uint8Array(28), dv = new DataView(buf.buffer);
    dv.setInt32(16, 1 << 30, true); dv.setInt32(24, 16384, true);
    return buf;
}
function packCross(nb, fs, fcs) {
    const out = new Uint8Array(168);
    let lps, hps;
    if (nb <= 1) { for (let i = 0; i < 3; i++) { out.set(flatBq(), i * 56); out.set(flatBq(), i * 56 + 28); } return out; }
    if (nb === 2) { lps = [crossBq(fs, fcs.low, false), null, null]; hps = [crossBq(fs, fcs.low, true), null, null]; }
    else if (nb === 3) {
        lps = [crossBq(fs, fcs.low, false), crossBq(fs, fcs.high, false), crossBq(fs, fcs.high, false)];
        hps = [crossBq(fs, fcs.low, true), crossBq(fs, fcs.high, true), crossBq(fs, fcs.high, true)];
    } else {
        lps = [crossBq(fs, fcs.low, false), crossBq(fs, fcs.mid, false), crossBq(fs, fcs.high, false)];
        hps = [crossBq(fs, fcs.low, true), crossBq(fs, fcs.mid, true), crossBq(fs, fcs.high, true)];
    }
    for (let i = 0; i < 3; i++) {
        out.set(lps[i] ? bqBytes(lps[i]) : flatBq(), i * 56);
        out.set(hps[i] ? bqBytes(hps[i]) : flatBq(), i * 56 + 28);
    }
    return out;
}
function fcFromBq(bytes, off, fs) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset + off, 28);
    const a2 = q30f(dv.getInt32(0, true)), a1 = q30f(dv.getInt32(4, true));
    const beta = -a2 / 2, gamma = a1 / 2;
    if (Math.abs(beta) < 1e-9 && Math.abs(gamma) < 1e-9) return NaN;
    const sn = (1 - 2 * beta) / (1 + 2 * beta);
    let s = sn * Math.SQRT2;
    if (s < -1) s = -1; if (s > 1) s = 1;
    const c = (0.5 + beta) === 0 ? 0 : gamma / (0.5 + beta);
    let th = Math.asin(s);
    if (c < 0) th = Math.PI - th;
    return th * fs / (2 * Math.PI);
}
function extractFcs(cb, nb, fs) {
    fs = fs || 48000;
    const fcs = { low: 200, mid: 2000, high: 5000 };
    if (nb < 2) return fcs;
    const f0 = fcFromBq(cb, 0, fs); if (!isNaN(f0)) fcs.low = Math.round(f0);
    if (nb === 3) { const f1 = fcFromBq(cb, 56, fs); if (!isNaN(f1)) fcs.high = Math.round(f1); }
    else if (nb === 4) {
        const f1 = fcFromBq(cb, 56, fs), f2 = fcFromBq(cb, 112, fs);
        if (!isNaN(f1)) fcs.mid = Math.round(f1);
        if (!isNaN(f2)) fcs.high = Math.round(f2);
    }
    return fcs;
}

function parseBlob(hex, fullName) {
    const raw = hex2bytes(hex);
    if (raw.length < ABI_HDR + 4) return null;
    const abiHdr = raw.slice(0, ABI_HDR);
    const payload = raw.slice(ABI_HDR);
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    if (isMb(fullName)) {
        const size = view.getUint32(0, true), num_bands = view.getUint32(4, true),
              emp = view.getUint32(8, true);
        const reservedBytes = payload.slice(12, 44), empBytes = payload.slice(44, 100),
              deempBytes = payload.slice(100, 156), crossBytes = payload.slice(156, 324);
        /* V10-FX blob V3 : détection par arithmétique de taille (miroir
         * firmware multiband_drc_init_coef) — une section crossover PAR
         * CANAL (ppb × 168 o) peut suivre les drc_coef. */
        const trailing = size - MBDRC_HDR;
        const oneBand = Math.max(1, num_bands) * DRC_PARAMS_SIZE;
        const oneBandX = oneBand + 168;
        let ppb, xoverPerCh = false;
        if (trailing % oneBandX === 0 && trailing / oneBandX > 1
                && trailing / oneBandX <= 8) {
            ppb = trailing / oneBandX;
            xoverPerCh = true;
        } else {
            ppb = Math.max(1, Math.floor(trailing / oneBand));
        }
        const drc = [];
        for (let b = 0; b < num_bands; b++) {
            const row = [];
            for (let c = 0; c < ppb; c++)
                row.push(parseDrcParams(view, MBDRC_HDR + (b * ppb + c) * DRC_PARAMS_SIZE));
            /* V10-FX : normalisation à 8 entrées par bande (copies
             * profondes) — l'édition par canal est toujours possible,
             * le pack écrit systématiquement le layout per-channel. */
            while (row.length < 8) {
                const src = row[row.length - 1];
                const cp = {};
                for (const k in src) cp[k] = k === "_raw" ? new Uint8Array(src._raw) : src[k];
                row.push(cp);
            }
            drc.push(row);
        }
        const ppbNorm = 8;
        /* crossover_fcs_ch : TOUJOURS exposé par canal (8 entrées) — V2 :
         * répliques du global ; V3 : sections décodées. L'édition d'un
         * canal pose xover_per_ch=true → le pack écrit le blob V3. */
        const fcsGlobal = extractFcs(crossBytes, num_bands, 48000);
        const fcsCh = [];
        const xbase = MBDRC_HDR + num_bands * ppb * DRC_PARAMS_SIZE;
        for (let c = 0; c < 8; c++) {
            if (xoverPerCh) {
                const cc = Math.min(c, ppb - 1);
                fcsCh.push(extractFcs(payload.slice(xbase + cc * 168,
                                                    xbase + (cc + 1) * 168),
                                      num_bands, 48000));
            } else {
                fcsCh.push({ low: fcsGlobal.low, mid: fcsGlobal.mid,
                             high: fcsGlobal.high });
            }
        }
        return { kind: "multiband", abiHdr: abiHdr, size: size, num_bands: num_bands,
                 enable_emp_deemp: emp, reservedBytes: reservedBytes, empBytes: empBytes,
                 deempBytes: deempBytes, crossBytes: crossBytes,
                 crossover_fcs: fcsGlobal,
                 xover_per_ch: xoverPerCh, crossover_fcs_ch: fcsCh,
                 drc: drc, params_per_band: ppbNorm };
    }
    const size = view.getUint32(0, true), reservedBytes = payload.slice(4, 20);
    const N = Math.max(1, Math.floor((size - DRC_CFG_HDR) / DRC_PARAMS_SIZE));
    const params = [];
    for (let i = 0; i < N; i++)
        params.push(parseDrcParams(view, DRC_CFG_HDR + i * DRC_PARAMS_SIZE));
    return { kind: "drc", abiHdr: abiHdr, size: size, reservedBytes: reservedBytes,
             params: params, params_per_band: N };
}

function packBlob(b) {
    let cfg;
    if (b.kind === "multiband") {
        /* V10-FX : blob V3 si xover par canal — 8 sections de 168 o après
         * les drc_coef (le firmware clampe ch ≥ ppb sur la dernière). Le
         * ppb passe à 8 en V3 (une entrée drc ET une entrée xover par ch). */
        const xover = b.xover_per_ch === true;
        const ppbOut = xover ? 8 : b.params_per_band;
        const tot = MBDRC_HDR + b.num_bands * ppbOut * DRC_PARAMS_SIZE
                    + (xover ? ppbOut * 168 : 0);
        cfg = new Uint8Array(tot);
        const dv = new DataView(cfg.buffer);
        dv.setUint32(0, tot, true); dv.setUint32(4, b.num_bands, true);
        dv.setUint32(8, b.enable_emp_deemp, true);
        cfg.set(b.reservedBytes, 12); cfg.set(b.empBytes, 44);
        cfg.set(b.deempBytes, 100); cfg.set(b.crossBytes, 156);
        for (let band = 0; band < b.num_bands; band++)
            for (let c = 0; c < ppbOut; c++) {
                const src = b.drc[band][Math.min(c, b.params_per_band - 1)];
                cfg.set(packDrcParams(src),
                        MBDRC_HDR + (band * ppbOut + c) * DRC_PARAMS_SIZE);
            }
        if (xover) {
            const xbase = MBDRC_HDR + b.num_bands * ppbOut * DRC_PARAMS_SIZE;
            for (let c = 0; c < ppbOut; c++)
                cfg.set(packCross(b.num_bands, 48000, b.crossover_fcs_ch[c]),
                        xbase + c * 168);
        }
    } else {
        const tot = DRC_CFG_HDR + b.params.length * DRC_PARAMS_SIZE;
        cfg = new Uint8Array(tot);
        new DataView(cfg.buffer).setUint32(0, tot, true);
        cfg.set(b.reservedBytes, 4);
        for (let i = 0; i < b.params.length; i++)
            cfg.set(packDrcParams(b.params[i]), DRC_CFG_HDR + i * DRC_PARAMS_SIZE);
    }
    const full = new Uint8Array(b.abiHdr.length + cfg.length);
    full.set(b.abiHdr, 0);
    full.set(cfg, b.abiHdr.length);
    return bytes2hex(full);
}

/* AUTOTEST round-trip (suggestion critic) : décode le blob puis le
 * réencode SANS modification — la taille et la structure doivent tenir,
 * et les champs primaires relus doivent coïncider (les champs dérivés du
 * solveur peuvent différer d'un LSB : on revalide par re-parse). */
function selfTest(hex, fullName) {
    const b = parseBlob(hex, fullName);
    if (!b) return "parse KO";
    /* V10-FX : le 1er pack peut NORMALISER (V2 legacy → per-channel 8
     * entrées, ajout section xover V3) — la taille peut donc changer au
     * 1er tour. L'invariant devient l'idempotence du 2e tour. */
    const hex2 = packBlob(b);
    const b2 = parseBlob(hex2, fullName);
    if (!b2) return "re-parse KO";
    const hex3 = packBlob(b2);
    if (hex3.length !== hex2.length)
        return "taille instable " + hex2.length + "→" + hex3.length;
    const p1 = b.kind === "multiband" ? b.drc[0][0] : b.params[0];
    const p2 = b2.kind === "multiband" ? b2.drc[0][0] : b2.params[0];
    for (const k of ["enabled", "threshold_dB", "knee_dB", "ratio", "attack_ms"])
        if (Math.abs((p1[k] || 0) - (p2[k] || 0)) > 0.01)
            return "champ " + k + " diverge: " + p1[k] + " vs " + p2[k];
    return "OK";
}
