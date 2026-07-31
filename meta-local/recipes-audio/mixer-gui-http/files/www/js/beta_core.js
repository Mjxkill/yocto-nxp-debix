/* A.L.A. console — état + construction UI (tranches, faders, knobs, banks)
 * Extrait de beta.html (V14.0 étape 7, tranche contiguë — ordre de
 * chargement = ordre d'origine, sémantique identique au script unique). */
"use strict";
const $=s=>document.querySelector(s);
const REDUCED=matchMedia("(prefers-reduced-motion: reduce)").matches;
const DPR=Math.min(devicePixelRatio||1,2);

/* ---------- scale stage to viewport ---------- */
function fit(){
  /* V10-P4c : PANEL défini ici (le module tick est chargé APRÈS le 1er fit()
   * → l'upscale ne s'appliquait jamais : écran « petit »). Le kiosk passe
   * --force-device-scale-factor=1.25 → innerWidth logique = 1024 et le
   * rendu est natif net ; ce fit ne sert alors que de filet. */
  window.PANEL=window.PANEL||(location.pathname==="/panel");
  /* V10-P4e : sur l'écran embarqué, pas de bandeau BETA ni de marges —
   * il poussait le châssis vers le bas (« coupé en bas », pris pour une
   * barre chromium). Plein cadre exact 1280x800. */
  if(window.PANEL){
    const n=document.querySelector(".note");if(n)n.style.display="none";
    document.body.style.margin="0";document.body.style.overflow="hidden";
  }
  const kw=window.PANEL?innerWidth/1280:(innerWidth-8)/1280;
  const kh=window.PANEL?innerHeight/800:(innerHeight-8)/800;
  let k=Math.min(kw,window.PANEL?kh:1);
  if(!window.PANEL)k=Math.min(k,1);
  $("#stage").style.transform=k!==1?`scale(${k})`:"";
  $("#stage").style.transformOrigin="top center";
  $("#stagewrap").style.height=(800*k)+"px";
}
addEventListener("resize",fit);fit();

/* ---------- état simulé ---------- */
const NAMES=["M1","M2","M3","M4","U1","U2","U3","U4"];
const KINDS=["MIC","MIC","MIC","MIC","USB","USB","USB","USB"];
const st=NAMES.map((n,i)=>({
  name:n,kind:KINDS[i],
  fader:0.72+((i*37)%13)/90,          // position 0..1
  gain:(i%5)*2-2,                      // dB knob
  mute:false,solo:false,
  base:[.62,.5,.4,.34,.58,.46,.3,.25][i],
  rate:[1.9,1.3,.8,2.6,1.1,.7,3.1,.5][i],
  ph:i*1.7, lvl:0, pk:0, sends:[0,0,0,0], map:{t:"in",idx:i}
}));
let anySolo=false;
const master={fader:.78,l:0,r:0,pkl:0,pkr:0,needL:0,needR:0};

/* ---------- fabrique tranches ---------- */
const bank=$("#bank");
st.forEach((s,i)=>{
  const el=document.createElement("div");el.className="strip";
  el.innerHTML=`
    <span class="id">${s.name}</span><span class="src">${s.kind}</span>
    <div class="knob" data-i="${i}" role="slider" aria-label="Gain ${s.name}" tabindex="0">
      <svg viewBox="0 0 40 40">
        <circle cx="20" cy="20" r="17" fill="#12171b" stroke="#05070a"/>
        <circle cx="20" cy="20" r="14.5" fill="url(#kg)" stroke="#31383f" stroke-width="1"/>
        <path class="karc" d="" fill="none" stroke="#e5a13c" stroke-width="2.4" stroke-linecap="round"/>
        <line class="kpin" x1="20" y1="20" x2="20" y2="8.5" stroke="#e9e5da" stroke-width="2.2" stroke-linecap="round"/>
      </svg>
    </div>
    <span class="kval">0.0 dB</span>
    <div class="fmrow">
      <div class="ftrack" data-i="${i}" role="slider" aria-label="Fader ${s.name}" tabindex="0">
        <div class="slot"></div><div class="fcap"></div>
      </div>
      <div class="mtr"><canvas></canvas></div>
    </div>
    <span class="dbro">-∞</span>
    <div class="amxbar" style="width:42px;height:3px;border-radius:1px;background:#1b2126;margin:1px auto;display:none">
      <div style="height:100%;border-radius:1px;background:#4cc470;width:100%"></div>
    </div>
    <div class="btnrow">
      <button class="sq mute" data-i="${i}">M</button>
      <button class="sq solo" data-i="${i}">S</button>
      <button class="sq amx" data-i="${i}" title="Automix">A</button>
    </div>
    <div class="send4" data-i="${i}">
      <button data-b="0">F1</button><button data-b="1">F2</button>
      <button data-b="2">F3</button><button data-b="3">F4</button>
    </div>`;
  bank.appendChild(el);
});
/* dégradé partagé pour les knobs */
const defs=document.createElementNS("http://www.w3.org/2000/svg","svg");
defs.setAttribute("width","0");defs.setAttribute("height","0");defs.style.position="absolute";
defs.innerHTML=`<defs><radialGradient id="kg" cx="35%" cy="28%" r="80%">
  <stop offset="0%" stop-color="#3a434b"/><stop offset="60%" stop-color="#242b31"/>
  <stop offset="100%" stop-color="#181d22"/></radialGradient></defs>`;
document.body.appendChild(defs);

/* master fader */
$("#mfader").innerHTML=`
  <div class="ftrack" data-m="1" role="slider" aria-label="Fader master" tabindex="0" style="height:128px">
    <div class="slot"></div><div class="fcap"></div></div>
  <span class="dbro on" id="mdb">0.0</span>`;
