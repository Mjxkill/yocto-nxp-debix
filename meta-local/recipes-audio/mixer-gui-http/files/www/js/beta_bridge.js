/* A.L.A. console — LIVE BRIDGE V10-P1 : SSE état + envoi ops
 * Extrait de beta.html (V14.0 étape 7, tranche contiguë — ordre de
 * chargement = ordre d'origine, sémantique identique au script unique). */
/* ================== V10-P1 : LIVE BRIDGE ==================
 * Connecte la console au vrai mixer-pro :
 *  - /api/state/sse : état versionné (seq/resync), enveloppe ML, sysload
 *  - /api/stream    : meters 30 Hz + analyzer (spectre réel, scope gonio)
 *  - POST /api/cmd  : faders → set_master, knobs → set_input_gain,
 *                     mute → set_mute, master → set_output_gain
 * Mapping tranches : M1-M4 = in[0..3] (mics DSP), U1-U4 = in[8..11] (USB).
 * Sans board (artifact/démo) : la simulation continue, rien ne casse. */
(function(){
"use strict";
let lastSeq=null, es=null, esRetry=800;

const post=(o)=>fetch("/api/cmd",{method:"POST",
  headers:{"Content-Type":"application/json"},body:JSON.stringify(o)}).catch(()=>{});
window.MXPOST=post;

/* throttle par clé (45 ms + trailing) — même recette que la GUI validée */
const thMap={}, trMap={};
function postT(key,o){
  const now=performance.now();
  clearTimeout(trMap[key]);
  if(now-(thMap[key]||0)>45){thMap[key]=now;post(o);}
  else trMap[key]=setTimeout(()=>{thMap[key]=performance.now();post(o);},45);
}

function applyState(d){
  const f=d.full||d.patch||{};
  window.STORE=window.STORE||{};
  Object.keys(f).forEach(k=>{window.STORE[k]=f[k];});
  if(f.insert&&f.insert.chain&&f.insert.chain[0]&&f.insert.chain[0].l)
    window.LIVE_ENV=f.insert.chain[0].l;
  if(f.sysload&&f.sysload.cpu){
    const s=f.sysload;
    const set=(id,v,bar)=>{const e=document.getElementById(id);if(e)e.textContent=v+"%";
      const b=document.getElementById(bar);if(b)b.style.height=Math.min(100,Math.max(3,v))+"%";};
    {const bars=document.querySelectorAll("#cpu4 i b");
     s.cpu.forEach((v,i)=>{if(bars[i])bars[i].style.height=Math.min(100,Math.max(3,v))+"%";});}
    set("dspv",s.dsp<0?0:s.dsp,"dspb");
    set("npuv",s.npu<0?0:s.npu,"npub");
  }
  if(f.stat){
    const x=document.querySelector(".mastnotes .mn b[style]");
    if(x)x.textContent=f.stat.xrun;
    const lat=document.querySelectorAll("#topbar .chip .v")[1];
    if(lat&&f.stat.latency_us_one_way)
      lat.innerHTML=(f.stat.latency_us_one_way/1000).toFixed(1)+" <em>ms</em>";
  }
  if(f.output_gain&&f.output_gain.gains){
    const g=f.output_gain.gains[0]||1000;
    master.fader=Math.max(0,Math.min(1,(20*Math.log10(Math.max(g,1)/1000)+72)/84));
  }
}

function connectState(){
  es=new EventSource("/api/state/sse");
  es.onmessage=(e)=>{
    try{
      const d=JSON.parse(e.data);
      if(lastSeq!==null&&d.seq!==lastSeq+1&&!d.full){
        /* gap → resync */
        fetch("/api/state/full").then(r=>r.json()).then(fd=>{
          lastSeq=fd.seq;applyState(fd);}).catch(()=>{});
      }
      lastSeq=d.seq;
      if(!window.LIVE){
        window.LIVE=true;
        window.dispatchEvent(new Event("mixer-live"));
        document.querySelector(".sysdot").childNodes[1].textContent="LIVE";
        /* tap analyzer 3 = OUT 0/1 pour spectre+gonio réels */
        post({op:"set_tap",tap:3,kind:3,a:0,b:1});
      }
      applyState(d);
    }catch(_){}
  };
  es.onerror=()=>{
    es.close();window.LIVE=false;window.LIVE_ENV=null;window.LIVE_SCOPE=null;
    setTimeout(connectState,esRetry);esRetry=Math.min(esRetry*2,8000);
  };
  es.onopen=()=>{esRetry=800;};
}

/* meters 30 Hz + analyzer */
function connectMeters(){
  const ms=new EventSource("/api/stream");
  const dbn=(p)=>{ /* peak s32 → 0..1 normalisé pour les bargraphs (-60..0 dB) */
    if(!p||p<=0)return 0;
    const db=20*Math.log10(p/2147483648);
    return Math.max(0,Math.min(1,(db+60)/60));
  };
  ms.onmessage=(e)=>{
    if(!window.LIVE)return;
    try{
      const m=JSON.parse(e.data);
      st.forEach((s)=>{ const mp=s.map;if(!mp)return;
        const arr=mp.t==="in"?m.in:m.out;
        if(Array.isArray(arr))s.liveTgt=dbn(arr[mp.idx]); });
      if(Array.isArray(m.out)){
        master.liveTgtL=dbn(m.out[0]);master.liveTgtR=dbn(m.out[1]);
      }
      if(Array.isArray(m.analyzer)&&m.analyzer[3]){
        const t3=m.analyzer[3];
        if(Array.isArray(t3.s)){
          /* 128 bins log dB → 48 cibles 0..1 (lissées à 60fps dans drawSpec) */
          for(let b=0;b<NB;b++){
            const v=t3.s[Math.min(127,Math.round(b*127/(NB-1)))];
            specTgt[b]=Math.max(0,Math.min(1,(v+90)/90));
          }
        }
        if(Array.isArray(t3.x))window.LIVE_SCOPE=t3.x;
      }
    }catch(_){}
  };
  ms.onerror=()=>{ms.close();setTimeout(connectMeters,2000);};
}

/* ---- contrôles → vrai mixer ---- */
/* fader de tranche IN = gain de la tranche (set_input_gain, -60..+6 dB) —
 * sémantique E7.2 restaurée. La MATRICE n'est jamais touchée par le mixer
 * (régression V10-P2c : fader→set_master out0+1 polluait le routage →
 * phasing). Fader OUT = trim de sortie, inchangé. */
document.querySelectorAll(".ftrack[data-i]").forEach(tr=>{
  const i=+tr.dataset.i;
  tr.addEventListener("pointermove",()=>{
    if(!window.LIVE)return;
    const mp=st[i].map;if(!mp)return;
    if(mp.t==="in"){
      const db=st[i].fader*66-60;
      postT("f"+i,{op:"set_input_gain",src:mp.idx,gain:Math.pow(10,db/20)});
      /* V13.3 : paire liée → le fader jumeau suit à la frame */
      if(mp.idx<16&&window.LINKS[mp.idx>>1]===1){
        const pidx=mp.idx^1;
        st.forEach((sp,j)=>{
          if(j!==i&&sp.map&&sp.map.t==="in"&&sp.map.idx===pidx){
            sp.fader=st[i].fader;
            placeCap(bank.children[j].querySelector(".ftrack"),sp.fader);
          }
        });
      }
    }else{
      const db=st[i].fader*84-72;
      postT("f"+i,{op:"set_output_gain",out:mp.idx,db:db});
    }
  });
});
/* knob gain d'entrée */
document.querySelectorAll(".knob[data-i]").forEach(k=>{
  const i=+k.dataset.i;
  k.addEventListener("pointermove",()=>{
    if(!window.LIVE)return;
    const mp=st[i].map;if(!mp||mp.t!=="in")return;
    postT("k"+i,{op:"set_input_gain",src:mp.idx,
                 gain:Math.pow(10,st[i].gain/20)});
  });
});
/* mute réel ; solo = GUI seule (P2) */
document.querySelectorAll(".sq.mute").forEach(b=>{
  const i=+b.dataset.i;
  b.addEventListener("click",()=>{
    const mp=st[i].map;
    if(window.LIVE&&mp&&mp.t==="in")post({op:"set_mute",src:mp.idx,mute:st[i].mute?1:0});
  });
});
/* V12-AMX : adhésion au groupe automix + sync état 1 s + bouton global */
document.querySelectorAll(".sq.amx").forEach(b=>{
  const i=+b.dataset.i;
  b.addEventListener("click",()=>{
    const mp=st[i].map;
    if(!window.LIVE||!mp||mp.t!=="in")return;
    const on=!b.classList.contains("on");
    b.classList.toggle("on",on);
    /* V12-AMX-UI : ne PAS envoyer weight_db (updates partiels moteur) —
     * l'ancien weight_db:0 écrasait le poids à chaque toggle */
    post({op:"set_automix",src:mp.idx,on:on?1:0});
  });
});
window.__amxOn=false;
const amxG=document.getElementById("amx-global");
if(amxG)amxG.addEventListener("click",()=>{
  if(!window.LIVE)return;
  post({op:"set_automix_cfg",on:window.__amxOn?0:1});
});
/* V12-AMX-UI : panneau réglages Dugan (response / floor / poids) */
const amxCfgBtn=document.getElementById("amx-cfg");
if(amxCfgBtn)amxCfgBtn.addEventListener("click",()=>{
  let p=document.getElementById("amxcfgpanel");
  if(p){p.remove();return;}
  fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({op:"get_automix"})}).then(r=>r.json()).then(j=>{
    if(!j.ok)return;
    const nm=i=>i<8?"M"+(i+1):i<16?"U"+(i-7):"P"+(i-15);
    p=document.createElement("div");
    p.id="amxcfgpanel";
    p.style.cssText="position:absolute;top:60px;right:20px;z-index:80;width:420px;"
      +"background:#12161a;border:1px solid #39434b;border-radius:8px;padding:12px;"
      +"display:flex;flex-direction:column;gap:6px;max-height:70%;overflow-y:auto;";
    const row=(lbl,min,max,step,val,unit,cb)=>{
      const d=document.createElement("div");
      d.style.cssText="display:flex;align-items:center;gap:8px;";
      d.innerHTML=`<span style="width:96px;font-size:10px;font-weight:700;color:var(--mut);">${lbl}</span>
        <input type="range" min="${min}" max="${max}" step="${step}" value="${val}" style="flex:1;">
        <span style="width:64px;font-family:monospace;font-weight:700;font-size:11px;">${val} ${unit}</span>`;
      const sl=d.querySelector("input"),tv=d.querySelector("span:last-child");
      sl.addEventListener("input",()=>{tv.textContent=sl.value+" "+unit;cb(+sl.value);});
      return d;};
    const head=document.createElement("div");
    head.style.cssText="display:flex;align-items:center;";
    head.innerHTML=`<span style="color:var(--accent);font-size:11px;font-weight:700;letter-spacing:2px;">AUTOMIX DUGAN · RÉGLAGES</span>
      <span style="flex:1"></span><button class="wbtn" id="amxcfgx">&#10005;</button>`;
    p.appendChild(head);
    p.appendChild(row("RESPONSE",10,2000,10,Math.round(j.resp_ms),"ms",
      v=>post({op:"set_automix_cfg",resp_ms:v})));
    p.appendChild(row("FLOOR",-40,0,0.5,j.floor_db.toFixed(1),"dB",
      v=>post({op:"set_automix_cfg",floor_db:v})));
    const wt=document.createElement("div");
    wt.style.cssText="font-size:10px;font-weight:700;color:var(--accent);letter-spacing:2px;margin-top:4px;";
    wt.textContent="POIDS PAR TRANCHE (membres)";
    p.appendChild(wt);
    let any=false;
    (j.members||[]).forEach((m,i)=>{
      if(m!==1)return; any=true;
      const w=(j.weights_db||[])[i]||0;
      p.appendChild(row("POIDS "+nm(i),-20,20,0.5,w.toFixed(1),"dB",
        v=>post({op:"set_automix",src:i,weight_db:v})));
    });
    if(!any){const e=document.createElement("div");
      e.style.cssText="font-size:10px;color:var(--mut);";
      e.textContent="Aucune tranche membre — active « A » sur les tranches à grouper.";
      p.appendChild(e);}
    document.body.appendChild(p);
    document.getElementById("amxcfgx").addEventListener("click",()=>p.remove());
  });
});
setInterval(()=>{
  if(!window.LIVE||document.hidden)return;
  fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({op:"get_automix"})})
  .then(r=>r.json()).then(j=>{
    if(!j||!j.ok)return;
    window.__amxOn=j.on===1;
    const gb=document.getElementById("amx-global");
    if(gb)gb.classList.toggle("on",window.__amxOn);
    BANKS[curBank].strips.forEach((sd,i)=>{
      if(sd.t!=="in"||sd.idx>=j.members.length)return;
      const el=bank.children[i];if(!el)return;
      const on=j.members[sd.idx]===1;
      el.querySelector(".sq.amx").classList.toggle("on",on);
      const bar=el.querySelector(".amxbar");
      bar.style.display=(on&&j.on===1)?"":"none";
      bar.firstElementChild.style.width=
        Math.max(0,Math.min(100,(j.gains_db[sd.idx]+15)/15*100))+"%";
    });
  }).catch(()=>{});
},1000);
/* master fader → gains de sortie 0+1 (dB) */
const mtr=document.querySelector(".ftrack[data-m]");
if(mtr)mtr.addEventListener("pointermove",()=>{
  if(!window.LIVE)return;
  const db=master.fader*84-72;
  postT("m0",{op:"set_output_gain",out:0,db:db});
  postT("m1",{op:"set_output_gain",out:1,db:db});
});

connectState();connectMeters();
})();

