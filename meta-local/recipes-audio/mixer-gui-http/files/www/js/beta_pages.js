/* A.L.A. console — pages EFFETS + MASTERING/ROUTING/SYSTÈME
 * Extrait de beta.html (V14.0 étape 7, tranche contiguë — ordre de
 * chargement = ordre d'origine, sémantique identique au script unique). */
/* ================== V10-P2a : page EFFETS branchée au réel ==================
 * list_lv2_plugins (cat/ins réels) + get_fx (params/ranges/meta) →
 * rack dynamique : knob (continu/log/int), switch (toggle), select (enum).
 * set_fx_engine au choix d'un plugin, set_fx_param throttlé au réglage. */
(function(){
"use strict";
let fxBus=0, fxPlugins=null, fxCat="TOUS";
const CATS={TOUS:null, DYNAMIQUE:/comp|gate|limit|expander|deess/i,
            EQ:/\beq\b|equal|filter|shelf/i, REVERB:/reverb|room|hall|plate/i,
            MOD:/chorus|phaser|flanger|tremolo|vibrato|rotary/i,
            DELAY:/delay|echo/i};
const post=(o)=>fetch("/api/cmd",{method:"POST",
  headers:{"Content-Type":"application/json"},body:JSON.stringify(o)})
  .then(r=>r.json()).catch(()=>null);
const thm={},trm={};
function postT(key,o){const now=performance.now();clearTimeout(trm[key]);
  if(now-(thm[key]||0)>45){thm[key]=now;post(o);}
  else trm[key]=setTimeout(()=>{thm[key]=performance.now();post(o);},45);}

/* V13.4 : VU du bus courant — cibles 10 Hz, ballistique rAF */
let fxMeters={fx:[],inl:[]};
setInterval(()=>{
  if(window.CURPAGE!=="EFFETS")return;
  post({op:"get_meters_lite"}).then(j=>{
    if(j&&j.ok){fxMeters.fx=j.fx||[];fxMeters.inl=j.in||[];}
  });
},100);
(function(){
  const disp={inL:0,inR:0,outL:0,outR:0};
  let last=performance.now();
  const dbn=v=>{if(!v||v<=0)return 0;
    const db=20*Math.log10(v/2147483647);
    return Math.max(0,Math.min(1,(db+48)/48));};
  function tick(){
    const now=performance.now(),dt=Math.min((now-last)/1000,0.1);last=now;
    if(window.CURPAGE==="EFFETS"){
      const b=fxBus*2;
      const kA=1-Math.exp(-dt/0.030),kR=1-Math.exp(-dt/0.120);
      const st=(k,t)=>{disp[k]+=(t-disp[k])*(t>disp[k]?kA:kR);};
      st("inL",dbn(fxMeters.fx[b]));st("inR",dbn(fxMeters.fx[b+1]));
      st("outL",dbn(fxMeters.inl[18+b]));st("outR",dbn(fxMeters.inl[19+b]));
      const put=(id,v)=>{const e=document.getElementById(id);if(e)e.style.width=(v*100)+"%";};
      put("fxvinL",disp.inL);put("fxvinR",disp.inR);
      put("fxvoutL",disp.outL);put("fxvoutR",disp.outR);
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
})();

/* --- factory knob (repris du moteur maquette, version dynamique) --- */
function makeKnob(o){ // {label,min,max,val,unit,log,step,onchange}
  const u=document.createElement("div");u.className="kunit";
  u.innerHTML=`<div class="knob"><svg viewBox="0 0 40 40">
    <circle cx="20" cy="20" r="17" fill="#12171b" stroke="#05070a"/>
    <circle cx="20" cy="20" r="14.5" fill="url(#kg)" stroke="#31383f" stroke-width="1"/>
    <path class="karc" d="" fill="none" stroke="#e5a13c" stroke-width="2.4" stroke-linecap="round"/>
    <line class="kpin" x1="20" y1="20" x2="20" y2="8.5" stroke="#e9e5da" stroke-width="2.2" stroke-linecap="round"/>
    </svg></div>
    <span class="kn">${o.label.toUpperCase().slice(0,11)}</span><span class="kv"></span>`;
  const k=u.querySelector(".knob"),kv=u.querySelector(".kv");
  let val=o.val;
  const lo=o.log?Math.max(o.min,1e-6):o.min;
  const norm=v=>o.log?Math.log(Math.max(v,lo)/lo)/Math.log(o.max/lo):(v-o.min)/(o.max-o.min||1);
  const dnorm=n=>o.log?lo*Math.pow(o.max/lo,n):o.min+n*(o.max-o.min);
  const dec=(o.max-o.min>50||o.log)?(o.max-o.min>500?0:1):2;
  function draw(){
    const n=Math.max(0,Math.min(1,norm(val)));
    const a0=-135,a1=a0+n*270,c=20,r=17.8,rad=d=>d*Math.PI/180;
    const x0=c+r*Math.sin(rad(a0)),y0=c-r*Math.cos(rad(a0));
    const x1=c+r*Math.sin(rad(a1)),y1=c-r*Math.cos(rad(a1));
    k.querySelector(".karc").setAttribute("d",
      `M ${x0} ${y0} A ${r} ${r} 0 ${(a1-a0)>180?1:0} 1 ${x1} ${y1}`);
    k.querySelector(".kpin").setAttribute("transform",`rotate(${a1} 20 20)`);
    kv.textContent=val.toFixed(o.step===1?0:dec)+(o.unit?" "+o.unit:"");
  }
  draw();
  k.addEventListener("pointerdown",e=>{
    const y0=e.clientY,n0=norm(val);k.setPointerCapture(e.pointerId);
    u.classList.add("dragging");
    const mv=e=>{val=dnorm(Math.max(0,Math.min(1,n0+(y0-e.clientY)*0.006)));
      if(o.step===1)val=Math.round(val);draw();o.onchange(val);};
    const up=()=>{k.removeEventListener("pointermove",mv);k.removeEventListener("pointerup",up);
      u.classList.remove("dragging");};
    k.addEventListener("pointermove",mv);k.addEventListener("pointerup",up);
  });
  u.setVal=v=>{val=v;draw();};   /* maj externe (suivi STORE) */
  return u;
}
window.MXKNOB=makeKnob;

function inferGroup(key,m){
  if(m&&m.grp)return m.grp;
  const s=(m&&m.label)||key;
  const mb=s.match(/(?:^|[ _])(\d+)(?:[ _]|$)/);
  return mb?("BANDE "+mb[1]):"GÉNÉRAL";
}

function renderRack(fx){
  const rack=document.getElementById("fxrack");
  const name=(fx.type==="lv2")?(fx.uri||"?").split("/").pop():fx.type;
  let html=`<div class="rackhead"><span class="pname">${name}</span>
    <span class="pmeta">${fx.type==="lv2"?"LV2":"NATIF"} · BUS FX${fxBus+1}</span>
    ${fx.type&&fx.type!=="passthrough"
      ?'<button class="wbtn warn" id="fxremove">✕ RETIRER L\'EFFET</button>':""}</div>`;
  rack.innerHTML=html;
  const rm=document.getElementById("fxremove");
  if(rm)rm.addEventListener("click",async()=>{
    await post({op:"set_fx_engine",bus:fxBus,engine:"passthrough"});
    const nfx=await post({op:"get_fx",bus:fxBus});
    if(nfx&&nfx.ok)renderRack(nfx);
  });
  if(fx.type!=="lv2"||!fx.params){
    const d=document.createElement("div");d.className="fxcard";
    d.innerHTML=`<div class="ct">MOTEUR ${String(fx.type||"?").toUpperCase()}</div>
      <div style="font-size:10px;color:var(--mut)">Choisis un effet dans la liste ←</div>`;
    rack.appendChild(d);return;
  }
  const meta=fx.meta||{},ranges=fx.ranges||{};
  const groups={},order=[];
  for(const key in fx.params){
    const g=inferGroup(key,meta[key]);
    if(!groups[g]){groups[g]=[];order.push(g);}
    groups[g].push(key);
  }
  order.sort((a,b)=>(a==="GÉNÉRAL"?-1:b==="GÉNÉRAL"?1:0));
  const wrap=document.createElement("div");wrap.className="fxgrid";
  order.forEach(g=>{
    const card=document.createElement("div");card.className="fxcard";
    card.innerHTML=`<div class="ct">${g} <em>· ${groups[g].length}</em></div>`;
    const krow=document.createElement("div");krow.className="krow";
    krow.style.flexWrap="wrap";krow.style.gap="8px";
    groups[g].forEach(key=>{
      const m=meta[key]||{},r=ranges[key]||{};
      const kind=(m.kind||0)&0x0f, log=((m.kind||0)&0x10)!==0;
      const val=Number(fx.params[key])||0;
      const send=v=>postT(fxBus+":"+key,{op:"set_fx_param",bus:fxBus,param:key,value:v});
      if(kind===1){ /* toggle */
        const d=document.createElement("div");d.className="kunit";
        d.innerHTML=`<div class="pwrsw${val>0.5?" on":""}"><i></i></div>
          <span class="kn">${(m.label||key).toUpperCase().slice(0,11)}</span>`;
        d.querySelector(".pwrsw").addEventListener("click",function(){
          this.classList.toggle("on");
          send(this.classList.contains("on")?1:0);});
        krow.appendChild(d);
      }else if(kind===2&&m.sp){ /* enum */
        const d=document.createElement("div");d.className="kunit";d.style.minWidth="110px";
        const opts=m.sp.split(";").map(s=>{const i=s.indexOf("=");
          return {v:parseFloat(s.slice(0,i)),l:s.slice(i+1)};})
          .sort((a,b)=>a.v-b.v);
        d.innerHTML=`<select style="width:100%;font-size:9px;background:var(--well);color:var(--text);border:1px solid var(--line-hi);border-radius:4px;padding:3px">${
          opts.map(o=>`<option value="${o.v}"${Math.round(val)===o.v?" selected":""}>${o.l}</option>`).join("")}</select>
          <span class="kn">${(m.label||key).toUpperCase().slice(0,11)}</span>`;
        d.querySelector("select").addEventListener("change",e=>send(parseFloat(e.target.value)));
        krow.appendChild(d);
      }else{ /* knob continu / int */
        const min=(r.min!=null&&isFinite(r.min))?r.min:0;
        const max=(r.max!=null&&isFinite(r.max)&&r.max>min)?r.max:(min+1);
        krow.appendChild(makeKnob({label:m.label||key,min,max,
          val:Math.max(min,Math.min(max,val)),unit:m.unit||"",
          log,step:kind===3?1:0,onchange:send}));
      }
    });
    card.appendChild(krow);wrap.appendChild(card);
  });
  rack.appendChild(wrap);
}

function renderList(){
  const list=document.querySelector(".fxlist");
  if(!fxPlugins){list.innerHTML='<div class="fxit">chargement…</div>';return;}
  const rx=CATS[fxCat];
  list.innerHTML="";
  const none=document.createElement("div");
  none.className="fxit";
  none.style.color="var(--accent)";
  none.innerHTML="— AUCUN EFFET (bus neutre) —";
  none.addEventListener("click",async()=>{
    await post({op:"set_fx_engine",bus:fxBus,engine:"passthrough"});
    const nfx=await post({op:"get_fx",bus:fxBus});
    if(nfx&&nfx.ok)renderRack(nfx);
  });
  list.appendChild(none);
  fxPlugins.filter(p=>!rx||rx.test(p.name)).forEach(p=>{
    const d=document.createElement("div");d.className="fxit";
    d.innerHTML=`${p.name}<em>${p.ai}/${p.ao}</em>`;
    d.addEventListener("click",async()=>{
      list.querySelectorAll(".fxit").forEach(x=>x.classList.remove("on"));
      d.classList.add("on");
      await post({op:"set_fx_engine",bus:fxBus,engine:"lv2",uri:p.uri});
      setTimeout(async()=>{
        const fx=await post({op:"get_fx",bus:fxBus});
        if(fx&&fx.ok)renderRack(fx);
      },350);
    });
    list.appendChild(d);
  });
}

async function openFx(){
  if(!window.LIVE)return;              /* mode démo : maquette statique */
  if(!fxPlugins){
    const j=await post({op:"list_lv2_plugins"});
    if(j&&j.plugins)
      fxPlugins=j.plugins.filter(p=>p.cat==="effet"&&p.ins)
        .sort((a,b)=>a.name.localeCompare(b.name));
  }
  renderList();
  const fx=await post({op:"get_fx",bus:fxBus});
  if(fx&&fx.ok)renderRack(fx);
}

window.addEventListener("mixer-live",()=>{
  const p=document.getElementById("fxpage");
  if(p&&!p.classList.contains("hid"))openFx();
});
/* hooks : ouverture du tab EFFETS + bus + cats */

document.querySelectorAll(".tab").forEach(t=>t.addEventListener("click",()=>{
  if(t.textContent.trim()==="EFFETS")openFx();
}));
document.querySelectorAll(".fxbus").forEach((b,i)=>b.addEventListener("click",()=>{
  fxBus=i;if(window.LIVE)openFx();
}));
document.querySelectorAll(".fxcat").forEach(b=>{
  b.addEventListener("click",()=>{
    fxCat=b.textContent.trim().split(" ")[0];
    if(fxCat!=="TOUS"&&!CATS[fxCat])fxCat="TOUS";
    if(window.LIVE)renderList();
  });
});
/* renomme les chips pour matcher CATS */
(function(){const c=document.querySelectorAll(".fxcat");
 const names=["TOUS","DYNAMIQUE","EQ","REVERB","MOD"];
 c.forEach((el,i)=>{if(names[i])el.textContent=names[i];});})();
})();

/* ================== V10-P2b : pages MASTERING / ROUTING / SYSTÈME ========= */
(function(){
"use strict";
const post=(o)=>fetch("/api/cmd",{method:"POST",
  headers:{"Content-Type":"application/json"},body:JSON.stringify(o)})
  .then(r=>r.json()).catch(()=>null);
const thm={},trm={};
function postT(key,o){const now=performance.now();clearTimeout(trm[key]);
  if(now-(thm[key]||0)>45){thm[key]=now;post(o);}
  else trm[key]=setTimeout(()=>{thm[key]=performance.now();post(o);},45);}
window.STORE=window.STORE||{};

const OUT_NAMES=[..."12345678"].map(n=>"S"+n)
  .concat([..."12345678"].map(n=>"U"+n)).concat(["P1","P2"]);
const IN_NAMES=[..."12345678"].map(n=>"M"+n)
  .concat([..."12345678"].map(n=>"U"+n)).concat(["P1","P2"])
  .concat([..."12345678"].map(n=>"R"+n));

/* ---------- MASTERING ---------- */
function drawMlEnv(){
  const cv=document.getElementById("mlenv");
  if(!cv||cv.offsetParent===null)return;
  const r=cv.getBoundingClientRect();
  if(cv.width!==r.width*DPR){cv.width=r.width*DPR;cv.height=r.height*DPR;}
  const x=cv.getContext("2d"),w=cv.width,h=cv.height;
  x.clearRect(0,0,w,h);
  x.strokeStyle="rgba(255,255,255,.06)";x.lineWidth=1;
  for(let g=1;g<4;g++){x.beginPath();x.moveTo(0,h*g/4);x.lineTo(w,h*g/4);x.stroke();}
  x.strokeStyle="rgba(255,255,255,.18)";
  x.beginPath();x.moveTo(0,h/2);x.lineTo(w,h/2);x.stroke();
  const ins=window.STORE.insert;
  if(!ins||!ins.chain||!ins.chain[0]||!ins.chain[0].l)return;
  [["l","#4cc470"],["r","#e5a13c"]].forEach(([ch,col])=>{
    const env=ins.chain[0][ch];if(!env)return;
    x.strokeStyle=col;x.lineWidth=1.8*DPR;x.shadowColor=col;x.shadowBlur=3;
    x.beginPath();
    env.forEach((g,i)=>{
      const px=i/(env.length-1)*w, py=h/2-(g/12)*(h/2)*0.92;
      i?x.lineTo(px,py):x.moveTo(px,py);
    });
    x.stroke();x.shadowBlur=0;
  });
}
/* V13.4 : réduction limiteur FLUIDE (cible g_in via SSE, ballistique rAF) */
(function(){
  let disp=0,last=performance.now();
  function tick(){
    const now=performance.now(),dt=Math.min((now-last)/1000,0.1);last=now;
    if(window.CURPAGE==="MASTERING"){
      const lim=(window.STORE&&STORE.insert&&STORE.insert.chain)?(STORE.insert.chain[2]||{}):{};
      let tgt=0;
      if(lim.g_in!==undefined&&lim.g_in>0)
        tgt=Math.max(0,Math.min(1,(-20*Math.log10(lim.g_in))/12));
      const kA=1-Math.exp(-dt/0.030),kR=1-Math.exp(-dt/0.200);
      disp+=(tgt-disp)*(tgt>disp?kA:kR);
      const b=document.getElementById("ml-grbar"),v=document.getElementById("ml-grv");
      if(b)b.style.width=(disp*100)+"%";
      if(v)v.textContent=(disp*12).toFixed(1)+" dB";
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
})();

function renderMastering(){
  const S=window.STORE;
  if(S.assistant){
    const m=document.getElementById("mst-mode"),s=document.getElementById("mst-src");
    if(m&&document.activeElement!==m)m.value=S.assistant.mode||"passthrough";
    if(s&&document.activeElement!==s)s.value=S.assistant.source||"usb";
  }
  const ins=S.insert;
  if(ins&&ins.chain){
    const exc=ins.chain[1]||{},lim=ins.chain[2]||{};
    const set=(id,v)=>{const e=document.getElementById(id);if(e)e.textContent=v;};
    set("mx-amount",(exc.amount??0).toFixed(3));
    set("mx-drive",(exc.drive??0).toFixed(1));
    set("mx-freq",((exc.freq??0)/1000).toFixed(1)+" kHz");
    set("ml-th",(lim.th??1).toFixed(3)+" ("+(20*Math.log10(lim.th||1)).toFixed(1)+" dB)");
    set("ml-gin",(lim.g_in??1).toFixed(2)+" ("+(20*Math.log10(lim.g_in||1)).toFixed(1)+" dB)");
    set("ml-ar",(lim.at??0).toFixed(1)+" / "+(lim.rt??0).toFixed(0)+" ms");
    [["mst-b0",0,ins.chain[0]],["mst-b1",1,exc],["mst-b2",2,lim]].forEach(([id,slot,p])=>{
      const el=document.getElementById(id);
      if(el&&p)el.classList.toggle("on",!(p.bypass>0.5));
    });
  }
  drawMlEnv();
}
["mst-mode","mst-src"].forEach(id=>{
  document.addEventListener("change",e=>{
    if(e.target.id!==id)return;
    const m=document.getElementById("mst-mode").value,
          s=document.getElementById("mst-src").value;
    post({op:"set_assistant_mode",mode:m,source:s});
  });
});
[["mst-b0",0],["mst-b1",1],["mst-b2",2]].forEach(([id,slot])=>{
  document.addEventListener("click",e=>{
    const el=e.target.closest("#"+id);if(!el)return;
    el.classList.toggle("on");
    post({op:"set_insert_param",slot,param:"bypass",
          value:el.classList.contains("on")?0:1});
  });
});

/* ---------- ROUTING (V10-P2k : knobs console, valeurs réelles) ---------- */
let routingBuilt=false;
const rtMx=[],rtOuts=[];
function rtLoadMatrix(src){
  fetch("/api/cmd",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({op:"get_strip_routing",src})})
  .then(r=>r.json()).then(j=>{
    if(!j||!j.ok||!Array.isArray(j.master))return;
    rtMx.forEach((u,o)=>{
      if(u.classList.contains("dragging"))return;
      const g=j.master[o]||0;
      u.setVal(g<=0.000316?-60:Math.max(-60,20*Math.log10(g)));
    });
  }).catch(()=>{});
}
const OUT_GROUPS=[["DSP OUT",0,8],["USB OUT",8,16],["TÉL OUT",16,18]];
function rtGroup(parent,title){
  const g=document.createElement("div");g.className="rtgrp";
  const t=document.createElement("div");t.className="gt";t.textContent=title;
  g.appendChild(t);
  const grid=document.createElement("div");grid.className="kgridf";
  g.appendChild(grid);parent.appendChild(g);
  return grid;
}
function buildRouting(){
  if(routingBuilt)return;routingBuilt=true;
  const rm=document.getElementById("rt-remap");
  for(let m=0;m<8;m++){
    const d=document.createElement("div");d.className="kcell";
    const sel=document.createElement("select");
    sel.dataset.mic=m;
    sel.innerHTML=[...Array(8)].map((_,s)=>`<option value="${s}">src ${s+1}</option>`).join("");
    sel.addEventListener("change",e=>post({op:"set_input_map",mic:m,slot:+e.target.value}));
    d.appendChild(sel);
    const lb=document.createElement("span");lb.className="kn";lb.textContent="M"+(m+1);
    d.appendChild(lb);
    rm.appendChild(d);
  }
  const sel=document.getElementById("rt-src");
  const IN_GROUPS=[["DSP IN",0,8],["USB IN",8,16],["TÉL IN",16,18],["RETOURS FX",18,26]];
  sel.innerHTML=IN_GROUPS.map(([t,a,b])=>
    `<optgroup label="${t}">`+IN_NAMES.slice(a,b).map((n,k)=>
      `<option value="${a+k}">${n} — entrée ${a+k}</option>`).join("")+"</optgroup>").join("");
  sel.addEventListener("change",()=>rtLoadMatrix(+sel.value));
  const mx=document.getElementById("rt-matrix");
  OUT_GROUPS.forEach(([t,a,b])=>{
    const grid=rtGroup(mx,t);
    for(let o=a;o<b;o++){
      const u=MXKNOB({label:OUT_NAMES[o],min:-60,max:6,val:-60,unit:"dB",step:0.5,
        onchange:db=>{
          const gain=db<=-59?0:Math.pow(10,db/20);
          postT("mx"+o,{op:"set_master",src:+sel.value,out:o,gain});
        }});
      rtMx.push(u);grid.appendChild(u);
    }
  });
  const outs=document.getElementById("rt-outs");
  OUT_GROUPS.forEach(([t,a,b])=>{
    const grid=rtGroup(outs,t);
    for(let o=a;o<b;o++){
      const u=MXKNOB({label:OUT_NAMES[o],min:-60,max:12,val:0,unit:"dB",step:0.5,
        onchange:db=>postT("og"+o,{op:"set_output_gain",out:o,db})});
      rtOuts.push(u);grid.appendChild(u);
    }
  });
  rtLoadMatrix(0);
}
function renderRouting(){
  buildRouting();
  const S=window.STORE;
  if(S.input_map&&S.input_map.map)
    document.querySelectorAll("#rt-remap select").forEach((s,m)=>{
      if(document.activeElement!==s)s.value=S.input_map.map[m];
    });
  if(S.output_gain&&S.output_gain.gains)
    rtOuts.forEach((u,o)=>{
      if(u.classList.contains("dragging"))return;
      const g=S.output_gain.gains[o]||1000;
      u.setVal(g<=0?-60:20*Math.log10(g/1000));
    });
}

/* ---------- SYSTÈME ---------- */
function renderSystem(){
  const S=window.STORE;
  const l=document.getElementById("sys-loads");
  if(l&&S.sysload){
    const rows=[["CPU 0",S.sysload.cpu?.[0]],["CPU 1",S.sysload.cpu?.[1]],
      ["CPU 2 (audio RT)",S.sysload.cpu?.[2]],["CPU 3",S.sysload.cpu?.[3]],
      ["DSP HiFi4",S.sysload.dsp],["NPU",S.sysload.npu],["GPU",S.sysload.gpu]];
    l.innerHTML=rows.map(([n,v])=>`<div class="mn"><span>${n}</span>
      <span style="display:flex;align-items:center;gap:6px;">
      <span class="loadbar" style="width:70px"><i style="width:${Math.max(0,Math.min(100,v??0))}%"></i></span>
      <b style="min-width:32px;text-align:right">${v==null||v<0?"—":v+"%"}</b></span></div>`).join("");
  }
  const a=document.getElementById("sys-audio");
  if(a&&S.stat){
    const s=S.stat;
    a.innerHTML=`<div class="mn"><span>VERSION</span><b>${s.version||"-"}</b></div>
      <div class="mn"><span>XRUN</span><b style="color:${s.xrun?"var(--warn)":"var(--good)"}">${s.xrun}</b></div>
      <div class="mn"><span>LATENCE</span><b>${((s.latency_us_one_way||0)/1000).toFixed(1)} ms</b></div>
      <div class="mn"><span>PROF CAP/MIX/ITER</span><b>${s.prof_cap_us}/${s.prof_mix_us}/${s.prof_iter_us} µs</b></div>
      <div class="mn"><span>RING DROPS</span><b>${s.ring_drops}</b></div>
      <div class="mn"><span>FPS GUI</span><b style="color:${(window.__fps&&window.__fps.v||0)<30?"var(--warn)":"var(--good)"}">${window.__fps&&window.__fps.v||"-"}</b></div>`;
  }
}
let driftTimer=null;
function pollDrift(){
  const el=document.getElementById("sys-drift");
  if(!el||el.offsetParent===null)return;
  fetch("/api/drift").then(r=>r.json()).then(d=>{
    el.innerHTML=`<div class="mn"><span>DRIFT</span><b>${(d.ppm??0).toFixed?.(2)??d.ppm} ppm</b></div>
      <div class="mn"><span>WR µs min/avg/max</span><b>${d.wr_us_min??"-"}/${d.wr_us_avg??"-"}/${d.wr_us_max??"-"}</b></div>
      <div class="mn"><span>RD µs min/avg/max</span><b>${d.rd_us_min??"-"}/${d.rd_us_avg??"-"}/${d.rd_us_max??"-"}</b></div>
      <div class="mn"><span>ERR CAP/PLAY</span><b>${d.xruns_cap??0} / ${d.xruns_play??0}</b></div>`;
  }).catch(()=>{});
}
document.addEventListener("click",e=>{
  if(e.target.id==="sys-rststats")fetch("/api/reset_drift_stats");
  if(e.target.id==="sys-tac"){
    e.target.textContent="RESET EN COURS…";
    fetch("/api/tac/reset",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({mode:"analog"})}).then(r=>r.json()).then(j=>{
      document.getElementById("sys-tac-out").textContent=JSON.stringify(j);
      e.target.textContent="RESET TACS (i2c)";}).catch(()=>{e.target.textContent="RESET TACS (i2c)";});
  }
});

/* ---------- dispatch : refresh de la page visible ---------- */
window.renderActivePage=function(){
  const vis=p=>{const e=document.getElementById(p);return e&&!e.classList.contains("hid");};
  if(vis("mastpage"))renderMastering();
  if(vis("routepage"))renderRouting();
  if(vis("syspage")){renderSystem();pollDrift();}
};

document.querySelectorAll(".tab").forEach(t=>t.addEventListener("click",()=>{
  setTimeout(window.renderActivePage,30);
}));
setInterval(()=>{if(window.LIVE)window.renderActivePage();},1000);
})();