$("#mmtr").innerHTML=`<div class="mtr"><canvas></canvas></div><div class="mtr"><canvas></canvas></div>`;

/* ---------- knob rendu + drag ---------- */
function knobArc(el,val){ // val -12..+12
  const a0=-135,a1=a0+((val+12)/24)*270;
  const c=20,r=17.8,rad=d=>d*Math.PI/180;
  const large=(a1-a0)>180?1:0;
  const x0=c+r*Math.sin(rad(a0)),y0=c-r*Math.cos(rad(a0));
  const x1=c+r*Math.sin(rad(a1)),y1=c-r*Math.cos(rad(a1));
  el.querySelector(".karc").setAttribute("d",`M ${x0} ${y0} A ${r} ${r} 0 ${large} 1 ${x1} ${y1}`);
  el.querySelector(".kpin").setAttribute("transform",`rotate(${a1} 20 20)`);
}
document.querySelectorAll(".knob[data-i]").forEach(k=>{
  const i=+k.dataset.i;
  knobArc(k,st[i].gain);
  let y0=0,v0=0;
  k.addEventListener("pointerdown",e=>{
    y0=e.clientY;v0=st[i].gain;k.setPointerCapture(e.pointerId);
    const mv=e=>{
      st[i].gain=Math.max(-12,Math.min(12,v0+(y0-e.clientY)*0.12));
      knobArc(k,st[i].gain);
      k.parentElement.querySelector(".kval").textContent=st[i].gain.toFixed(1)+" dB";
    };
    const up=()=>{k.removeEventListener("pointermove",mv);k.removeEventListener("pointerup",up);};
    k.addEventListener("pointermove",mv);k.addEventListener("pointerup",up);
  });
});

/* ---------- faders drag ---------- */
function bindFader(tr,get,set){
  const cap=tr.querySelector(".fcap");
  const place=()=>{const h=tr.clientHeight-24;cap.style.top=(12+(1-get())*h)+"px";};
  place();new ResizeObserver(place).observe(tr);
  tr.addEventListener("pointerdown",e=>{
    tr.setPointerCapture(e.pointerId);
    const mv=e=>{
      const r=tr.getBoundingClientRect();
      const v=1-((e.clientY-r.top-12)/(r.height-24));
      set(Math.max(0,Math.min(1,v)));place();
    };
    mv(e);
    const up=()=>{tr.removeEventListener("pointermove",mv);tr.removeEventListener("pointerup",up);};
    tr.addEventListener("pointermove",mv);tr.addEventListener("pointerup",up);
  });
  return place;
}
document.querySelectorAll(".ftrack[data-i]").forEach(tr=>{
  const i=+tr.dataset.i;
  bindFader(tr,()=>st[i].fader,v=>st[i].fader=v);
});
bindFader($(".ftrack[data-m]"),()=>master.fader,v=>{
  master.fader=v;
  $("#mdb").textContent=faderDb(v).toFixed(1);
});

/* fader position → dB (échelle console : haut +12, 0 dB aux 3/4) */
function faderDb(v){return v<=0? -72 : (v*84-72)*(v>.857?1:1);}

/* ---------- mute / solo ---------- */
document.querySelectorAll(".sq.mute").forEach(b=>b.addEventListener("click",()=>{
  const i=+b.dataset.i;st[i].mute=!st[i].mute;b.classList.toggle("on",st[i].mute);
}));
document.querySelectorAll(".sq.solo").forEach(b=>b.addEventListener("click",()=>{
  const i=+b.dataset.i;st[i].solo=!st[i].solo;b.classList.toggle("on",st[i].solo);
  anySolo=st.some(s=>s.solo);
}));
/* ---------- banques console (layers X32) ---------- */
const BANKS=[
 {label:"IN DSP",  strips:Array.from({length:8},(_,i)=>({n:"M"+(i+1),sub:"MIC",t:"in",idx:i}))},
 {label:"IN USB",  strips:Array.from({length:8},(_,i)=>({n:"U"+(i+1),sub:"USB",t:"in",idx:8+i}))},
 {label:"TÉLÉPHONE", strips:[
   {n:"P1",sub:"TEL IN", t:"in", idx:16},{n:"P2",sub:"TEL IN", t:"in", idx:17},
   {n:"P1",sub:"TEL OUT",t:"out",idx:16},{n:"P2",sub:"TEL OUT",t:"out",idx:17}]},
 {label:"OUT DSP", strips:Array.from({length:8},(_,i)=>({n:"S"+(i+1),sub:"DSP OUT",t:"out",idx:i}))},
 {label:"OUT USB", strips:Array.from({length:8},(_,i)=>({n:"U"+(i+1),sub:"USB OUT",t:"out",idx:8+i}))},
];
const bankState=BANKS.map(b=>b.strips.map(()=>({fader:.72,gain:0,mute:false,sends:[0,0,0,0]})));
let curBank=0;
function placeCap(tr,v){const cap=tr.querySelector(".fcap");
  const h=tr.clientHeight-24;cap.style.top=(12+(1-v)*h)+"px";}
