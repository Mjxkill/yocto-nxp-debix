/* A.L.A. console — drawer effets par piste (TAC+DSP) + EQ paramétrique PRO — une seule closure, voir fiche étape 7
 * Extrait de beta.html (V14.0 étape 7, tranche contiguë — ordre de
 * chargement = ordre d'origine, sémantique identique au script unique). */
/* ================== V10-P2d : effets par piste (TAC + DSP) ==================
 * Port fidèle du panneau détail de l'ancienne GUI (index.html) :
 *  - /api/alsa/contents (amixer) → contrôles TAC ADC/DAC/OUT + PGA par tranche
 *  - biquads programmables TAC : forme RBJ (type/Hz/Q/gain) → blob Q1.31 BE
 *  - blobs SOF DRC / MULTIBAND_DRC : /api/dsp/blob — mêmes formules que
 *    drc_gen_coefs.m (solveur K dichotomique, crossover Butterworth).
 * Ouverture : tap sur le nom d'une tranche. Canal DSP visé = idx%8. */
(function(){
"use strict";
const $=s=>document.querySelector(s);
const A={controls:{},bq:{},blobs:{},idx:0,isOut:false,tab:"",open:false};
const BIQUAD_TYPES=['Bypass','LPF','HPF','BPF','Notch','Peak','LowShelf','HighShelf','AllPass'];
const DRC_SLIDERS=[
  {key:'threshold_dB',      label:'Threshold',   unit:'dB', min:-60,max:0,  step:0.5},
  {key:'knee_dB',           label:'Knee',        unit:'dB', min:0,  max:40, step:0.5},
  {key:'ratio',             label:'Ratio',       unit:':1', min:1,  max:30, step:0.1},
  {key:'pre_delay_ms',      label:'Pre-delay',   unit:'ms', min:0,  max:50, step:0.1},
  {key:'attack_ms',         label:'Attack',      unit:'ms', min:1,  max:500,step:0.5},
  {key:'master_gain_dB',    label:'Make-up',     unit:'dB', min:-24,max:24, step:0.1},
  {key:'release_spacing_dB',label:'Rel. spacing',unit:'dB', min:0,  max:20, step:0.5},
];
const DRC_PARAMS_SIZE=88, DRC_CFG_HDR=20, MBDRC_HDR=324, ABI_HDR=32;
const q24f=v=>v/(1<<24), q30f=v=>v/(1<<30);
const fq24=v=>Math.max(-2147483648,Math.min(2147483647,Math.round(v*(1<<24))));
const fq30=v=>Math.max(-2147483648,Math.min(2147483647,Math.round(v*(1<<30))));
const db2mag=db=>Math.pow(10,db/20), mag2db=m=>20*Math.log10(Math.max(m,1e-30));
const hex2b=h=>{const n=h.length>>>1,o=new Uint8Array(n);
  for(let i=0;i<n;i++)o[i]=parseInt(h.substr(i*2,2),16);return o;};
const b2hex=a=>{let s="";for(let i=0;i<a.length;i++)s+=a[i].toString(16).padStart(2,"0");return s;};
const isDrcBlob=n=>/^(MULTIBAND_DRC|DRC)\d+\.\d+ /.test(n||"");
const isMb=n=>/^MULTIBAND_DRC/.test(n||"");
const parseBqName=n=>{const m=(n||"").match(/^(TAC\d+) (ADC|DAC) BQ(\d+) Coefs$/);
  return m?{tac:m[1],chain:m[2],idx:+m[3]}:null;};

/* ---- amixer contents parser (identique ancien GUI) ---- */
function parseAmixer(text){
  const out={};
  const blocks=text.split(/^numid=/m).filter(b=>b.trim());
  for(const blk of blocks){
    const lines=blk.split("\n");
    const m=lines[0].match(/^(\d+),iface=MIXER,name='([^']+)'/);
    if(!m)continue;
    const c={numid:+m[1],name:m[2],items:[],type:"?",value:""};
    for(let i=1;i<lines.length;i++){
      const t=lines[i].trim();
      if(t.startsWith("; type=")){
        const tm=t.match(/type=([A-Z]+)/),mn=t.match(/min=(-?\d+)/),
              mx=t.match(/max=(-?\d+)/),cnt=t.match(/values=(\d+)/);
        if(tm)c.type=tm[1];if(mn)c.min=+mn[1];if(mx)c.max=+mx[1];if(cnt)c.count=+cnt[1];
      }else if(t.startsWith("; Item #")){
        const im=t.match(/Item #(\d+) '([^']*)'/);if(im)c.items[+im[1]]=im[2];
      }else if(t.startsWith(": values=")){
        c.value=t.substring(": values=".length);
      }else if(t.startsWith("| dB")){
        const dm=t.match(/min=(-?[\d.]+)\s*dB/),dx=t.match(/max=(-?[\d.]+)\s*dB/),
              ds=t.match(/step=(-?[\d.]+)\s*dB/);
        if(dm)c.dbMin=parseFloat(dm[1]);if(dx)c.dbMax=parseFloat(dx[1]);
        if(ds)c.dbStep=parseFloat(ds[1]);
      }
    }
    out[c.name]=c;
  }
  return out;
}
/* V10-N7 : garder la lettre A/B des volumes DAC (OUTxA/OUTxB = deux
 * drivers de sortie physiques par canal du TAC5212) */
const shortLbl=n=>n
  .replace(/^TAC\d+\s+(?:ADC|DAC|OUT|CH|MICBIAS|VAD|VREF)(?:\d([A-Z])?)?\s+/,
           (m,l)=>l?"OUT "+l+" · ":"")
  .replace(/^TAC\d+\s+/,"").replace(/^PGA\d\.\d \d /,"")
  .replace(/^(MULTIBAND_DRC|DRC)\d\.\d /,"");

/* ---- groupes par tranche (port alsaGroupsForStrip) ---- */
function groupsFor(){
  const groups=[],idx=A.idx,isOut=A.isOut,all=A.controls,names=Object.keys(all);
  const mk=(n,full)=>({...all[n],short:shortLbl(n),fullName:full?n:all[n].fullName});
  if(idx<8){
    const dsp=names.filter(n=>isOut?n.startsWith("MULTIBAND_DRC2.0 ")
      :(n.startsWith("MULTIBAND_DRC1.0 ")||n.startsWith("DRC1.0 "))).map(n=>mk(n,true));
    if(dsp.length)groups.push({title:isOut?"CHAÎNE DSP (sortie)":"CHAÎNE DSP (entrée)",
      subtitle:"réglages par canal — "+(isOut?"S":"M")+((idx%8)+1),controls:dsp});
  }
  const isBq=n=>/^TAC\d+ (ADC|DAC) BQ\d+ Coefs$/.test(n);
  if(!isOut){
    if(idx<8){
      const tac=Math.floor(idx/2),ch=(idx%2)+1,P="TAC"+tac;
      const perCh=names.filter(n=>n.startsWith(P+" ADC"+ch)||n===P+" CH"+ch+" Input Mux").map(n=>mk(n));
      const shared=names.filter(n=>n.startsWith(P+" ADC ")&&!n.match(/ADC\d/)&&!isBq(n)).map(n=>mk(n));
      const bq=names.filter(n=>n.startsWith(P+" ADC BQ")&&isBq(n))
        .sort((a,b)=>+a.match(/BQ(\d+)/)[1]-+b.match(/BQ(\d+)/)[1]).map(n=>mk(n,true));
      const misc=names.filter(n=>n.startsWith(P+" MICBIAS")||n.startsWith(P+" VAD")||n.startsWith(P+" VREF")).map(n=>mk(n));
      const pgaN="PGA1.0 1 Strip"+(idx+1)+" Volume";
      const pga=all[pgaN]?[mk(pgaN)]:[];
      if(perCh.length)groups.push({title:"TAC ADC "+ch,subtitle:perCh.length+" contrôles canal",controls:perCh});
      if(shared.length)groups.push({title:"TAC ADC COMMUN",subtitle:"partagé CH1+CH2",controls:shared});
      if(bq.length)groups.push({title:"TAC BIQUADS",subtitle:"12 filtres programmables (partage CH1+CH2)",controls:bq});
      if(misc.length)groups.push({title:"TAC DIVERS",subtitle:"bias / VAD / VREF",controls:misc});
      if(pga.length)groups.push({title:"DSP PGA",subtitle:"volume strip SOF",controls:pga});
      if(!perCh.length&&!shared.length&&!bq.length&&!misc.length)
        groups.push({title:P+" ABSENT",subtitle:"codec non câblé sur ce port",controls:[],
          placeholder:P+" est déclaré mais ne répond pas sur I²C (adresse 0x"+(0x50+tac).toString(16)+")."});
    }else if(idx<18){
      const pgaN="PGA1.0 1 Strip"+(idx+1)+" Volume";
      if(all[pgaN])groups.push({title:"DSP PGA",subtitle:"volume strip SOF (pas de TAC sur cette source)",controls:[mk(pgaN)]});
    }
  }else if(idx<8){
    const tac=Math.floor(idx/2),ch=(idx%2)+1,P="TAC"+tac;
    const perCh=names.filter(n=>n.startsWith(P+" DAC"+ch)||n.startsWith(P+" OUT"+ch)).map(n=>mk(n));
    const shared=names.filter(n=>n.startsWith(P+" DAC ")&&!n.match(/DAC\d/)&&!isBq(n)).map(n=>mk(n));
    const bq=names.filter(n=>n.startsWith(P+" DAC BQ")&&isBq(n))
      .sort((a,b)=>+a.match(/BQ(\d+)/)[1]-+b.match(/BQ(\d+)/)[1]).map(n=>mk(n,true));
    const pgaN="PGA2.0 2 Out Strip"+(idx+1)+" Volume";
    const pga=all[pgaN]?[mk(pgaN)]:[];
    if(perCh.length)groups.push({title:"TAC DAC "+ch,subtitle:perCh.length+" contrôles canal",controls:perCh});
    if(shared.length)groups.push({title:"TAC DAC COMMUN",subtitle:"partagé CH1+CH2",controls:shared});
    if(bq.length)groups.push({title:"TAC BIQUADS",subtitle:"12 filtres programmables (partage CH1+CH2)",controls:bq});
    if(pga.length)groups.push({title:"DSP PGA",subtitle:"volume strip SOF",controls:pga});
    if(!perCh.length&&!shared.length&&!bq.length)
      groups.push({title:P+" ABSENT",subtitle:"codec non câblé sur ce port",controls:[],
        placeholder:P+" est déclaré mais ne répond pas sur I²C (adresse 0x"+(0x50+tac).toString(16)+")."});
  }
  return groups;
}

/* ---- valeurs + écriture ALSA ---- */
const intVal=c=>{if(!c.value)return c.min||0;
  const n=parseInt(String(c.value).split(",")[0].trim(),10);return isNaN(n)?(c.min||0):n;};
const dbLbl=c=>{const v=intVal(c);
  if(c.dbStep!==undefined&&c.dbMin!==undefined)return(c.dbMin+v*c.dbStep).toFixed(1)+" dB";
  if(c.dbMin!==undefined&&c.dbMax!==undefined&&c.max)
    return(c.dbMin+(v/c.max)*(c.dbMax-c.dbMin)).toFixed(1)+" dB";
  return v+" / "+(c.max??"?");};
const isOn=c=>{const v=String(c.value||"").toLowerCase();
  return v.includes("on")||v==="1"||v.includes("true");};
const thA={};
function setAlsa(c,value){
  const val=String(value);
  if(c.type==="INTEGER"){const n=parseInt(val,10);c.value=(c.count===2)?(n+","+n):String(n);}
  else if(c.type==="BOOLEAN")c.value=val;
  else if(c.type==="ENUMERATED")c.value="'"+val+"'";
  clearTimeout(thA[c.numid]);
  thA[c.numid]=setTimeout(()=>{
    fetch("/api/alsa/set",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({numid:c.numid,value:val})}).catch(()=>{});
  },50);
}

/* ---- biquads RBJ (port rbjBiquadBlob, coefs Q1.31 BE, convention TI) ---- */
function rbjBlob(type,fHz,q,gainDb,fs){
  fs=fs||48000;
  fHz=Math.max(20,Math.min(+fHz||1000,fs/2-100));
  q=Math.max(0.1,Math.min(+q||0.707,16));
  gainDb=Math.max(-24,Math.min(+gainDb||0,24));
  let b0,b1,b2,a0,a1,a2;
  const w0=2*Math.PI*fHz/fs,cw=Math.cos(w0),sw=Math.sin(w0),al=sw/(2*q);
  const Aa=Math.pow(10,gainDb/40),be=Math.sqrt(Aa)/q;
  switch(+type|0){
    case 0:return[0x7f,0xff,0xff,0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0];
    case 1:b0=(1-cw)/2;b1=1-cw;b2=(1-cw)/2;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 2:b0=(1+cw)/2;b1=-(1+cw);b2=(1+cw)/2;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 3:b0=al;b1=0;b2=-al;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 4:b0=1;b1=-2*cw;b2=1;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 5:b0=1+al*Aa;b1=-2*cw;b2=1-al*Aa;a0=1+al/Aa;a1=-2*cw;a2=1-al/Aa;break;
    case 6:b0=Aa*((Aa+1)-(Aa-1)*cw+be*sw);b1=2*Aa*((Aa-1)-(Aa+1)*cw);
      b2=Aa*((Aa+1)-(Aa-1)*cw-be*sw);a0=(Aa+1)+(Aa-1)*cw+be*sw;
      a1=-2*((Aa-1)+(Aa+1)*cw);a2=(Aa+1)+(Aa-1)*cw-be*sw;break;
    case 7:b0=Aa*((Aa+1)+(Aa-1)*cw+be*sw);b1=-2*Aa*((Aa-1)+(Aa+1)*cw);
      b2=Aa*((Aa+1)+(Aa-1)*cw-be*sw);a0=(Aa+1)-(Aa-1)*cw+be*sw;
      a1=2*((Aa-1)-(Aa+1)*cw);a2=(Aa+1)-(Aa-1)*cw-be*sw;break;
    case 8:b0=1-al;b1=-2*cw;b2=1+al;a0=1+al;a1=-2*cw;a2=1-al;break;
    default:return[0x7f,0xff,0xff,0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0];
  }
  /* V11-AL : hardware TAC = (N0+2·N1·z⁻¹+N2·z⁻²)/(2³¹−2·D1·z⁻¹−D2·z⁻²)
   * → N1/D1 stockés DIVISÉS PAR 2 (sinon D1 des filtres graves sature
   * le Q1.31 → filtre inopérant, validé à l'oreille) */
  const N0=b0/a0,N1=b1/a0/2,N2=b2/a0,D1=-a1/a0/2,D2=-a2/a0;
  const q31=x=>{let v=Math.round(Math.max(-1,Math.min(0.9999999995,x))*0x80000000);
    if(v<-0x80000000)v=-0x80000000;if(v>0x7FFFFFFF)v=0x7FFFFFFF;if(v<0)v+=0x100000000;
    return[(v>>>24)&255,(v>>>16)&255,(v>>>8)&255,v&255];};
  const out=[];for(const c of[N0,N1,N2,D1,D2])out.push(...q31(c));
  return out;
}
const bqDefault=()=>({type:0,fHz:1000,q:0.707,gainDb:0});
function initBq(){
  const names=Object.keys(A.controls).filter(n=>parseBqName(n));
  const fresh={};
  for(const n of names)fresh[n]=A.bq[n]||bqDefault();
  try{const st=JSON.parse(localStorage.getItem("biquadParams")||"{}");
    for(const n of names)if(st[n])Object.assign(fresh[n],st[n]);}catch(e){}
  A.bq=fresh;
}
function setBq(name,partial,noMirror){
  const c=A.controls[name];if(!c)return;
  const cur={...(A.bq[name]||bqDefault()),...partial};
  A.bq[name]=cur;
  /* V13.3 : paire mic liée → la bande jumelle (CH1↔CH2, même TAC)
   * reçoit les mêmes paramètres */
  if(!noMirror&&!A.isOut&&A.idx<8&&window.LINKS&&window.LINKS[A.idx>>1]===1){
    const m=parseBqName(name);
    if(m&&m.chain==="ADC"){
      const mine=[1,5,9,2,6,10],twin=[2,6,10,1,5,9];
      const pos=mine.indexOf(m.idx);
      if(pos>=0){
        const tn=`${m.tac} ADC BQ${twin[pos]} Coefs`;
        if(A.controls[tn])setBq(tn,partial,true);
      }
    }
  }
  try{localStorage.setItem("biquadParams",JSON.stringify(A.bq));}catch(e){}
  clearTimeout(thA["bq"+c.numid]);
  thA["bq"+c.numid]=setTimeout(()=>{
    fetch("/api/alsa/set",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({numid:c.numid,value:rbjBlob(cur.type,cur.fHz,cur.q,cur.gainDb,48000).join(",")})}).catch(()=>{});
  },60);
}

/* ---- blobs DRC / MULTIBAND_DRC (ports _parse/_pack, formules drc_gen_coefs.m) ---- */
function parseDrcParams(view,off){
  const r=i=>view.getInt32(off+i*4,true);
  const inv=r(13),att_s=(inv?1/q30f(inv):0)/48000;
  return{enabled:r(0),threshold_dB:q24f(r(1)),knee_dB:q24f(r(2)),ratio:q24f(r(3)),
    pre_delay_ms:q30f(r(4))*1000,attack_ms:att_s*1000,
    master_gain_dB:mag2db(q24f(r(12))),release_spacing_dB:r(16),
    _raw:new Uint8Array(view.buffer.slice(view.byteOffset+off,view.byteOffset+off+88))};
}
function packDrcParams(p,Fs){
  Fs=Fs||48000;
  const buf=new Uint8Array(88);buf.set(p._raw);
  const dv=new DataView(buf.buffer),w=(i,v)=>dv.setInt32(i*4,v|0,true);
  const thr=+p.threshold_dB||0,knee=+p.knee_dB||0,ratio=Math.max(1.0001,+p.ratio||1);
  /* clamp 0.05 ms (l'ancien 1 ms altérait les attacks réels < 1 ms —
   * bug attrapé par l'autotest round-trip du natif N5, fix miroir) */
  const pre=(+p.pre_delay_ms||0)/1000,att=Math.max(0.00005,(+p.attack_ms||0)/1000);
  const mlin=db2mag(+p.master_gain_dB||0);
  const lt=db2mag(thr),slope=1/ratio,kt=db2mag(thr+knee);
  const slopeAt=(x,k)=>{
    if(x<lt)return 1;
    const kc=xv=>lt+(1-Math.exp(-k*(xv-lt)))/k;
    const x2=x*1.001;
    return(mag2db(kc(x2))-mag2db(kc(x)))/(mag2db(x2)-mag2db(x));};
  let mn=0.1,mx=10000,K=5;
  for(let it=0;it<15;it++){const sl=slopeAt(kt,K);if(sl<slope)mx=K;else mn=K;K=Math.sqrt(mn*mx);}
  const ka=lt+1/K,kb=-Math.exp(K*lt)/K;
  const kc=x=>x<lt?x:lt+(1-Math.exp(-K*(x-lt)))/K;
  const rbase=kc(kt)*Math.pow(kt,-slope);
  w(0,p.enabled?1:0);w(1,fq24(thr));w(2,fq24(knee));w(3,fq24(ratio));
  w(4,fq30(pre));w(5,fq30(lt));w(6,fq30(slope));
  w(7,Math.round(K*(1<<20))|0);w(8,fq24(ka));w(9,fq24(kb));w(10,fq24(kt));
  w(11,fq30(rbase));w(12,fq24(mlin));w(13,fq30(1/(att*Fs)));
  w(16,Math.round(+p.release_spacing_dB||0));
  return buf;
}
function parseDrcCfg(payload){
  const view=new DataView(payload.buffer,payload.byteOffset,payload.byteLength);
  const size=view.getUint32(0,true),reservedBytes=payload.slice(4,20);
  const N=Math.max(1,Math.floor((size-DRC_CFG_HDR)/DRC_PARAMS_SIZE));
  const params=[];
  for(let i=0;i<N;i++)params.push(parseDrcParams(view,DRC_CFG_HDR+i*DRC_PARAMS_SIZE));
  return{size,reservedBytes,params,params_per_band:N};
}
function crossBq(fs,fc,hp){
  let cut=fc/(fs/2);if(cut<0)cut=0;if(cut>1)cut=1;
  if(cut===0||cut===1){const b0=hp?(1-cut):cut;return{a1:0,a2:0,b0,b1:0,b2:0};}
  const d=Math.SQRT2,th=Math.PI*cut,sn=0.5*d*Math.sin(th);
  const beta=0.5*(1-sn)/(1+sn),gamma=(0.5+beta)*Math.cos(th);
  const alpha=0.25*(0.5+beta+(hp?gamma:-gamma));
  return{b0:2*alpha,b1:hp?-4*alpha:4*alpha,b2:2*alpha,a1:-2*gamma,a2:2*beta};
}
function bqBytes(bq){
  const buf=new Uint8Array(28),dv=new DataView(buf.buffer);
  dv.setInt32(0,fq30(-bq.a2),true);dv.setInt32(4,fq30(-bq.a1),true);
  dv.setInt32(8,fq30(bq.b2),true);dv.setInt32(12,fq30(bq.b1),true);
  dv.setInt32(16,fq30(bq.b0),true);dv.setInt32(20,0,true);dv.setInt32(24,16384,true);
  return buf;
}
function flatBq(){
  const buf=new Uint8Array(28),dv=new DataView(buf.buffer);
  dv.setInt32(16,1<<30,true);dv.setInt32(24,16384,true);
  return buf;
}
function packCross(nb,fs,fcs){
  const out=new Uint8Array(168);let lps,hps;
  if(nb<=1){for(let i=0;i<3;i++){out.set(flatBq(),i*56);out.set(flatBq(),i*56+28);}return out;}
  if(nb===2){lps=[crossBq(fs,fcs.low,false),null,null];hps=[crossBq(fs,fcs.low,true),null,null];}
  else if(nb===3){lps=[crossBq(fs,fcs.low,false),crossBq(fs,fcs.high,false),crossBq(fs,fcs.high,false)];
    hps=[crossBq(fs,fcs.low,true),crossBq(fs,fcs.high,true),crossBq(fs,fcs.high,true)];}
  else{lps=[crossBq(fs,fcs.low,false),crossBq(fs,fcs.mid,false),crossBq(fs,fcs.high,false)];
    hps=[crossBq(fs,fcs.low,true),crossBq(fs,fcs.mid,true),crossBq(fs,fcs.high,true)];}
  for(let i=0;i<3;i++){
    out.set(lps[i]?bqBytes(lps[i]):flatBq(),i*56);
    out.set(hps[i]?bqBytes(hps[i]):flatBq(),i*56+28);
  }
  return out;
}
function fcFromBq(bytes,off,fs){
  const dv=new DataView(bytes.buffer,bytes.byteOffset+off,28);
  const a2=q30f(dv.getInt32(0,true)),a1=q30f(dv.getInt32(4,true));
  const beta=-a2/2,gamma=a1/2;
  if(Math.abs(beta)<1e-9&&Math.abs(gamma)<1e-9)return NaN;
  const sn=(1-2*beta)/(1+2*beta);
  let s=sn*Math.SQRT2;if(s<-1)s=-1;if(s>1)s=1;
  const c=(0.5+beta)===0?0:gamma/(0.5+beta);
  let th=Math.asin(s);if(c<0)th=Math.PI-th;
  return th*fs/(2*Math.PI);
}
function extractFcs(cb,nb,fs){
  fs=fs||48000;
  const fcs={low:200,mid:2000,high:5000};
  if(nb<2)return fcs;
  const f0=fcFromBq(cb,0,fs);if(!isNaN(f0))fcs.low=Math.round(f0);
  if(nb===3){const f1=fcFromBq(cb,56,fs);if(!isNaN(f1))fcs.high=Math.round(f1);}
  else if(nb===4){const f1=fcFromBq(cb,56,fs),f2=fcFromBq(cb,112,fs);
    if(!isNaN(f1))fcs.mid=Math.round(f1);if(!isNaN(f2))fcs.high=Math.round(f2);}
  return fcs;
}
function parseMbCfg(payload){
  const view=new DataView(payload.buffer,payload.byteOffset,payload.byteLength);
  const size=view.getUint32(0,true),num_bands=view.getUint32(4,true),
        enable_emp_deemp=view.getUint32(8,true);
  const reservedBytes=payload.slice(12,44),empBytes=payload.slice(44,100),
        deempBytes=payload.slice(100,156),crossBytes=payload.slice(156,324);
  /* V10-FX blob V3 : détection par taille (miroir firmware + stripfx.js) —
   * section optionnelle crossover PAR CANAL (ppb×168 o) après drc_coef */
  const trailing=size-MBDRC_HDR;
  const oneBand=Math.max(1,num_bands)*DRC_PARAMS_SIZE, oneBandX=oneBand+168;
  let ppb, xoverPerCh=false;
  if(trailing%oneBandX===0&&trailing/oneBandX>1&&trailing/oneBandX<=8){
    ppb=trailing/oneBandX; xoverPerCh=true;
  }else{
    ppb=Math.max(1,Math.floor(trailing/oneBand));
  }
  const drc=[];
  for(let b=0;b<num_bands;b++){const row=[];
    for(let c=0;c<ppb;c++)
      row.push(parseDrcParams(view,MBDRC_HDR+(b*ppb+c)*DRC_PARAMS_SIZE));
    /* normalisation à 8 entrées (copies profondes) — pack per-channel */
    while(row.length<8){
      const src=row[row.length-1],cp={};
      for(const k in src)cp[k]=k==="_raw"?new Uint8Array(src._raw):src[k];
      row.push(cp);
    }
    drc.push(row);}
  const fcsGlobal=extractFcs(crossBytes,num_bands,48000);
  const fcsCh=[],xbase=MBDRC_HDR+num_bands*ppb*DRC_PARAMS_SIZE;
  for(let c=0;c<8;c++){
    if(xoverPerCh){
      const cc=Math.min(c,ppb-1);
      fcsCh.push(extractFcs(payload.slice(xbase+cc*168,xbase+(cc+1)*168),num_bands,48000));
    }else{
      fcsCh.push({low:fcsGlobal.low,mid:fcsGlobal.mid,high:fcsGlobal.high});
    }
  }
  return{size,num_bands,enable_emp_deemp,reservedBytes,empBytes,deempBytes,crossBytes,
    crossover_fcs:fcsGlobal,xover_per_ch:xoverPerCh,crossover_fcs_ch:fcsCh,
    drc,params_per_band:8};
}
function packDrcCfg(p){
  const N=p.params.length,tot=DRC_CFG_HDR+N*DRC_PARAMS_SIZE;
  const buf=new Uint8Array(tot);new DataView(buf.buffer).setUint32(0,tot,true);
  buf.set(p.reservedBytes,4);
  for(let i=0;i<N;i++)buf.set(packDrcParams(p.params[i]),DRC_CFG_HDR+i*DRC_PARAMS_SIZE);
  return buf;
}
function packMbCfg(p){
  /* V10-FX : blob V3 si xover par canal (8 sections de 168 o après drc_coef) */
  const xover=p.xover_per_ch===true, ppb=p.params_per_band;
  const tot=MBDRC_HDR+p.num_bands*ppb*DRC_PARAMS_SIZE+(xover?ppb*168:0);
  const buf=new Uint8Array(tot),dv=new DataView(buf.buffer);
  dv.setUint32(0,tot,true);dv.setUint32(4,p.num_bands,true);dv.setUint32(8,p.enable_emp_deemp,true);
  buf.set(p.reservedBytes,12);buf.set(p.empBytes,44);buf.set(p.deempBytes,100);buf.set(p.crossBytes,156);
  for(let b=0;b<p.num_bands;b++)for(let c=0;c<ppb;c++)
    buf.set(packDrcParams(p.drc[b][c]),MBDRC_HDR+(b*ppb+c)*DRC_PARAMS_SIZE);
  if(xover){
    const xbase=MBDRC_HDR+p.num_bands*ppb*DRC_PARAMS_SIZE;
    for(let c=0;c<ppb;c++)
      buf.set(packCross(p.num_bands,48000,p.crossover_fcs_ch[c]),xbase+c*168);
  }
  return buf;
}
const drcDefaults=()=>({enabled:1,threshold_dB:-30,knee_dB:20,ratio:10,pre_delay_ms:6,
  attack_ms:3,master_gain_dB:3,release_spacing_dB:5,_raw:new Uint8Array(88)});