function saveBank(){
  BANKS[curBank].strips.forEach((sd,i)=>{const ls=bankState[curBank][i];
    ls.fader=st[i].fader;ls.gain=st[i].gain;ls.mute=st[i].mute;ls.sends=st[i].sends.slice();});
}
function applyBank(bi){
  curBank=bi;
  document.querySelectorAll("#bankbar .bk").forEach((b,j)=>b.classList.toggle("on",j===bi));
  st.forEach((s,i)=>{
    const el=bank.children[i], sd=BANKS[bi].strips[i];
    if(!sd){el.classList.add("hidden");s.map=null;s.liveTgt=0;return;}
    el.classList.remove("hidden");
    el.classList.toggle("isout",sd.t==="out");
    el.querySelector(".id").textContent=sd.n;
    el.querySelector(".src").textContent=sd.sub;
    s.map={t:sd.t,idx:sd.idx};s.name=sd.n;
    const ls=bankState[bi][i];
    s.gain=ls.gain;s.mute=ls.mute;s.sends=ls.sends.slice();s.solo=false;s.fader=ls.fader;
    /* sorties : position = gain REEL persisté (STORE.output_gain, milli-lin → dB → 0..1) */
    if(sd.t==="out"&&window.STORE&&STORE.output_gain&&STORE.output_gain.gains){
      const g=STORE.output_gain.gains[sd.idx];
      if(g!==undefined)s.fader=Math.max(0,Math.min(1,(20*Math.log10(Math.max(g,1)/1000)+72)/84));
    }
    s.lvl=0;s.pk=0;s.liveTgt=0;
    knobArc(el.querySelector(".knob"),s.gain);
    el.querySelector(".kval").textContent=s.gain.toFixed(1)+" dB";
    el.querySelector(".sq.mute").classList.toggle("on",s.mute);
    el.querySelector(".sq.solo").classList.remove("on");
    el.querySelectorAll(".send4 button").forEach((b,k)=>b.classList.toggle("on",!!s.sends[k]));
    placeCap(el.querySelector(".ftrack"),s.fader);
  });
  anySolo=false;
}
BANKS.forEach((b,bi)=>{
  const e=document.createElement("button");e.className="bk";e.textContent=b.label;
  e.addEventListener("click",()=>{saveBank();applyBank(bi);drawLinkBtns();});
  $("#bankbar").appendChild(e);
});
/* V13.3 : liens stéréo (source moteur, partagé toutes GUIs) */
window.LINKS=[0,0,0,0,0,0,0,0];
function pollLinks(){
  fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({op:"get_links"})})
  .then(r=>r.json()).then(j=>{if(j.ok&&j.links){window.LINKS=j.links;drawLinkBtns();}})
  .catch(()=>{});
}
function drawLinkBtns(){
  document.querySelectorAll(".lnkbtn").forEach(b=>b.remove());
  const sd0=BANKS[curBank].strips[0];
  if(!sd0||sd0.t!=="in"||sd0.idx>=16)return;
  const base=sd0.idx;                     /* 0 (IN DSP) ou 8 (IN USB) */
  for(let k=0;k<4;k++){
    const pair=(base>>1)+k;
    const b=document.createElement("button");
    b.className="lnkbtn";
    b.textContent="⛓";
    const on=window.LINKS[pair]===1;
    b.style.cssText="position:absolute;z-index:5;top:4px;width:28px;height:28px;"
      +"border-radius:14px;font-size:13px;cursor:pointer;"
      +`left:calc(${(k*2+1)*12.5}% - 14px);`
      +(on?"background:#2a2214;border:2px solid #e5a13c;color:#e5a13c;"
          :"background:#14181c;border:1px solid #39434b;color:#5c666e;");
    b.addEventListener("click",()=>{
      fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
        body:JSON.stringify({op:"set_link",pair:pair,on:on?0:1})})
      .then(()=>pollLinks());
    });
    bank.appendChild(b);
  }
}
pollLinks();
setInterval(pollLinks,5000);

/* sends FX1-4 par entrée (paire stéréo bus 2b/2b+1, toggle 1.0/0.0) */
document.querySelectorAll(".send4 button").forEach(b=>{
  b.addEventListener("click",()=>{
    const i=+b.parentElement.dataset.i, k=+b.dataset.b;
    const mp=st[i].map;if(!mp||mp.t!=="in")return;
    st[i].sends[k]=st[i].sends[k]?0:1;
    b.classList.toggle("on",!!st[i].sends[k]);
    if(window.LIVE&&window.MXPOST){
      const g=st[i].sends[k]?1.0:0.0;
      /* V13.3 : paire liée = send STÉRÉO (impaire→L, paire→R) */
      const linked=mp.idx<16&&window.LINKS[mp.idx>>1]===1;
      if(linked){
        const left=(mp.idx&1)===0;
        MXPOST({op:"set_send",in:mp.idx,bus:k*2,  gain:left?g:0.0});
        MXPOST({op:"set_send",in:mp.idx,bus:k*2+1,gain:left?0.0:g});
      }else{
        MXPOST({op:"set_send",in:mp.idx,bus:k*2,  gain:g});
        MXPOST({op:"set_send",in:mp.idx,bus:k*2+1,gain:g});
      }
    }
  });
});
applyBank(0);

/* V10-N4.2 — sync RETOUR (LCD/API → web) : toutes les 2 s, recharge
 * l'état réel des tranches d'entrée de la banque affichée
 * (get_strip_routing : fader/gain/mute/sends). Pas pendant un geste. */
window.__uiDrag=0;
document.addEventListener("pointerdown",()=>{window.__uiDrag++;},true);
document.addEventListener("pointerup",()=>{window.__uiDrag=Math.max(0,window.__uiDrag-1);},true);
document.addEventListener("pointercancel",()=>{window.__uiDrag=Math.max(0,window.__uiDrag-1);},true);
setInterval(()=>{
  if(!window.LIVE||window.__uiDrag>0||document.hidden)return;
  BANKS[curBank].strips.forEach((sd,i)=>{
    if(sd.t!=="in")return;
    fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({op:"get_strip_routing",src:sd.idx})})
    .then(r=>r.json()).then(j=>{
      if(!j||!j.ok||window.__uiDrag>0)return;
      const s=st[i],el=bank.children[i];
      /* V10-N7 : fader IN synchronisé depuis le gain de tranche (E7.2) */
      const g=j.gain||0;
      const db=g>0.001?Math.max(-60,20*Math.log10(g)):-60;
      s.fader=(db+60)/66;
      placeCap(el.querySelector(".ftrack"),s.fader);
      if(j.gain>0){
        s.gain=Math.max(-12,Math.min(12,20*Math.log10(j.gain)));
        knobArc(el.querySelector(".knob"),s.gain);
        el.querySelector(".kval").textContent=s.gain.toFixed(1)+" dB";
      }
      s.mute=j.mute===1;
      el.querySelector(".sq.mute").classList.toggle("on",s.mute);
      if(j.sends){
        const so=[];
        for(let b=0;b<4;b++)so.push((j.sends[b*2]||0)>0.001?1:0);
        s.sends=so;
        el.querySelectorAll(".send4 button").forEach((bt,k)=>bt.classList.toggle("on",!!so[k]));
      }
    }).catch(()=>{});
  });
},2000);

document.querySelectorAll(".tab").forEach(t=>t.addEventListener("click",()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.remove("on"));t.classList.add("on");
  const name=t.textContent.trim();
  const map={"MIXER":"MIXER","EFFETS":"EFFETS","MASTERING":"MASTERING",
             "PADS":"PADS","LOOPER":"LOOPER","EXPANDEUR":"EXPANDEUR",
             "AUTO MIX":"BANDMIX","SCÈNE":"SCENE",
             "ROUTING":"ROUTING","SYSTÈME":"SYSTEME"};
  const target=map[name]||"MIXER";
  window.CURPAGE=target;   /* V12 web : gate les pollers PADS/LOOPER/EXPANDEUR */
  document.querySelectorAll("#pages>.page").forEach(p=>
    p.classList.toggle("hid",p.dataset.page!==target));
}));
/* V13 : lien profond ?page=MIXER|EFFETS|MASTERING|PADS|LOOPER|EXPANDEUR|
 * BANDMIX|SCENE|ROUTING|SYSTEME — favoris + captures du manuel */
{
  const want=(new URLSearchParams(location.search).get("page")||"").toUpperCase();
  if(want){
    window.CURPAGE=want;
    document.querySelectorAll("#pages>.page").forEach(p=>
      p.classList.toggle("hid",p.dataset.page!==want));
    const rmap={"MIXER":"MIXER","EFFETS":"EFFETS","MASTERING":"MASTERING",
      "PADS":"PADS","LOOPER":"LOOPER","EXPANDEUR":"EXPANDEUR",
      "AUTO MIX":"BANDMIX","SCÈNE":"SCENE","ROUTING":"ROUTING","SYSTÈME":"SYSTEME"};
    document.querySelectorAll(".tab").forEach(t=>
      t.classList.toggle("on",rmap[t.textContent.trim()]===want));
  }
}
/* diag géométrie : ?geo=1 → panneau des largeurs réelles (debug layout) */
if(new URLSearchParams(location.search).get("geo")){
  setTimeout(()=>{
    const lines=[];
    let e=document.getElementById("pages");
    while(e&&e!==document.body){
      lines.push(`↑ ${e.tagName}#${e.id||"-"} ow=${e.offsetWidth} cs.w=${getComputedStyle(e).width} disp=${getComputedStyle(e).display}`);
      e=e.parentElement;
    }
    document.querySelectorAll("#stage *").forEach(el=>{
      if(el.offsetWidth>1300&&lines.length<26)
        lines.push(`⚠ ${el.tagName}#${el.id||"-"}.${(el.className||"").toString().split(" ")[0]||"-"} ow=${el.offsetWidth}`);
    });
    const d=document.createElement("div");
    d.style.cssText="position:fixed;top:4px;left:4px;z-index:9999;background:#000;color:#0f0;font:11px monospace;padding:6px;border:1px solid #0f0;max-height:92vh;overflow:auto;";
    d.innerHTML=lines.join("<br>");
    document.body.appendChild(d);
  },2500);
}
/* bus FX + items + cats : sélection visuelle */
document.querySelectorAll(".fxbus").forEach(b=>b.addEventListener("click",()=>{
  document.querySelectorAll(".fxbus").forEach(x=>x.classList.remove("on"));b.classList.add("on");}));
document.querySelectorAll(".fxit").forEach(b=>b.addEventListener("click",()=>{
  document.querySelectorAll(".fxit").forEach(x=>x.classList.remove("on"));b.classList.add("on");
  document.querySelector(".rackhead .pname").textContent=b.childNodes[0].textContent;}));
document.querySelectorAll(".fxcat").forEach(b=>b.addEventListener("click",()=>{
  document.querySelectorAll(".fxcat").forEach(x=>x.classList.remove("on"));b.classList.add("on");}));
document.querySelectorAll(".pwrsw").forEach(s=>s.addEventListener("click",()=>s.classList.toggle("on")));