function loadBlob(numid,fullName,done){
  const ex=A.blobs[numid];
  if(ex&&(ex.loading||(ex.parsed&&!ex.error))){done&&done();return;}
  A.blobs[numid]=A.blobs[numid]||{bandTab:0};
  Object.assign(A.blobs[numid],{loading:true,error:null,fullName,applying:false,dirty:false});
  fetch("/api/dsp/blob/"+numid+"/raw").then(r=>r.json()).then(j=>{
    if(!j.ok)throw new Error(j.err||"blob read failed");
    const raw=hex2b(j.hex);
    if(raw.length<ABI_HDR+4)throw new Error("blob too small");
    const abiHdr=raw.slice(0,ABI_HDR),payload=raw.slice(ABI_HDR);
    const kind=isMb(fullName)?"multiband":"drc";
    const parsed=kind==="multiband"?parseMbCfg(payload):parseDrcCfg(payload);
    Object.assign(A.blobs[numid],{loading:false,kind,parsed,abiHdr,error:null});
  }).catch(e=>{Object.assign(A.blobs[numid],{loading:false,error:e.message});})
    .finally(()=>done&&done());
}
function blobParams(numid){
  const b=A.blobs[numid];if(!b||!b.parsed)return null;
  const ch=A.idx%8;
  if(b.kind==="multiband"){
    const band=b.bandTab|0;
    if(band>=b.parsed.drc.length)return null;
    const row=b.parsed.drc[band];
    return row[Math.min(ch,row.length-1)]||null;
  }
  const arr=b.parsed.params;
  return arr[Math.min(ch,arr.length-1)]||null;
}
function applyBlob(numid,btn){
  const b=A.blobs[numid];if(!b||!b.parsed||b.applying)return;
  b.applying=true;if(btn)btn.disabled=true;
  /* V13.3 : paire liée → le canal édité est recopié sur le jumeau */
  if(!A.isOut&&A.idx<16&&window.LINKS&&window.LINKS[A.idx>>1]===1){
    const ch=A.idx%8,pc=(A.idx^1)%8,pr=b.parsed;
    if(b.kind==="multiband"&&pr.drc){
      pr.drc.forEach(row=>{
        if(ch<row.length&&pc<row.length)
          row[pc]={...row[ch],_raw:row[ch]._raw?new Uint8Array(row[ch]._raw):row[pc]._raw};
      });
      if(pr.crossover_fcs_ch&&pr.crossover_fcs_ch[ch])
        pr.crossover_fcs_ch[pc]={...pr.crossover_fcs_ch[ch]};
    }else if(pr.params&&ch<pr.params.length&&pc<pr.params.length){
      pr.params[pc]={...pr.params[ch],_raw:pr.params[ch]._raw?new Uint8Array(pr.params[ch]._raw):pr.params[pc]._raw};
    }
  }
  const cfg=b.kind==="multiband"?packMbCfg(b.parsed):packDrcCfg(b.parsed);
  const full=new Uint8Array(b.abiHdr.length+cfg.length);
  full.set(b.abiHdr,0);full.set(cfg,b.abiHdr.length);
  fetch("/api/dsp/blob/set",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({numid,hex:b2hex(full)})})
  .then(r=>r.json()).then(j=>{if(!j.ok)throw new Error(j.err||"write failed");b.dirty=false;})
  .catch(e=>alert("Échec écriture blob : "+e.message))
  .finally(()=>{b.applying=false;if(btn){btn.disabled=false;render();}});
}