/* ---------- knobs FX génériques (data-min/max/val/unit[/log]) ---------- */
document.querySelectorAll(".knob.fxk").forEach(k=>{
  const min=+k.dataset.min,max=+k.dataset.max,log=k.dataset.log==="1",unit=k.dataset.unit||"";
  let val=+k.dataset.val;
  k.innerHTML=`<svg viewBox="0 0 40 40">
    <circle cx="20" cy="20" r="17" fill="#12171b" stroke="#05070a"/>
    <circle cx="20" cy="20" r="14.5" fill="url(#kg)" stroke="#31383f" stroke-width="1"/>
    <path class="karc" d="" fill="none" stroke="#e5a13c" stroke-width="2.4" stroke-linecap="round"/>
    <line class="kpin" x1="20" y1="20" x2="20" y2="8.5" stroke="#e9e5da" stroke-width="2.2" stroke-linecap="round"/>
  </svg>`;
  const norm=v=>log?Math.log(v/min)/Math.log(max/min):(v-min)/(max-min);
  const dnorm=n=>log?min*Math.pow(max/min,n):min+n*(max-min);
  const kv=k.parentElement.querySelector(".kv");
  const fmt=v=>{
    const d=(max-min>50||log)?0:1;
    const s=(v>0&&min<0?"+":"")+v.toFixed(d);
    return s+unit;
  };
  function draw(){
    const n=Math.max(0,Math.min(1,norm(val)));
    const a0=-135,a1=a0+n*270,c=20,r=17.8,rad=d=>d*Math.PI/180;
    const large=(a1-a0)>180?1:0;
    const x0=c+r*Math.sin(rad(a0)),y0=c-r*Math.cos(rad(a0));
    const x1=c+r*Math.sin(rad(a1)),y1=c-r*Math.cos(rad(a1));
    k.querySelector(".karc").setAttribute("d",`M ${x0} ${y0} A ${r} ${r} 0 ${large} 1 ${x1} ${y1}`);
    k.querySelector(".kpin").setAttribute("transform",`rotate(${a1} 20 20)`);
    if(kv)kv.textContent=fmt(val);
  }
  draw();
  k.addEventListener("pointerdown",e=>{
    const y0=e.clientY,n0=norm(val);
    k.setPointerCapture(e.pointerId);
    const mv=e=>{val=dnorm(Math.max(0,Math.min(1,n0+(y0-e.clientY)*0.006)));draw();};
    const up=()=>{k.removeEventListener("pointermove",mv);k.removeEventListener("pointerup",up);};
    k.addEventListener("pointermove",mv);k.addEventListener("pointerup",up);
  });
});

/* ---------- canvases ---------- */
function setupCanvas(c){
  const r=c.parentElement.getBoundingClientRect();
  c.width=Math.max(2,r.width*DPR);c.height=Math.max(2,r.height*DPR);
  return c.getContext("2d");
}
const stripCv=[...document.querySelectorAll("#bank .mtr canvas")];
const mmCv=[...document.querySelectorAll("#mmtr canvas")];
let ctxs=[],mctxs=[],vuctx,spctx;
function initCv(){
  ctxs=stripCv.map(setupCanvas);
  mctxs=mmCv.map(setupCanvas);
  const vu=$("#vu");const vr=vu.getBoundingClientRect();
  vu.width=vr.width*DPR;vu.height=128*DPR;vuctx=vu.getContext("2d");
  const sp=$("#spec");const sr=sp.parentElement.getBoundingClientRect();
  sp.width=sr.width*DPR;sp.height=sr.height*DPR;spctx=sp.getContext("2d");
}
initCv();addEventListener("resize",()=>setTimeout(initCv,60));

/* ---------- dessin d'un bargraph ---------- */
function drawMeter(ctx,lvl,pk){
  const w=ctx.canvas.width,h=ctx.canvas.height;
  ctx.clearRect(0,0,w,h);
  const segs=Math.floor(h/(5*DPR));
  for(let s=0;s<segs;s++){
    const f=s/segs;                       // 0 bas → 1 haut
    const on=f<lvl;
    const y=h-(s+1)*(h/segs);
    let col;
    if(f>0.86)col=on?"#e05545":"#3a1512";
    else if(f>0.68)col=on?"#e5a13c":"#33270f";
    else col=on?"#4cc470":"#10281a";
    ctx.fillStyle=col;
    ctx.fillRect(1,y+1,w-2,h/segs-2*DPR*0.7);
  }
  if(pk>0.02){
    ctx.fillStyle="#f4efe4";
    ctx.fillRect(0,h*(1-pk)-1,w,2*DPR*0.7);
  }
}

/* ---------- goniomètre (Lissajous, rémanence phosphore) ---------- */
let scopeMode=0;   /* 0 = VU aiguilles, 1 = goniomètre */
$("#vuwrap").addEventListener("click",()=>{
  scopeMode^=1;
  $("#vl0").textContent=scopeMode?"L":"LEFT";
  $("#vl1").textContent=scopeMode?"GONIOMÈTRE":"VU";
  $("#vl2").textContent=scopeMode?"R":"RIGHT";
});
let gonioPts=[];
function drawGonio(t){
  const w=vuctx.canvas.width,h=vuctx.canvas.height;
  /* rémanence : voile sombre au lieu d'un clear */
  vuctx.fillStyle="rgba(13,10,8,0.22)";
  vuctx.fillRect(0,0,w,h);
  const cx=w/2,cy=h/2,R=h*0.46;
  /* réticule */
  vuctx.strokeStyle="rgba(229,161,60,.14)";vuctx.lineWidth=1;
  vuctx.beginPath();vuctx.moveTo(cx-R,cy+R);vuctx.lineTo(cx+R,cy-R);vuctx.stroke();
  vuctx.beginPath();vuctx.moveTo(cx-R,cy-R);vuctx.lineTo(cx+R,cy+R);vuctx.stroke();
  /* nuage stéréo : phase corrélée + wobble */
  vuctx.fillStyle="rgba(120,225,150,.5)";
  vuctx.shadowColor="rgba(120,225,150,.9)";vuctx.shadowBlur=4;
  if(window.LIVE_SCOPE&&window.LIVE_SCOPE.length>=4){
    const sc=window.LIVE_SCOPE;      /* [l,r,l,r,...] S16 réels */
    for(let i=0;i<sc.length;i+=2){
      const l=sc[i]/32768, r=sc[i+1]/32768;
      const x=cx+(l-r)*R*1.1, y=cy-(l+r)*R*0.62;
      vuctx.fillRect(x,y,1.6*DPR,1.6*DPR);
    }
  }else{
    const amp=(master.l+master.r)*0.5;
    const n=REDUCED?40:90;
    for(let i=0;i<n;i++){
      const ph=t*7+i*0.31;
      const m=amp*(0.35+0.65*Math.abs(Math.sin(ph*1.7+i)));
      const corr=0.72+0.2*Math.sin(t*0.4);
      const l=m*Math.sin(ph), r=m*Math.sin(ph*corr+0.4*Math.sin(t+i));
      const x=cx+(l-r)*R*1.1, y=cy-(l+r)*R*0.62;
      vuctx.fillRect(x,y,1.6*DPR,1.6*DPR);
    }
  }
  vuctx.shadowBlur=0;
}