/* ============================ rendu ============================ */
function el(tag,cls,txt){const e=document.createElement(tag);
  if(cls)e.className=cls;if(txt!==undefined)e.textContent=txt;return e;}
function row(label){const r=el("div","sfrow");r.appendChild(el("span","lb",label));return r;}
function slider(min,max,step,val,oninput){
  const s=document.createElement("input");s.type="range";
  s.min=min;s.max=max;s.step=step;s.value=val;
  s.addEventListener("input",()=>oninput(parseFloat(s.value)));
  return s;
}
function pwr(on,ontoggle){
  const p=el("div","pwrsw"+(on?" on":""));p.appendChild(el("i"));
  p.addEventListener("click",()=>{p.classList.toggle("on");ontoggle(p.classList.contains("on"));});
  return p;
}
function kcell(label,node){
  const u=el("div","kcell");
  u.appendChild(node);
  u.appendChild(el("span","kn",label.toUpperCase().slice(0,16)));
  return u;
}
function rowFor(c){
  if(c.type==="INTEGER"){
    /* knob console ; domaine dB quand le kcontrol expose son TLV */
    if(c.dbStep!==undefined&&c.dbMin!==undefined){
      const lo=c.min||0;
      return MXKNOB({label:c.short,min:c.dbMin,max:c.dbMin+((c.max||0)-lo)*c.dbStep,
        val:c.dbMin+(intVal(c)-lo)*c.dbStep,unit:"dB",step:c.dbStep>=1?1:0.5,
        onchange:v=>setAlsa(c,Math.round((v-c.dbMin)/c.dbStep)+lo)});
    }
    return MXKNOB({label:c.short,min:c.min||0,max:c.max||100,val:intVal(c),
      unit:"",step:1,onchange:v=>setAlsa(c,Math.round(v))});
  }
  if(c.type==="BOOLEAN")
    return kcell(c.short,pwr(isOn(c),on=>setAlsa(c,on?"on":"off")));
  if(c.type==="ENUMERATED"){
    const sel=document.createElement("select");
    const cur=String(c.value||"").replace(/'/g,"");
    c.items.forEach((it,ix)=>{const o=document.createElement("option");
      o.value=it;o.textContent=it;
      if(cur===it||parseInt(cur,10)===ix)o.selected=true;sel.appendChild(o);});
    sel.addEventListener("change",()=>setAlsa(c,sel.value));
    return kcell(c.short,sel);
  }
  return kcell(c.short||"?",el("span","kn",c.type));
}
/* ============ V13-EQ : égaliseur paramétrique PRO (biquads TAC) ============
 * Courbe de réponse interactive (drag des points : X=fréquence, Y=gain —
 * ou Q pour les types sans gain ; molette = Q) + champs, + FFT temps réel
 * de la voie en fond (tap 2 de l'analyseur pointé sur la tranche).
 * 3 biquads par canal (mapping matériel TAC5212 : CH1=BQ1/5/9, CH2=BQ2/6/10). */
const EQ_COLORS=["#e5a13c","#4cc470","#5aa9e6"];
function bqCoeffs(p){
  /* mêmes formules RBJ que rbjBlob, coefficients normalisés (pour |H|) */
  const fs=48000,f=Math.max(20,Math.min(p.fHz,22000)),q=Math.max(0.1,Math.min(p.q,16));
  const g=Math.max(-24,Math.min(p.gainDb||0,24));
  const w0=2*Math.PI*f/fs,cw=Math.cos(w0),sw=Math.sin(w0),al=sw/(2*q);
  const Aa=Math.pow(10,g/40),be=Math.sqrt(Aa)/q;
  let b0,b1,b2,a0,a1,a2;
  switch(+p.type|0){
    case 1:b0=(1-cw)/2;b1=1-cw;b2=(1-cw)/2;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 2:b0=(1+cw)/2;b1=-(1+cw);b2=(1+cw)/2;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 3:b0=al;b1=0;b2=-al;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 4:b0=1;b1=-2*cw;b2=1;a0=1+al;a1=-2*cw;a2=1-al;break;
    case 5:b0=1+al*Aa;b1=-2*cw;b2=1-al*Aa;a0=1+al/Aa;a1=-2*cw;a2=1-al/Aa;break;
    case 6:b0=Aa*((Aa+1)-(Aa-1)*cw+be*sw);b1=2*Aa*((Aa-1)-(Aa+1)*cw);
      b2=Aa*((Aa+1)-(Aa-1)*cw-be*sw);a0=(Aa+1)+(Aa-1)*cw+be*sw;
      a1=-2*((Aa-1)+(Aa+1)*cw);a2=(Aa+1)+(Aa-1)*cw-be*sw;break;
    case 7:b0=Aa*((Aa+1)+(Aa-1)*cw+be*sw);b1=-2*Aa*((Aa-1)+(Aa+1)*cw);
      b2=Aa*((Aa+1)+(Aa-1)*cw-be*sw);a0=(Aa+1)-(Aa-1)*cw+be*sw;
      a1=2*((Aa-1)-(Aa+1)*cw);a2=(Aa+1)-(Aa-1)*cw-be*sw;break;
    case 8:b0=1-al;b1=-2*cw;b2=1+al;a0=1+al;a1=-2*cw;a2=1-al;break;
    default:return null;
  }
  return{b0:b0/a0,b1:b1/a0,b2:b2/a0,a1:a1/a0,a2:a2/a0};
}
function bqMagDb(c,f){
  const w=2*Math.PI*f/48000,cw=Math.cos(w),c2=Math.cos(2*w),sw=Math.sin(w),s2=Math.sin(2*w);
  const nr=c.b0+c.b1*cw+c.b2*c2, ni=-(c.b1*sw+c.b2*s2);
  const dr=1+c.a1*cw+c.a2*c2,  di=-(c.a1*sw+c.a2*s2);
  return 10*Math.log10(((nr*nr+ni*ni)+1e-20)/((dr*dr+di*di)+1e-20));
}
function eqPanel(bqs){
  const post=(o)=>fetch("/api/cmd",{method:"POST",
    headers:{"Content-Type":"application/json"},body:JSON.stringify(o)})
    .then(r=>r.json()).catch(()=>null);
  const wrap=el("div","fxcard");
  const ct=el("div","ct","ÉGALISEUR PARAMÉTRIQUE");
  const em=document.createElement("em");
  em.textContent=A.isOut
    ?"  ·  1 biquad TAC du canal (BQ5/9 réservés anti-larsen) — drag : fréquence/gain · molette : Q"
    :"  ·  3 biquads TAC du canal — drag : fréquence/gain · molette : Q";
  ct.appendChild(em);wrap.appendChild(ct);

  /* BQ vivants de CE canal (mapping modulo-4 du TAC5212 : CH1=BQ1/5/9,
   * CH2=BQ2/6/10). Côté DAC (sorties) l'anti-larsen possède BQ5/9 et
   * BQ6/10 → seul BQ1 (resp. BQ2) reste à l'utilisateur. */
  const chIn=(A.idx%2);
  const idxs=A.isOut?(chIn===0?[1]:[2]):(chIn===0?[1,5,9]:[2,6,10]);
  const bands=idxs.map(i=>bqs.find(c=>{const m=parseBqName(c.fullName);return m&&+m.idx===i;}))
                  .filter(Boolean);
  if(!bands.length){wrap.appendChild(el("div","sfsub","Biquads indisponibles."));return wrap;}

  /* le TAC n'insère que 'N Biquads/Ch' dans le chemin (config partagée) :
   * il faut '3 Biquads/Ch' pour que BQ5/9 (resp. 6/10) agissent — même
   * forçage que le daemon anti-larsen côté DAC. Appliqué à la 1ʳᵉ
   * activation d'une bande, jamais au simple affichage. */
  function ensure3(){
    const tac=parseBqName(bands[0].fullName).tac;
    const cfg=A.controls[tac+" "+(A.isOut?"DAC":"ADC")+" Biquad Config"];
    if(!cfg)return;
    const cur=String(cfg.value||"").replace(/'/g,"");
    if(cur==="3"||/3 Biquads/.test(cur))return;
    setAlsa(cfg,"3 Biquads/Ch");
  }

  const W=1180,H=320,FMIN=20,FMAX=20000,DBR=18;
  const cv=document.createElement("canvas");
  cv.width=W;cv.height=H;
  cv.style.cssText="width:100%;height:320px;background:#0b0e11;border:1px solid #22282e;border-radius:6px;touch-action:none;display:block;";
  wrap.appendChild(cv);
  const ctx=cv.getContext("2d");
  const xOf=f=>W*Math.log(f/FMIN)/Math.log(FMAX/FMIN);
  const fOf=x=>FMIN*Math.pow(FMAX/FMIN,x/W);
  const yOf=db=>H/2-db*(H/2)/DBR;
  const dbOfY=y=>(H/2-y)*DBR/(H/2);

  let fft=null;   /* s[128] int8 dB, bins déjà log 20→20k côté analyzer */
  function draw(){
    ctx.clearRect(0,0,W,H);
    if(fft){
      ctx.beginPath();ctx.moveTo(0,H);
      for(let b=0;b<fft.length;b++){
        const x=(b+0.5)*W/fft.length;
        const y=H-Math.max(0,Math.min(1,(fft[b]+96)/96))*H;
        ctx.lineTo(x,y);
      }
      ctx.lineTo(W,H);ctx.closePath();
      ctx.fillStyle="rgba(76,196,112,.14)";ctx.fill();
      ctx.strokeStyle="rgba(76,196,112,.45)";ctx.lineWidth=1;ctx.stroke();
    }
    ctx.font="10px monospace";ctx.lineWidth=1;
    [50,100,200,500,1000,2000,5000,10000].forEach(f=>{
      const x=xOf(f);
      ctx.strokeStyle="#1b2126";
      ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,H);ctx.stroke();
      ctx.fillStyle="#5c666e";
      ctx.fillText(f>=1000?(f/1000)+"k":f,x+3,H-5);
    });
    [-12,-6,0,6,12].forEach(db=>{
      const y=yOf(db);
      ctx.strokeStyle=db===0?"#2c343c":"#1b2126";
      ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();
      ctx.fillStyle="#5c666e";
      ctx.fillText((db>0?"+":"")+db,W-28,y-3);
    });
    const N=240,sum=new Float32Array(N);
    bands.forEach((c,bi)=>{
      const p=A.bq[c.fullName]||bqDefault();
      if(p.type===0)return;
      const co=bqCoeffs(p);if(!co)return;
      ctx.beginPath();
      for(let i=0;i<N;i++){
        const f=fOf(i*W/(N-1)),db=bqMagDb(co,f);
        sum[i]+=db;
        const y=yOf(Math.max(-DBR,Math.min(DBR,db)));
        i?ctx.lineTo(i*W/(N-1),y):ctx.moveTo(0,y);
      }
      ctx.strokeStyle=EQ_COLORS[bi]+"55";ctx.lineWidth=1.2;ctx.stroke();
    });
    ctx.beginPath();
    for(let i=0;i<N;i++){
      const y=yOf(Math.max(-DBR,Math.min(DBR,sum[i])));
      i?ctx.lineTo(i*W/(N-1),y):ctx.moveTo(0,y);
    }
    ctx.strokeStyle="#e5a13c";ctx.lineWidth=2.5;ctx.stroke();
    bands.forEach((c,bi)=>{
      const p=A.bq[c.fullName]||bqDefault();
      const on=p.type!==0;
      const gy=[5,6,7].includes(p.type)?p.gainDb:0;
      const x=xOf(Math.max(FMIN,Math.min(FMAX,p.fHz))),y=yOf(Math.max(-DBR,Math.min(DBR,gy)));
      ctx.beginPath();ctx.arc(x,y,9,0,7);
      ctx.fillStyle=on?EQ_COLORS[bi]:"#14181c";ctx.fill();
      ctx.strokeStyle=EQ_COLORS[bi];ctx.lineWidth=2;ctx.stroke();
      ctx.fillStyle=on?"#0b0e11":EQ_COLORS[bi];
      ctx.font="bold 10px monospace";ctx.fillText(bi+1,x-3,y+3.5);
    });
  }

  let drag=-1;
  const hit=(mx,my)=>{
    let best=-1,bd=26;
    bands.forEach((c,bi)=>{
      const p=A.bq[c.fullName]||bqDefault();
      const gy=[5,6,7].includes(p.type)?p.gainDb:0;
      const d=Math.hypot(mx-xOf(p.fHz),my-yOf(gy));
      if(d<bd){bd=d;best=bi;}
    });
    return best;
  };
  const evPos=e=>{const r=cv.getBoundingClientRect();
    return[(e.clientX-r.left)*W/r.width,(e.clientY-r.top)*H/r.height];};
  cv.addEventListener("pointerdown",e=>{
    const[mx,my]=evPos(e);drag=hit(mx,my);
    if(drag>=0){cv.setPointerCapture(e.pointerId);e.preventDefault();}
  });
  cv.addEventListener("pointermove",e=>{
    if(drag<0)return;
    const[mx,my]=evPos(e);
    const c=bands[drag],p=A.bq[c.fullName]||bqDefault();
    const upd={fHz:Math.round(Math.max(FMIN,Math.min(FMAX,fOf(mx))))};
    if(p.type===0){upd.type=5;ensure3();}        /* drag d'une bande OFF → peaking */
    if([5,6,7].includes(upd.type??p.type))
      upd.gainDb=Math.round(Math.max(-DBR,Math.min(DBR,dbOfY(my)))*10)/10;
    else
      upd.q=Math.round(Math.max(0.1,Math.min(10,(H-my)/H*10))*100)/100;
    setBq(c.fullName,upd);syncRows();draw();
  });
  const endDrag=()=>{drag=-1;};
  cv.addEventListener("pointerup",endDrag);cv.addEventListener("pointercancel",endDrag);
  cv.addEventListener("wheel",e=>{
    const[mx,my]=evPos(e);const bi=hit(mx,my);
    if(bi<0)return;e.preventDefault();
    const c=bands[bi],p=A.bq[c.fullName]||bqDefault();
    const q=Math.round(Math.max(0.1,Math.min(16,p.q*(e.deltaY<0?1.12:0.89)))*100)/100;
    setBq(c.fullName,{q});syncRows();draw();
  },{passive:false});

  const rows=el("div");rows.style.cssText="display:flex;flex-direction:column;gap:4px;margin-top:8px;";
  wrap.appendChild(rows);
  const inputs=[];
  bands.forEach((c,bi)=>{
    const p=A.bq[c.fullName]||bqDefault();
    const r=el("div");
    r.style.cssText="display:flex;align-items:center;gap:8px;";
    r.innerHTML=`
      <span style="width:14px;height:14px;border-radius:7px;background:${EQ_COLORS[bi]};"></span>
      <button class="wbtn" data-eqon style="width:64px;">${p.type!==0?"ON":"OFF"}</button>
      <select data-eqt style="background:#1b2126;color:#c9c4b8;border:1px solid #39434b;border-radius:4px;padding:6px;">
        ${BIQUAD_TYPES.map((t,ti)=>`<option value="${ti}" ${p.type===ti?"selected":""}>${t}</option>`).join("")}
      </select>
      <label style="font-size:9px;color:var(--mut);">F<input data-eqf type="number" min="20" max="20000" step="1" value="${Math.round(p.fHz)}"
        style="width:74px;background:#0b0e11;color:#e9e5da;border:1px solid #39434b;border-radius:4px;padding:5px;margin-left:4px;"> Hz</label>
      <label style="font-size:9px;color:var(--mut);">G<input data-eqg type="number" min="-18" max="18" step="0.5" value="${p.gainDb}"
        style="width:64px;background:#0b0e11;color:#e9e5da;border:1px solid #39434b;border-radius:4px;padding:5px;margin-left:4px;"> dB</label>
      <label style="font-size:9px;color:var(--mut);">Q<input data-eqq type="number" min="0.1" max="16" step="0.05" value="${p.q}"
        style="width:64px;background:#0b0e11;color:#e9e5da;border:1px solid #39434b;border-radius:4px;padding:5px;margin-left:4px;"></label>
      <span style="font-size:9px;color:var(--mut);">BQ${parseBqName(c.fullName).idx}</span>`;
    rows.appendChild(r);
    inputs.push(r);
    r.querySelector("[data-eqon]").addEventListener("click",()=>{
      const q=A.bq[c.fullName]||bqDefault();
      if(q.type===0)ensure3();
      setBq(c.fullName,{type:q.type===0?(q.lastType||5):0,
                        lastType:q.type!==0?q.type:(q.lastType||5)});
      syncRows();draw();
    });
    r.querySelector("[data-eqt]").addEventListener("change",e=>{
      if(+e.target.value!==0)ensure3();
      setBq(c.fullName,{type:+e.target.value});syncRows();draw();});
    r.querySelector("[data-eqf]").addEventListener("change",e=>{
      setBq(c.fullName,{fHz:Math.max(20,Math.min(20000,+e.target.value||1000))});draw();});
    r.querySelector("[data-eqg]").addEventListener("change",e=>{
      setBq(c.fullName,{gainDb:Math.max(-18,Math.min(18,+e.target.value||0))});draw();});
    r.querySelector("[data-eqq]").addEventListener("change",e=>{
      setBq(c.fullName,{q:Math.max(0.1,Math.min(16,+e.target.value||0.707))});draw();});
  });
  function syncRows(){
    bands.forEach((c,bi)=>{
      const p=A.bq[c.fullName]||bqDefault(),r=inputs[bi];
      const on=r.querySelector("[data-eqon]");
      on.textContent=p.type!==0?"ON":"OFF";
      on.style.color=p.type!==0?EQ_COLORS[bi]:"";
      r.querySelector("[data-eqt]").value=p.type;
      r.querySelector("[data-eqf]").value=Math.round(p.fHz);
      r.querySelector("[data-eqg]").value=p.gainDb;
      r.querySelector("[data-eqq]").value=p.q;
    });
  }

  /* FFT de la voie : tap 2 de l'analyseur pointé sur CETTE tranche
   * (entrée pour les strips IN, sortie pour les strips OUT).
   * Cibles à 10 Hz + ballistique 25 Hz (poser les valeurs brutes à basse
   * cadence = saccades). Nettoyé par render() (_eqCleanup). */
  const kind=A.isOut?3:1;
  post({op:"set_tap",tap:2,kind:kind,a:A.idx,b:-1});
  let fftT=null;
  const timer=setInterval(()=>{
    post({op:"get_meters"}).then(j=>{
      if(!j)return;
      const t=(j.analyzer||[]).find(a=>a.k===kind&&a.a===A.idx);
      if(t&&t.s)fftT=t.s;
    });
  },100);
  const anim=setInterval(()=>{
    if(!fftT)return;
    if(!fft){fft=fftT.slice();draw();return;}
    const kA=0.74,kR=0.28;   /* dt 40 ms : attaque τ30 ms, retombée τ120 ms */
    let mv=0;
    for(let b=0;b<fft.length;b++){
      const d=fftT[b]-fft[b];
      fft[b]+=d*(d>0?kA:kR);
      mv=Math.max(mv,Math.abs(d));
    }
    if(mv>0.5)draw();
  },40);
  window._eqCleanup=()=>{clearInterval(timer);clearInterval(anim);
    post({op:"set_tap",tap:2,kind:0,a:0,b:-1});window._eqCleanup=null;};
  draw();
  return wrap;
}

function bqEditor(c){
  const blk=el("div","bqblk");
  const info=parseBqName(c.fullName);
  blk.appendChild(el("span","bqn","BQ"+(info?info.idx:"?")));
  const p=A.bq[c.fullName]||bqDefault();
  const sel=document.createElement("select");
  BIQUAD_TYPES.forEach((t,ti)=>{const o=document.createElement("option");
    o.value=ti;o.textContent=t;if(p.type===ti)o.selected=true;sel.appendChild(o);});
  blk.appendChild(sel);
  const params=el("div","kgridf");params.style.flex="1";blk.appendChild(params);
  function renderParams(){
    params.innerHTML="";
    const q=A.bq[c.fullName]||bqDefault();
    if(q.type===0)return;
    params.appendChild(MXKNOB({label:"Fréquence",min:20,max:22000,val:q.fHz,unit:"Hz",
      log:true,step:1,onchange:v=>setBq(c.fullName,{fHz:Math.round(v)})}));
    params.appendChild(MXKNOB({label:"Q",min:0.1,max:10,val:q.q,unit:"",step:0.01,
      onchange:v=>setBq(c.fullName,{q:v})}));
    if([5,6,7].includes(q.type))
      params.appendChild(MXKNOB({label:"Gain",min:-24,max:24,val:q.gainDb,unit:"dB",
        step:0.1,onchange:v=>setBq(c.fullName,{gainDb:v})}));
  }
  sel.addEventListener("change",()=>{setBq(c.fullName,{type:parseInt(sel.value,10)});renderParams();});
  renderParams();
  return blk;
}
function drcEditor(c){
  const wrap=el("div","fxcard");
  const ct=el("div","ct",c.short);
  const em=document.createElement("em");
  em.textContent="  ·  compresseur DSP — canal "+(A.isOut?"S":"M")+((A.idx%8)+1);
  ct.appendChild(em);wrap.appendChild(ct);
  const inner=el("div");wrap.appendChild(inner);
  function renderBlob(){
    inner.innerHTML="";
    const b=A.blobs[c.numid];
    if(!b||b.loading){inner.appendChild(el("div","sfsub","Chargement du blob DSP…"));return;}
    if(b.error){inner.appendChild(el("div","sfsub","Erreur : "+b.error));return;}
    const p=blobParams(c.numid);
    if(!p){inner.appendChild(el("div","sfsub","Paramètres indisponibles."));return;}
    const dirt=el("span","dirt","● modifié — non appliqué");
    const touch=()=>{b.dirty=true;dirt.style.display="";};
    if(b.kind==="multiband"){
      const nb=b.parsed.num_bands;
      const tabs=el("div","sftabs");tabs.style.padding="0 0 8px";tabs.style.border="none";
      for(let i=0;i<nb;i++){
        const t=el("span","sftab"+(b.bandTab===i?" on":""),"BANDE "+(i+1));
        t.addEventListener("click",()=>{b.bandTab=i;renderBlob();});
        tabs.appendChild(t);
      }
      inner.appendChild(tabs);
      /* V10-FX : crossover PAR CANAL (blob V3) — le knob édite le canal
       * courant, le 1er réglage bascule le blob en layout V3 */
      const xch=A.idx%8;
      const fcs=b.parsed.crossover_fcs_ch[xch];
      const xk=nb===2?["low"]:nb===3?["low","high"]:["low","mid","high"];
      const xg=el("div","kgridf");xg.style.marginBottom="8px";
      xk.forEach(k=>{
        xg.appendChild(MXKNOB({label:"Xover "+k+" (canal)",min:20,max:20000,val:fcs[k],unit:"Hz",
          log:true,step:1,onchange:v=>{
            fcs[k]=Math.max(20,Math.min(Math.round(v),20000));
            b.parsed.xover_per_ch=true;touch();
          }}));
      });
      inner.appendChild(xg);
    }
    const head=el("div","drchead");
    head.appendChild(pwr(p.enabled===1,on=>{
      /* V10-FX : sur multiband, ACTIF/OFF = le canal sur TOUTES les bandes */
      if(b.kind==="multiband"){
        const ch=A.idx%8;
        for(const row of b.parsed.drc)row[Math.min(ch,row.length-1)].enabled=on?1:0;
      }else{
        p.enabled=on?1:0;
      }
      touch();
    }));
    head.appendChild(el("span","lb","ACTIF"));
    head.appendChild(dirt);
    inner.appendChild(head);
    const g=el("div","kgridf");
    DRC_SLIDERS.forEach(s=>{
      g.appendChild(MXKNOB({label:s.label,min:s.min,max:s.max,val:+p[s.key]||0,
        unit:s.unit,step:s.step,onchange:v=>{p[s.key]=v;touch();}}));
    });
    inner.appendChild(g);
    const bar=el("div","sfbar");
    const ap=el("button","apply","APPLIQUER AU DSP");
    ap.addEventListener("click",()=>applyBlob(c.numid,ap));
    const rs=el("button",null,"DÉFAUT");
    rs.addEventListener("click",()=>{
      const cur=blobParams(c.numid);if(!cur)return;
      const d=drcDefaults();d._raw=cur._raw;Object.assign(cur,d);
      b.dirty=true;renderBlob();
    });
    bar.appendChild(ap);bar.appendChild(rs);
    inner.appendChild(bar);
    dirt.style.display=b.dirty?"":"none";
  }
  loadBlob(c.numid,c.fullName,renderBlob);
  renderBlob();
  return wrap;
}
function dbOf(g){return g<=0.000316?-72:20*Math.log10(g);}
function dbTxt(db){return db<=-71?"-∞":db.toFixed(1)+" dB";}
function sendKnob(label,curGain,onDb){
  return MXKNOB({label,min:-72,max:12,val:Math.max(-72,dbOf(curGain)),
    unit:"dB",step:0.5,onchange:onDb});
}
function card(title,sub){
  const c=el("div","fxcard");
  const ct=el("div","ct",title);
  if(sub){const em=document.createElement("em");em.textContent="  ·  "+sub;ct.appendChild(em);}
  c.appendChild(ct);
  const g=el("div","kgridf");c.appendChild(g);
  return {c,g};
}
const thR={};
function postRt(key,o){
  clearTimeout(thR[key]);
  thR[key]=setTimeout(()=>{
    fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify(o)}).catch(()=>{});
  },45);
}
/* onglet ROUTAGE+SENDS (entrees) : valeurs REELLES via get_strip_routing */
function renderRouting(body){
  body.appendChild(el("div","sfsub","Envois de la tranche — valeurs lues du mixer"));
  const holder=el("div");body.appendChild(holder);
  holder.appendChild(el("div","sfsub","Lecture du routage…"));
  fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({op:"get_strip_routing",src:A.idx})})
  .then(r=>r.json()).then(j=>{
    holder.innerHTML="";
    if(!j||!j.ok||!Array.isArray(j.master)){
      holder.appendChild(el("div","sfsub","Routage indisponible ("+((j&&j.err)||"pas de réponse")+")"));
      return;
    }
    const cm=card("ENVOI MASTER","gains lus du mixer");
    cm.g.appendChild(sendKnob("Master L",j.master[0]||0,db=>{
      postRt("m0",{op:"set_master",src:A.idx,out:0,gain:db<=-71?0:Math.pow(10,db/20)});}));
    cm.g.appendChild(sendKnob("Master R",j.master[1]||0,db=>{
      postRt("m1",{op:"set_master",src:A.idx,out:1,gain:db<=-71?0:Math.pow(10,db/20)});}));
    holder.appendChild(cm.c);
    const cs=card("SENDS FX","paires stéréo bus 1-8");
    for(let b=0;b<4;b++){
      cs.g.appendChild(sendKnob("FX"+(b+1),j.sends?(j.sends[b*2]||0):0,(db=>{
        const gain=db<=-71?0:Math.pow(10,db/20);
        postRt("s"+b+"a",{op:"set_send","in":A.idx,bus:b*2,gain});
        postRt("s"+b+"b",{op:"set_send","in":A.idx,bus:b*2+1,gain});
      })));
    }
    holder.appendChild(cs.c);
  }).catch(()=>{
    holder.innerHTML="";
    holder.appendChild(el("div","sfsub","Erreur réseau — rouvre le panneau."));
  });
}
/* onglet SORTIE : gain reel (STORE.output_gain) */
function renderOutTab(body){
  let db=-72;
  const S=window.STORE;
  if(S&&S.output_gain&&S.output_gain.gains&&S.output_gain.gains[A.idx]!==undefined)
    db=20*Math.log10(Math.max(S.output_gain.gains[A.idx],1)/1000);
  const co=card("SORTIE","gain persisté du mixer");
  co.g.appendChild(MXKNOB({label:"Gain sortie",min:-72,max:12,val:Math.max(-72,db),
    unit:"dB",step:0.5,onchange:v=>postRt("og",{op:"set_output_gain",out:A.idx,db:v})}));
  body.appendChild(co.c);
}
/* V12-EXP web : onglet GATE — expandeur/gate natif mixer-pro par tranche */
function renderGateTab(body){
  const src=A.idx;
  const post=(o)=>fetch("/api/cmd",{method:"POST",
    headers:{"Content-Type":"application/json"},body:JSON.stringify(o)})
    .then(r=>r.json()).catch(()=>({ok:false}));
  const cg=card("GATE / EXPANDEUR","pré-fader : agit sur sends, master, looper, automix");
  cg.g.style.display="block";
  cg.g.innerHTML=`
    <div style="display:flex;align-items:center;gap:14px;margin-bottom:8px;">
      <button class="wbtn" id="gt_on" style="min-width:110px;">GATE —</button>
      <div>
        <div style="font-size:8px;color:var(--mut);letter-spacing:2px;">RÉDUCTION <span id="gt_grv">—</span></div>
        <div style="width:230px;height:12px;border-radius:6px;background:#0b0e11;overflow:hidden;">
          <div id="gt_gr" style="height:100%;width:0;background:#e05545;float:right;"></div></div>
      </div>
    </div>
    <div id="gt_rows"></div>`;
  body.appendChild(cg.c);
  const PAR=[["SEUIL","threshold_db",-80,0,"dB"],["RATIO","ratio",1,20,":1"],
    ["ATTACK","attack_ms",0.5,100,"ms"],["RELEASE","release_ms",5,1000,"ms"],
    ["RANGE","range_db",0,80,"dB"],["HOLD","hold_ms",0,500,"ms"]];
  let cur=null;
  function draw(){
    const rows=document.getElementById("gt_rows");
    if(!rows||!cur)return;
    const on=document.getElementById("gt_on");
    on.textContent=cur.on?"GATE ON":"GATE OFF";
    on.style.color=cur.on?"var(--accent)":"";
    on.style.borderColor=cur.on?"var(--accent)":"";
    if(!rows.dataset.built){
      rows.dataset.built="1";
      rows.innerHTML=PAR.map(([lbl,k,mn,mx,un])=>`
        <div style="display:flex;align-items:center;gap:10px;margin-bottom:4px;">
          <span style="width:90px;font-size:10px;font-weight:700;color:var(--mut);">${lbl}</span>
          <input type="range" data-gk="${k}" min="${mn}" max="${mx}" step="${k==="attack_ms"?0.5:1}"
                 value="${cur[k]}" style="flex:1;">
          <span style="width:70px;font-family:monospace;font-weight:700;" id="gtv_${k}">${cur[k]} ${un}</span>
        </div>`).join("");
      rows.querySelectorAll("[data-gk]").forEach(sl=>sl.addEventListener("input",()=>{
        const k=sl.dataset.gk,v=+sl.value;
        cur[k]=v;
        document.getElementById("gtv_"+k).textContent=v+" "+PAR.find(p=>p[1]===k)[4];
        const o={op:"set_expander",src};o[k]=v;post(o);
      }));
      on.addEventListener("click",()=>{post({op:"set_expander",src,on:cur.on?0:1});});
    }
  }
  function poll(){
    post({op:"get_expander"}).then(r=>{
      if(!r.ok||!document.getElementById("gt_rows"))return;
      const c=r.channels[src];
      if(!cur){cur=c;draw();}
      else{cur.on=c.on;draw();}
      const grv=document.getElementById("gt_grv"),gr=document.getElementById("gt_gr");
      if(grv){grv.textContent=c.gr_db.toFixed(1)+" dB";
        gr.style.width=Math.min(100,-c.gr_db/Math.max(1,c.range_db)*100)+"%";}
    });
  }
  poll();
  window._gateTimer=setInterval(poll,250);
}
function render(){
  const groups=groupsFor();
  /* onglets synthetiques : le panneau n'est JAMAIS vide */
  if(A.isOut)groups.unshift({title:"SORTIE",custom:"out"});
  else groups.unshift({title:"ROUTAGE + SENDS",custom:"routing"});
  /* V12-EXP web : gate par tranche (voies IN 0..15, natif mixer-pro) */
  if(!A.isOut&&A.idx<16)groups.splice(1,0,{title:"GATE",custom:"gate"});
  if(window._gateTimer){clearInterval(window._gateTimer);window._gateTimer=null;}
  if(window._eqCleanup)window._eqCleanup();
  const tabs=$("#sftabs"),body=$("#sfbody");
  tabs.innerHTML="";body.innerHTML="";
  const act=groups.find(g=>g.title===A.tab)||groups[0];A.tab=act.title;
  groups.forEach(g=>{
    const t=el("span","sftab"+(g===act?" on":""),g.title);
    t.addEventListener("click",()=>{A.tab=g.title;render();});
    tabs.appendChild(t);
  });
  if(act.custom==="routing"){renderRouting(body);return;}
  if(act.custom==="out"){renderOutTab(body);return;}
  if(act.custom==="gate"){renderGateTab(body);return;}
  const blobs=act.controls.filter(c=>c.type==="BYTES"&&isDrcBlob(c.fullName));
  const bqs=act.controls.filter(c=>c.type==="BYTES"&&c.fullName&&parseBqName(c.fullName));
  const plain=act.controls.filter(c=>blobs.indexOf(c)<0&&bqs.indexOf(c)<0);
  if(plain.length||act.placeholder||(!blobs.length&&!bqs.length)){
    const cp=card(act.title,(act.subtitle||"")+(A.diag?" · ALSA "+A.diag:""));
    if(act.placeholder)cp.g.appendChild(el("div","sfsub",act.placeholder));
    plain.forEach(c=>{
      try{cp.g.appendChild(rowFor(c));}
      catch(e){cp.g.appendChild(el("div","sfsub","⚠ "+(c.short||"?")+" : "+e.message));}
    });
    body.appendChild(cp.c);
  }
  if(bqs.length){
    /* V13-EQ : courbe paramétrique pro + FFT de la voie */
    try{body.appendChild(eqPanel(bqs));}
    catch(e){body.appendChild(el("div","sfsub","⚠ EQ : "+e.message));}
    /* accès expert aux 12 blobs bruts (autres canaux / BQ morts) */
    const det=document.createElement("details");
    det.style.cssText="margin-top:6px;";
    const sm=document.createElement("summary");
    sm.textContent="BIQUADS BRUTS ("+bqs.length+" filtres · forme RBJ)";
    sm.style.cssText="cursor:pointer;font-size:10px;letter-spacing:.12em;color:var(--mut);padding:6px 2px;";
    det.appendChild(sm);
    bqs.forEach(c=>{
      try{det.appendChild(bqEditor(c));}
      catch(e){det.appendChild(el("div","sfsub","⚠ "+(c.short||"?")+" : "+e.message));}
    });
    body.appendChild(det);
  }
  blobs.forEach(c=>{
    try{body.appendChild(drcEditor(c));}
    catch(e){body.appendChild(el("div","sfsub","⚠ "+(c.short||"?")+" : "+e.message));}
  });
}
function labelFor(idx,isOut){
  if(!isOut)return idx<8?["M"+(idx+1),"MIC DSP"]:idx<16?["U"+(idx-7),"USB IN"]:["P"+(idx-15),"TEL IN"];
  return idx<8?["S"+(idx+1),"DSP OUT"]:idx<16?["U"+(idx-7),"USB OUT"]:["P"+(idx-15),"TEL OUT"];
}
function openDrawer(mp){
  A.idx=mp.idx;A.isOut=(mp.t==="out");A.tab="";A.open=true;
  const[l,k]=labelFor(A.idx,A.isOut);
  $("#sftitle").textContent=l;$("#sfkind").textContent=k+" · EFFETS PISTE";
  $("#stripfx").style.display="flex";
  A.diag=null;
  loadControls();
}
function beacon(msg){
  try{navigator.sendBeacon("/api/client-log",
    "beta "+(document.getElementById("guiver")||{}).textContent+" | "+msg+" | "+navigator.userAgent.slice(0,60));}catch(_){}
}
/* mouchard : remonte la taille réelle du viewport au journal board */
if(window.PANEL)setTimeout(()=>{
  beacon("viewport "+innerWidth+"x"+innerHeight+" dpr="+devicePixelRatio+" screen "+screen.width+"x"+screen.height);
},3000);
function loadControls(){
  $("#sfbody").innerHTML='<div class="sfsub">Lecture des contrôles ALSA…</div>';
  const t0=performance.now();
  const ctl=new AbortController();
  const to=setTimeout(()=>ctl.abort(),8000);
  fetch("/api/alsa/contents",{signal:ctl.signal}).then(r=>{
    if(!r.ok)throw new Error("HTTP "+r.status);
    return r.text();
  }).then(t=>{
    clearTimeout(to);
    if(t.indexOf("numid=")<0)throw new Error("réponse invalide ("+t.slice(0,60)+")");
    A.controls=parseAmixer(t);
    const n=Object.keys(A.controls).length;
    A.diag=Math.round(performance.now()-t0)+" ms · "+n+" contrôles";
    if(n<300){A.diag+=" ⚠ INCOMPLET (attendu 339)";beacon("alsa/contents INCOMPLET n="+n);}
    initBq();render();
  }).catch(e=>{
    clearTimeout(to);
    A.controls={};A.diag=null;render();
    beacon("alsa/contents ECHEC: "+(e.name==="AbortError"?"timeout 8s":e.name+" "+e.message));
    const body=$("#sfbody");
    const msg=el("div","sfsub","⚠ Contrôles ALSA inaccessibles : "+(e.name==="AbortError"?"timeout 8 s":e.message)+" — les onglets TAC/DSP sont masqués.");
    msg.style.color="var(--warn)";
    body.insertBefore(msg,body.firstChild);
    const btn=el("button",null,"RÉESSAYER");btn.className="sftab";
    btn.addEventListener("click",loadControls);
    body.insertBefore(btn,body.firstChild.nextSibling);
  });
}
$("#sfclose").addEventListener("click",()=>{$("#stripfx").style.display="none";A.open=false;});
window.addEventListener("error",e=>{
  beacon("JS error: "+e.message+" @"+(e.filename||"").split("/").pop()+":"+e.lineno);
  const b=$("#sfbody");
  if(b&&A.open){const m=el("div","sfsub","⚠ Erreur JS : "+e.message+" ("+(e.filename||"").split("/").pop()+":"+e.lineno+")");
    m.style.color="var(--warn)";b.insertBefore(m,b.firstChild);}
});
function step(d){
  A.idx=(A.idx+d+18)%18;A.tab="";
  const[l,k]=labelFor(A.idx,A.isOut);
  $("#sftitle").textContent=l;$("#sfkind").textContent=k+" · EFFETS PISTE";
  render();
}
$("#sfprev").addEventListener("click",()=>step(-1));
$("#sfnext").addEventListener("click",()=>step(1));
document.querySelectorAll("#bank .strip").forEach((elm,i)=>{
  const h=()=>{const mp=st[i].map;if(mp)openDrawer(mp);};
  elm.querySelector(".id").addEventListener("click",h);
  elm.querySelector(".src").addEventListener("click",h);
});
/* lien profond tiroir : ?strip=N (0..17) ou ?strip=oN (sortie) + &fxtab=TITRE
 * — captures du manuel + debug sans passer par un clic */
{
  const q=new URLSearchParams(location.search),s=q.get("strip");
  if(s!==null&&s!==""){
    const out=/^o/i.test(s),idx=parseInt(s.replace(/\D/g,""),10)||0;
    openDrawer({idx,t:out?"out":"in"});
    const ft=q.get("fxtab");if(ft)A.tab=ft.toUpperCase();
  }
}
/* bouton RESET TAC topbar */
const tb=$("#tacbtn");
if(tb)tb.addEventListener("click",()=>{
  if(tb.classList.contains("busy"))return;
  tb.classList.add("busy");tb.textContent="RESET…";
  fetch("/api/tac/reset",{method:"POST"}).catch(()=>{}).finally(()=>{
    setTimeout(()=>{tb.classList.remove("busy");tb.textContent="RESET TAC";},900);});
});
})();