/* ---------- VU à aiguilles ---------- */
function drawVU(t){
  const w=vuctx.canvas.width,h=vuctx.canvas.height;
  vuctx.clearRect(0,0,w,h);
  const half=w/2;
  [[half*0.5,master.needL],[half*1.5,master.needR]].forEach(([cx,val],idx)=>{
    const cy=h*1.16,R=h*0.92;
    // cadran
    vuctx.strokeStyle="rgba(229,161,60,.28)";
    vuctx.lineWidth=1*DPR;
    vuctx.beginPath();vuctx.arc(cx,cy,R,-2.15,-0.99);vuctx.stroke();
    // graduations
    for(let g=0;g<=10;g++){
      const a=-2.15+(g/10)*1.16;
      const r0=R-4*DPR,r1=(g%5===0)?R-11*DPR:R-7*DPR;
      vuctx.strokeStyle=g>=8?"rgba(224,85,69,.8)":"rgba(233,229,218,.5)";
      vuctx.lineWidth=(g%5===0?1.6:1)*DPR;
      vuctx.beginPath();
      vuctx.moveTo(cx+Math.cos(a)*r0,cy+Math.sin(a)*r0);
      vuctx.lineTo(cx+Math.cos(a)*r1,cy+Math.sin(a)*r1);
      vuctx.stroke();
    }
    // aiguille
    const a=-2.15+Math.min(1,val)*1.16;
    vuctx.strokeStyle="#e9e5da";
    vuctx.lineWidth=1.8*DPR;
    vuctx.shadowColor="rgba(229,161,60,.55)";vuctx.shadowBlur=6*DPR;
    vuctx.beginPath();
    vuctx.moveTo(cx,cy-6*DPR);
    vuctx.lineTo(cx+Math.cos(a)*(R-6*DPR),cy+Math.sin(a)*(R-6*DPR));
    vuctx.stroke();vuctx.shadowBlur=0;
    // pivot
    vuctx.fillStyle="#0d0a07";
    vuctx.beginPath();vuctx.arc(cx,cy-4*DPR,5*DPR,0,7);vuctx.fill();
    vuctx.strokeStyle="#8a6526";vuctx.stroke();
  });
}

/* ---------- spectre + enveloppe ML + waterfall ---------- */
const NB=48;
const specState=new Array(NB).fill(0);
const specTgt=new Array(NB).fill(0);   /* cibles LIVE, lissées à 60fps */
let wfCanvas=null,wfctx=null;
function drawSpec(t){
  const w=spctx.canvas.width,h=spctx.canvas.height;
  const hs=Math.floor(h*0.66);            /* zone spectre */
  const hw=h-hs-2;                        /* zone waterfall */
  if(!wfCanvas||wfCanvas.width!==w||wfCanvas.height!==hw){
    wfCanvas=document.createElement("canvas");wfCanvas.width=w;wfCanvas.height=hw;
    wfctx=wfCanvas.getContext("2d");
  }
  spctx.clearRect(0,0,w,h);
  // grille faible (zone spectre)
  spctx.strokeStyle="rgba(255,255,255,.05)";spctx.lineWidth=1;
  for(let gy=1;gy<4;gy++){spctx.beginPath();spctx.moveTo(0,hs*gy/4);spctx.lineTo(w,hs*gy/4);spctx.stroke();}
  const bw=w/NB;
  const g=spctx.createLinearGradient(0,hs,0,0);
  g.addColorStop(0,"rgba(76,196,112,.85)");
  g.addColorStop(.62,"rgba(76,196,112,.85)");
  g.addColorStop(.8,"rgba(229,161,60,.9)");
  g.addColorStop(1,"rgba(224,85,69,.95)");
  spctx.fillStyle=g;
  const mAmp=(master.l+master.r)/2;
  for(let b=0;b<NB;b++){
    if(window.LIVE){
      specState[b]+=(specTgt[b]-specState[b])*(specTgt[b]>specState[b]?kdt(window.__dt||16.7,25):kdt(window.__dt||16.7,90));
    }else{
      const f=b/NB;
      const shape=Math.pow(1-f,0.72);
      const wob=0.14*Math.sin(t*(1.1+f*5.3)+b*1.31)+0.09*Math.sin(t*0.37+b*0.7);
      const tgt=Math.max(0,(shape+wob)*mAmp*1.18);
      specState[b]+= (tgt-specState[b])*(tgt>specState[b]?kdt(window.__dt||16.7,25):kdt(window.__dt||16.7,120));
    }
    const bh=specState[b]*hs*0.92;
    spctx.fillRect(b*bw+1,hs-bh,bw-2,bh);
  }
  /* --- waterfall : décale l'historique d'1px vers le bas, nouvelle ligne en haut --- */
  if(!window.PANEL&&wfCanvas.width>0&&wfCanvas.height>1){
  wfctx.drawImage(wfCanvas,0,0,w,hw-1,0,1,w,hw-1);
  for(let b=0;b<NB;b++){
    const v=Math.min(1,specState[b]*1.25);
    let col;
    if(v<0.04)col="#0c1013";
    else if(v<0.4)col=`rgba(46,110,72,${0.25+v})`;
    else if(v<0.75)col=`rgba(229,161,60,${0.35+v*0.6})`;
    else col=`rgba(224,85,69,${0.5+v*0.5})`;
    wfctx.fillStyle=col;
    wfctx.fillRect(b*bw,0,bw,1.6);
  }
  spctx.drawImage(wfCanvas,0,hs+2);
  }
  spctx.fillStyle="rgba(233,229,218,.35)";
  // enveloppe ML (courbe ambre lissée par-dessus)
  spctx.strokeStyle="#e5a13c";spctx.lineWidth=1.6*DPR;
  spctx.shadowColor="rgba(229,161,60,.6)";spctx.shadowBlur=3;
  spctx.beginPath();
  const mlenv=window.LIVE_ENV;      /* 64 gains dB réels (spectral_env) */
  for(let b=0;b<=NB;b++){
    const f=b/NB;
    let y;
    if(mlenv&&mlenv.length===64){
      const g=mlenv[Math.min(63,Math.round(f*63))];   /* -12..+12 dB */
      y=hs*(0.5-(g/12)*0.42);
    }else{
      const env=0.5-0.16*Math.sin(f*5+t*0.21)-0.10*Math.cos(f*11-t*0.13)+0.10*f;
      y=hs*(0.62-env*0.34);
    }
    if(b===0)spctx.moveTo(0,y);else spctx.lineTo(f*w,y);
  }
  spctx.stroke();spctx.shadowBlur=0;
}

/* ---------- moteur de simulation ---------- */
let t0=performance.now();
let tickErr=0,frameNo=0;
window.LIVE=false;          /* bascule par le bridge SSE si la board répond */
/* V10-P4a : mode PANEL (écran embarqué) — l'i.MX8MP rend la page en local :
 * 30 fps + waterfall off pour laisser les cores 0-1 au daemon ML/IRQ.
 * Mesure kiosk 60 fps plein rendu : cpu0/1 99 % → 13 xruns/min. */
window.PANEL=window.PANEL||(location.pathname==="/panel");
let lastNow=0;
/* V10-P2e : ballistiques en TEMPS REEL (dt), plus par frame - un rAF lent
 * (fenetre chargee, 15-20 fps) gardait les memes coefficients par frame et
 * multipliait les constantes de temps x3-6 (VU en retard de ~0.5-1 s).
 * k = 1-exp(-dt/tau) : identique a 60 fps, correct a bas fps. */
const kdt=(dt,tau)=>1-Math.exp(-dt/tau);
window.__fps={n:0,t:0,v:0};
function tick(now){
 try{
  const t=(now-t0)/1000;
  const dt=Math.min(100,lastNow?now-lastNow:16.7);lastNow=now;
  window.__dt=dt;
  window.__fps.n++;
  if(now-window.__fps.t>=1000){window.__fps.v=Math.round(window.__fps.n*1000/(now-window.__fps.t));
    window.__fps.n=0;window.__fps.t=now;
    const fe=document.getElementById("fpsro");
    if(fe){fe.textContent=window.__fps.v;fe.style.color=window.__fps.v<30?"var(--warn)":"";}}
  const kAtk=kdt(dt,28),kRel=kdt(dt,110),kPk=Math.exp(-dt/1100);
  if(window.LIVE){
    st.forEach(s=>{
      const tgt=s.liveTgt||0;
      s.lvl+=(tgt-s.lvl)*(tgt>s.lvl?kAtk:kRel);
      s.pk=Math.max(s.pk*kPk,s.lvl);
    });
    const tl=master.liveTgtL||0, tr=master.liveTgtR||0;
    master.l+=(tl-master.l)*(tl>master.l?kAtk:kRel);
    master.r+=(tr-master.r)*(tr>master.r?kAtk:kRel);
  }
  if(!window.LIVE){
  let suml=0,sumr=0;
  st.forEach((s,i)=>{
    const active=!s.mute&&(!anySolo||s.solo);
    const sig=active?Math.max(0,s.base+0.22*Math.sin(t*s.rate+s.ph)+0.16*Math.sin(t*4.7*s.rate+s.ph*2)+(Math.random()-0.5)*0.1):0;
    const post=sig*s.fader*Math.pow(10,s.gain/40);
    s.lvl+= (post-s.lvl)*(post>s.lvl?kAtk:kRel);
    s.pk=Math.max(s.pk*kPk,s.lvl);
    suml+=post*(i%2?0.8:1.05);sumr+=post*(i%2?1.05:0.8);
  });
  const mf=master.fader;
  const ml=Math.min(1,suml*0.34*mf),mr=Math.min(1,sumr*0.34*mf);
  master.l+=(ml-master.l)*(ml>master.l?kAtk:kRel);
  master.r+=(mr-master.r)*(mr>master.r?kAtk:kRel);
  }
  master.pkl=Math.max(master.pkl*kPk,master.l);
  master.pkr=Math.max(master.pkr*kPk,master.r);
  // ballistique aiguilles VU (tau 150 ms, temps reel)
  const kNd=kdt(dt,150);
  master.needL+=(master.l-master.needL)*kNd;
  master.needR+=(master.r-master.needR)*kNd;

  ctxs.forEach((c,i)=>drawMeter(c,st[i].lvl,st[i].pk));
  drawMeter(mctxs[0],master.l,master.pkl);
  drawMeter(mctxs[1],master.r,master.pkr);
  if(scopeMode)drawGonio(t);else drawVU(t);
  drawSpec(t);

  // readouts par tranche (éléments cachés, maj 1 frame sur 4)
  if(!window.__dbroEls)window.__dbroEls=[...document.querySelectorAll("#bank .dbro")];
  if((frameNo++&3)===0)window.__dbroEls.forEach((el,i)=>{
    const db=st[i].lvl>0.003?(20*Math.log10(st[i].lvl)).toFixed(1):"-∞";
    el.textContent=db==="-∞"?"-∞":db;
    el.classList.toggle("on",st[i].lvl>0.003);
  });

 }catch(e){
   if(!tickErr){tickErr=1;console.error("tick:",e);
     const d=document.createElement("div");
     d.style.cssText="position:fixed;bottom:4px;left:8px;color:#e05545;font:11px monospace;z-index:99";
     d.textContent="⚠ "+e.message;document.body.appendChild(d);}
 }
 requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
if(false){ // une frame statique de démonstration
  st.forEach((s,i)=>{s.lvl=s.base*s.fader;s.pk=s.lvl*1.1;});
  master.l=.55;master.r=.5;master.needL=.55;master.needR=.5;
  setTimeout(()=>{ctxs.forEach((c,i)=>drawMeter(c,st[i].lvl,st[i].pk));
    drawMeter(mctxs[0],master.l,master.pkl);drawMeter(mctxs[1],master.r,master.pkr);
    drawVU(1);drawSpec(1);},80);
}

/* ---------- horloge + stats lentes ---------- */
let secs=0;
setInterval(()=>{
  secs++;
  const h=String(Math.floor(secs/3600)).padStart(2,"0"),
        m=String(Math.floor(secs/60)%60).padStart(2,"0"),
        s=String(secs%60).padStart(2,"0");
  $("#clock").innerHTML=`${h}:${m}:${s}<span class="fr">.${String(Math.floor(Math.random()*99)).padStart(2,"0")}</span>`;
},1000);
setInterval(()=>{
  if(window.LIVE){
    /* LIVE : sysload vient du state stream ; on met à jour LUFS→peak réel */
    const pl=20*Math.log10(Math.max(master.pkl,0.001)), pr=20*Math.log10(Math.max(master.pkr,0.001));
    $("#lufs").innerHTML=Math.max(pl,pr).toFixed(1)+" <em>dB pk</em>";
    $("#mlufs").textContent=Math.max(pl,pr).toFixed(1);
    $("#pkl").textContent=pl.toFixed(1);
    $("#pkr").textContent=pr.toFixed(1);
    return;
  }
  const cpu=18+Math.round(Math.random()*9), dsp=70+Math.round(Math.random()*4), npu=12+Math.round(Math.random()*6);
  {const bars=document.querySelectorAll("#cpu4 i b");
   [cpu,cpu-4,18+Math.round(Math.random()*8),6+Math.round(Math.random()*6)]
     .forEach((v,i)=>{if(bars[i])bars[i].style.height=Math.max(3,v)+"%";});}
  $("#dspv").textContent=dsp+"%";$("#dspb").style.height=dsp+"%";
  $("#npuv").textContent=npu+"%";$("#npub").style.height=npu+"%";
  const lu=(-14.6+Math.random()*0.9).toFixed(1);
  $("#lufs").innerHTML=`${lu} <em>LUFS</em>`;$("#mlufs").textContent=lu;
  $("#grb").style.width=(6+Math.random()*16)+"%";
  const fxgr=$("#fxgr");if(fxgr)fxgr.style.width=(14+Math.random()*20)+"%";
  $("#pkl").textContent=(20*Math.log10(Math.max(master.pkl,0.001))).toFixed(1);
  $("#pkr").textContent=(20*Math.log10(Math.max(master.pkr,0.001))).toFixed(1);
},900);


/* V13.2 : appui long (anneau de progression, parité LCD HoldButton).
 * ÉTAIT égaré dans le bloc <style> de beta.html (bug préexistant :
 * jamais défini côté JS → ReferenceError sur scènes/pads web).
 * Découvert au test navigateur du découpage V14.0 étape 7. */
/* V13.2 : appui long avec anneau de progression (parité LCD HoldButton).
 * Presse 900 ms → cb() ; relâcher/le pointeur sort = annulé.
 * opts.freeze/unfreeze : gel du re-render pendant l'appui (pages pollées). */
function holdify(btn,cb,opts={}){
  const ms=opts.ms||900;
  let t0=0,raf=0,ring=null;
  const stop=()=>{cancelAnimationFrame(raf);
    if(ring){ring.remove();ring=null;}
    if(opts.unfreeze)opts.unfreeze();};
  btn.addEventListener("pointerdown",e=>{
    if(btn.disabled)return;
    e.preventDefault();
    if(opts.freeze)opts.freeze();
    t0=performance.now();
    ring=document.createElement("div");
    ring.style.cssText="position:absolute;inset:-3px;border-radius:inherit;pointer-events:none;";
    const mask="radial-gradient(closest-side, transparent 60%, black 62%)";
    ring.style.webkitMask=mask;ring.style.mask=mask;
    if(getComputedStyle(btn).position==="static")btn.style.position="relative";
    btn.appendChild(ring);
    const step=()=>{
      const pct=Math.min(1,(performance.now()-t0)/ms);
      ring.style.background=
        `conic-gradient(${opts.color||"#f2796a"} ${pct*360}deg, transparent 0)`;
      if(pct>=1){stop();cb();return;}
      raf=requestAnimationFrame(step);
    };
    raf=requestAnimationFrame(step);
  });
  ["pointerup","pointerleave","pointercancel"].forEach(ev=>
    btn.addEventListener(ev,stop));
  btn.addEventListener("click",e=>{e.preventDefault();e.stopImmediatePropagation();},true);
}
