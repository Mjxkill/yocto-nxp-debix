/* A.L.A. console — PADS / LOOPER / EXPANDEUR
 * Extrait de beta.html (V14.0 étape 7, tranche contiguë — ordre de
 * chargement = ordre d'origine, sémantique identique au script unique). */
/* ================= V12 web : PADS / LOOPER / EXPANDEUR =================
 * Miroir des pages natives. Tout passe par /api/cmd (passthrough JSON
 * vers /run/mixer-pro.sock). Pollers gatés sur window.CURPAGE. */
(function(){
"use strict";
const cmd=(o)=>fetch("/api/cmd",{method:"POST",
  headers:{"Content-Type":"application/json"},body:JSON.stringify(o)})
  .then(r=>r.json()).catch(()=>({ok:false}));
const dbw=(peak,ref)=>{             /* peak s32 → largeur % (-48..0 dB) */
  const f=peak/2147483647; if(f<=0)return 0;
  const db=20*Math.log10(f); return Math.max(0,Math.min(100,(db+48)/48*100));};
const esc=(s)=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));

/* ---------------- PADS ---------------- */
function padRender(slots){
  const g=document.getElementById("padgrid");
  g.innerHTML=slots.map((s,i)=>{
    const has=s.name!=="", on=s.playing===1;
    return `<div data-slot="${i}" style="height:86px;border-radius:8px;cursor:pointer;
      display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;
      background:${on?"#3a2c10":has?"#1b2126":"#14181c"};
      border:${on?"2px solid #e5a13c":"1px solid #39434b"};">
      <div style="font-size:13px;font-weight:700;color:${on?"#e5a13c":has?"#e9e5da":"#3a434b"};
        max-width:90%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${has?esc(s.name):"—"}</div>
      <div style="font-size:9px;color:var(--mut);font-family:monospace;">
        ${has?(on?s.pos_s.toFixed(1)+" / ":"")+s.len_s.toFixed(1)+" s":""}</div></div>`;}).join("");
  g.querySelectorAll("[data-slot]").forEach(el=>el.addEventListener("click",()=>{
    const s=slots[+el.dataset.slot];
    if(s&&s.name!=="")cmd({op:"sampler_trigger",slot:+el.dataset.slot,gain_db:0}).then(padPoll);
  }));
}
function padPoll(){
  if(window.CURPAGE!=="PADS")return;
  cmd({op:"sampler_list"}).then(r=>{if(r.ok)padRender(r.slots);});
}
setInterval(padPoll,500);
document.getElementById("padreload").addEventListener("click",()=>cmd({op:"sampler_reload"}).then(padPoll));
document.getElementById("padstopall").addEventListener("click",()=>cmd({op:"sampler_stop",slot:-1}).then(padPoll));

/* ---------------- LOOPER ---------------- */
const TRCOL=["#e5a13c","#4cc470","#5aa9e6","#c77dff","#e0678a","#e8b84b"];
const srcName=(i)=>i<0?"—":(i<8?"M"+(i+1):"USB"+(i-7));
let loopSt=null;
function loopRender(st){
  const c=document.getElementById("looptracks");
  document.getElementById("loopinfo").textContent=
    st.master_len_s>0?("boucle "+st.master_len_s.toFixed(2)+" s   "+st.pos_s.toFixed(1)+" s")
                     :"aucune boucle — enregistrez une 1re piste";
  document.getElementById("looppos").style.width=
    st.master_len_s>0?(st.pos_s/st.master_len_s*100)+"%":"0";
  document.getElementById("looppos").style.background=st.run?"#4cc470":"#5c666e";
  const mv=document.getElementById("loopmvu");
  mv.style.width=dbw(st.master_peak||0)+"%";
  mv.style.background=(st.master_peak||0)>1932735283?"#e05545":"#4cc470";
  c.innerHTML=st.tracks.map((t,i)=>{
    const rec=t.state==="rec", play=t.state==="play", empty=t.state==="empty",
          armed=t.state==="armed";
    const mut=t.muted===1, ac=TRCOL[i];
    return `<div style="display:flex;align-items:center;gap:10px;border-radius:8px;padding:8px 10px;
      background:${rec?"#2a1512":armed?"#241d10":play?"#141b16":"#14181c"};
      border:${rec?"2px solid #e05545":armed?"2px solid #e8b84b":(play&&!mut)?"2px solid "+ac:"1px solid #22282e"};
      ${armed?"animation:looparm 0.9s ease-in-out infinite alternate;":""}">
      <div style="width:34px;height:34px;border-radius:17px;display:flex;align-items:center;justify-content:center;
        background:${empty?"#1b2126":ac};color:${empty?"#5c666e":"#0b0e11"};font-weight:700;">${i+1}</div>
      <button class="wbtn" data-t="${i}" data-a="srcdn" ${rec?"disabled":""}>&#8249;</button>
      <span style="width:52px;text-align:center;font-weight:700;color:#e9e5da;">${srcName(t.src_a)}</span>
      <button class="wbtn" data-t="${i}" data-a="srcup" ${rec?"disabled":""}>&#8250;</button>
      <div style="width:110px;height:12px;border-radius:6px;background:#0b0e11;overflow:hidden;">
        <div style="height:100%;width:${(rec||(play&&!mut))?dbw(t.peak):0}%;background:${rec?"#e05545":ac};"></div></div>
      <span style="width:64px;font-family:monospace;font-size:10px;color:${rec?"#e05545":armed?"#e8b84b":"var(--mut)"};">
        ${t.len_s>0?t.len_s.toFixed(2)+" s":(rec?"● rec…":armed?"⏳ armé":"—")}</span>
      <input type="range" data-t="${i}" data-a="vol" min="-40" max="6" step="0.5" value="${t.gain_db}"
        style="width:110px;accent-color:${ac};" ${rec?"disabled":""}>
      <span data-vl="${i}" style="width:52px;font-family:monospace;font-size:10px;color:var(--mut);"
        >${t.gain_db>=0?"+":""}${t.gain_db.toFixed(1)} dB</span>
      <span style="flex:1"></span>
      <button class="wbtn warn" data-t="${i}" data-a="rec"  ${(empty||armed)?"":"disabled"}
        style="${armed?"color:#e8b84b;border-color:#e8b84b;":""}">${armed?"⏳ ARMÉ":"&#9679; REC"}</button>
      <button class="wbtn ok"   data-t="${i}" data-a="play" ${rec?"":"disabled"}>&#9654; PLAY</button>
      <button class="wbtn"      data-t="${i}" data-a="${mut?"unmute":"mute"}" ${play?"":"disabled"}
        style="${mut?"color:#e8b84b;border-color:#e8b84b;":""}">${mut?"MUTED":"ON"}</button>
      <button class="wbtn warn" data-t="${i}" data-a="clear" ${(!empty&&!rec)?"":"disabled"}>&#10005;</button>
    </div>`;}).join("");
  c.querySelectorAll("input[data-a=vol]").forEach(sl=>{
    sl.addEventListener("input",()=>{
      const v=+sl.value;
      const lb=c.querySelector(`[data-vl="${sl.dataset.t}"]`);
      if(lb)lb.textContent=(v>=0?"+":"")+v.toFixed(1)+" dB";   /* écho local */
      cmd({op:"looper_track_cfg",track:+sl.dataset.t,gain_db:v});
    });
    /* le poll re-render tout : gel pendant le drag du curseur */
    sl.addEventListener("pointerdown",()=>{window._loopVolDrag=true;});
    sl.addEventListener("pointerup",()=>{window._loopVolDrag=false;});
    sl.addEventListener("pointercancel",()=>{window._loopVolDrag=false;});
  });
  c.querySelectorAll("button[data-a=clear]").forEach(b=>holdify(b,()=>{
    window._loopVolDrag=false;
    cmd({op:"looper_track_ctl",track:+b.dataset.t,action:"clear"}).then(loopPoll);
  },{freeze:()=>{window._loopVolDrag=true;},
     unfreeze:()=>{window._loopVolDrag=false;}}));
  c.querySelectorAll("button[data-a]").forEach(b=>b.addEventListener("click",()=>{
    const t=+b.dataset.t, a=b.dataset.a, tr=loopSt.tracks[t];
    if(a==="clear")return;   /* géré par holdify ci-dessus */
    if(a==="srcdn"||a==="srcup"){
      let s=tr.src_a+(a==="srcup"?1:-1); s=Math.max(0,Math.min(15,s));
      cmd({op:"looper_track_cfg",track:t,src_a:s,src_b:-1}).then(loopPoll);
    }else cmd({op:"looper_track_ctl",track:t,action:a}).then(loopPoll);
  }));
}
function loopPoll(){
  if(window.CURPAGE!=="LOOPER"||window._loopVolDrag)return;
  cmd({op:"looper_status"}).then(r=>{if(r.ok){loopSt=r;loopRender(r);}});
}
setInterval(loopPoll,300);
document.getElementById("loopplayall").addEventListener("click",()=>cmd({op:"looper_ctl",action:"play_all"}).then(loopPoll));
document.getElementById("loopstopall").addEventListener("click",()=>cmd({op:"looper_ctl",action:"stop_all"}).then(loopPoll));
document.getElementById("loopclearall").addEventListener("click",()=>cmd({op:"looper_ctl",action:"clear_all"}).then(loopPoll));

/* ---------------- EXPANDEUR ---------------- */
const GM=["Grand Piano","Bright Piano","Electric Grand","Honky-tonk",
"E.Piano 1","E.Piano 2","Harpsichord","Clavinet","Celesta","Glockenspiel",
"Music Box","Vibraphone","Marimba","Xylophone","Tubular Bells","Dulcimer",
"Drawbar Organ","Percussive Organ","Rock Organ","Church Organ","Reed Organ",
"Accordion","Harmonica","Tango Accordion","Nylon Guitar","Steel Guitar",
"Jazz Guitar","Clean Guitar","Muted Guitar","Overdrive Guitar",
"Distortion Guitar","Harmonics","Acoustic Bass","Finger Bass","Pick Bass",
"Fretless Bass","Slap Bass 1","Slap Bass 2","Synth Bass 1","Synth Bass 2",
"Violin","Viola","Cello","Contrabass","Tremolo Strings","Pizzicato","Harp",
"Timpani","String Ens. 1","String Ens. 2","Synth Strings 1","Synth Strings 2",
"Choir Aahs","Voice Oohs","Synth Voice","Orchestra Hit","Trumpet","Trombone",
"Tuba","Muted Trumpet","French Horn","Brass Section","Synth Brass 1",
"Synth Brass 2","Soprano Sax","Alto Sax","Tenor Sax","Baritone Sax","Oboe",
"English Horn","Bassoon","Clarinet","Piccolo","Flute","Recorder","Pan Flute",
"Blown Bottle","Shakuhachi","Whistle","Ocarina","Square Lead","Saw Lead",
"Calliope","Chiff Lead","Charang","Voice Lead","Fifths Lead","Bass+Lead",
"New Age Pad","Warm Pad","Polysynth Pad","Choir Pad","Bowed Pad",
"Metallic Pad","Halo Pad","Sweep Pad","Rain FX","Soundtrack","Crystal",
"Atmosphere","Brightness","Goblins","Echoes","Sci-Fi","Sitar","Banjo",
"Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai","Tinkle Bell",
"Agogo","Steel Drums","Woodblock","Taiko Drum","Melodic Tom","Synth Drum",
"Reverse Cymbal","Fret Noise","Breath Noise","Seashore","Bird Tweet",
"Telephone","Helicopter","Applause","Gunshot"];
let xpdSt=null, patchNames=[], instNames=[], editIdx=-1, pd=null;
function xpdRender(){
  const st=xpdSt; if(!st)return;
  document.getElementById("xpdstat").textContent=(st.sf2||"?")+" + M1";
  document.getElementById("xpdgv").textContent=(+st.gain).toFixed(2);
  if(document.activeElement!==document.getElementById("xpdgain"))
    document.getElementById("xpdgain").value=Math.round(st.gain*100);
  const c=document.getElementById("xpdchans");
  c.innerHTML=st.chans.map((prog,i)=>{
    const m1=(st.engines||[])[i]===1, pi=(st.patch||[])[i]||0;
    const drums=i===9, act=(st.act||[])[i]||0;
    const nm=m1?(patchNames[pi]||("Patch "+pi)):(drums?"Drum Kit "+prog:GM[prog]);
    return `<div style="display:flex;align-items:center;gap:8px;border-radius:6px;padding:6px 8px;
      background:${m1?"#181420":"#14181c"};border:1px solid ${m1?"#5a4a7a":drums?"#5a4a2a":"#22282e"};">
      <span style="width:44px;font-weight:700;color:var(--accent);font-size:11px;">CH ${i+1}</span>
      <button class="wbtn" data-c="${i}" data-a="eng"
        style="${m1?"color:#b3a5f0;border-color:#8a7ad0;":""}">${m1?"M1":"GM"}</button>
      <button class="wbtn" data-c="${i}" data-a="dn">&#8249;</button>
      <span style="width:170px;font-weight:700;font-size:12px;color:${m1?"#b3a5f0":"#e9e5da"};
        overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${esc(nm)}</span>
      <button class="wbtn" data-c="${i}" data-a="up">&#8250;</button>
      ${m1?`<button class="wbtn" data-c="${i}" data-a="edit" style="color:var(--accent);">ÉDIT</button>`:""}
      <div style="flex:1;height:10px;border-radius:5px;background:#0b0e11;overflow:hidden;">
        <div style="height:100%;width:${Math.min(100,act/10)}%;
          background:${m1?"#b3a5f0":drums?"#e8b84b":"#4cc470"};"></div></div>
    </div>`;}).join("");
  c.querySelectorAll("[data-a]").forEach(b=>b.addEventListener("click",()=>{
    const i=+b.dataset.c, a=b.dataset.a;
    const m1=(xpdSt.engines||[])[i]===1, pi=(xpdSt.patch||[])[i]||0;
    if(a==="eng")cmd({op:"midix_ctl",line:"engine "+i+" "+(m1?0:1)+" "+pi}).then(xpdPoll);
    else if(a==="edit")xpdEdit(pi);
    else if(m1){const p=(pi+(a==="up"?1:15))%16;
      cmd({op:"midix_ctl",line:"engine "+i+" 1 "+p}).then(xpdPoll);}
    else{let n=xpdSt.chans[i]+(a==="up"?1:-1);n=(n+128)%128;
      cmd({op:"midix_ctl",cmd:"prog",chan:i,num:n}).then(xpdPoll);}
  }));
}
function xpdPoll(){
  if(window.CURPAGE!=="EXPANDEUR")return;
  cmd({op:"midix_ctl",cmd:"status"}).then(r=>{if(r.ok){xpdSt=r;xpdRender();}});
  cmd({op:"get_midix"}).then(r=>{
    if(r.ok)document.getElementById("xpdvu").style.width=dbw(r.peak)+"%";});
  if(patchNames.length===0)
    cmd({op:"midix_ctl",line:"patch_list"}).then(r=>{if(r.ok)patchNames=r.patches;});
}
setInterval(xpdPoll,300);
document.getElementById("xpdgain").addEventListener("change",e=>
  cmd({op:"midix_ctl",cmd:"gain",value:e.target.value/100}));
document.getElementById("xpdpanic").addEventListener("click",()=>cmd({op:"midix_ctl",cmd:"panic"}));

/* ---------------- AUTO MIX web (assistant groupe) ---------------- */
const BMXROLES=["off","lead","choir","kick","snare","drums","bass","guitar","keys","line"];
const BMXNAMES=["—","VOIX LEAD","CHŒURS","GR. CAISSE","C. CLAIRE","BATTERIE","BASSE","GUITARE","CLAVIER","LIGNE"];
const bmxStrip=(i)=>i<8?"M"+(i+1):"U"+(i-7);
let bmxSt=null,bmxVf=null,bmxDugan=false,bmxVfDrag=false;
function bmxRender(){
  const st=bmxSt;if(!st)return;
  document.getElementById("bmxstat").textContent=
    st.measuring>=0?("mesure "+bmxStrip(st.measuring)+" "+st.meas_elapsed+"/12 s")
    :st.locking?"capture de l'équilibre… (30 s)"
    :st.live?"LIVE — équilibre tenu"
    :(st.ref_valid?"équilibre verrouillé, LIVE prêt":"assigne les rôles puis mesure");
  const av=document.getElementById("bmxauto");
  av.textContent=st.autolive?"● AUTOMIX LIVE":"AUTOMIX LIVE";
  av.style.background=st.autolive?"#142a19":"";
  av.style.color=st.autolive?"#4cc470":"";
  const lv=document.getElementById("bmxlive");
  lv.textContent=st.live?"LIVE ON":"LIVE OFF";
  lv.style.color=st.live?"#4cc470":"";
  const dg=document.getElementById("bmxdugan");
  dg.textContent=bmxDugan?"DUGAN ON":"DUGAN OFF";
  dg.style.color=bmxDugan?"var(--accent)":"";
  const c=document.getElementById("bmxchans");
  c.innerHTML=st.chans.map((ch,i)=>{
    const active=ch.role!=="off",meas=st.measuring===i;
    const ri=BMXROLES.indexOf(ch.role);
    const kdb=active?ch.keeper_db:0;
    return `<div style="display:flex;align-items:center;gap:6px;border-radius:6px;padding:5px 8px;
      background:${meas?"#2a1512":active?"#171c21":"#14181c"};
      border:1px solid ${meas?"#e05545":active?"#39434b":"#22282e"};">
      <span style="width:34px;font-weight:700;color:var(--accent);font-size:11px;">${bmxStrip(i)}</span>
      <button class="wbtn" data-br="${i}" data-d="-1" style="padding:2px 6px;">&#8249;</button>
      <span style="width:84px;text-align:center;font-weight:700;font-size:10px;
        color:${active?"#e9e5da":"#3a434b"};">${ri>=0?BMXNAMES[ri]:ch.role}</span>
      <button class="wbtn" data-br="${i}" data-d="1" style="padding:2px 6px;">&#8250;</button>
      ${active?`<button class="wbtn ${ch.done?"ok":""}" data-bm="${i}" ${meas?"disabled":""}
        style="width:90px;padding:3px 4px;font-size:9px;">${meas?("● "+st.meas_elapsed+"/12")
          :(ch.done?("✓ "+ch.rms_db.toFixed(1)):"MESURER")}</button>
      <button class="wbtn" data-so="${i}" title="Solo (monte la voie à la place de la voix)"
        style="width:26px;padding:3px 2px;font-size:9px;font-weight:700;
        ${st.solo===i?"background:#2a2214;border-color:#e5a13c;color:#e5a13c;":""}">S</button>`
        :`<span style="width:122px;"></span>`}
      ${active?`
      <!-- VU (niveau) — prend la place restante -->
      <div style="flex:1;min-width:40px;display:flex;align-items:center;gap:5px;">
        <span style="font-size:8px;letter-spacing:.15em;color:var(--mut);width:16px;">VU</span>
        <div style="flex:1;height:12px;border-radius:6px;background:#0b0e11;overflow:hidden;">
          <div data-vu="${i}" style="height:100%;width:0;background:#3d8a54;border-radius:6px;transition:width .09s;"></div></div>
      </div>
      <!-- KEEPER (volume auto) — CÔTE À CÔTE avec le VU -->
      <div style="width:132px;display:flex;align-items:center;gap:5px;">
        <span style="font-size:8px;color:var(--mut);width:26px;">VOL</span>
        <div style="flex:1;height:12px;border-radius:6px;background:#0b0e11;position:relative;overflow:hidden;">
          <div style="position:absolute;left:calc(50% - 1px);top:0;width:2px;height:100%;background:#39434b;"></div>
          <div style="position:absolute;top:0;height:100%;border-radius:6px;transition:all .3s ease;
            ${(()=>{const k=Math.max(-24,Math.min(24,kdb));const half=50;
              return k>=0?`left:50%;width:${k/24*half}%;background:#4cc470;`
                        :`left:${50+k/24*half}%;width:${-k/24*half}%;background:#e8b84b;`;})()}"></div></div>
        <span style="font-size:9px;font-weight:700;width:38px;text-align:right;
          color:${kdb>=0?"#4cc470":"#e8b84b"};">${(kdb>=0?"+":"")+kdb.toFixed(1)}</span>
      </div>`:`<span style="flex:1"></span>`}
    </div>`;}).join("");
  c.querySelectorAll("[data-br]").forEach(bt=>bt.addEventListener("click",()=>{
    const i=+bt.dataset.br,cur=BMXROLES.indexOf(bmxSt.chans[i].role);
    const nx=(cur+(+bt.dataset.d)+BMXROLES.length)%BMXROLES.length;
    cmd({op:"bandmix_role",src:i,role:BMXROLES[nx]}).then(bmxPoll);}));
  c.querySelectorAll("[data-bm]").forEach(bt=>bt.addEventListener("click",()=>
    cmd({op:"bandmix_measure",src:+bt.dataset.bm}).then(bmxPoll)));
  c.querySelectorAll("[data-so]").forEach(bt=>bt.addEventListener("click",()=>{
    const i=+bt.dataset.so;
    cmd({op:"bandmix_solo",src:bmxSt&&bmxSt.solo===i?-1:i}).then(bmxPoll);}));
  const sa=document.getElementById("bmxsoloauto");
  sa.textContent=st.solo_auto?(st.solo_is_auto?"● SOLO AUTO":"SOLO AUTO ON"):"SOLO AUTO";
  sa.style.color=st.solo_auto?(st.solo_is_auto?"#e5a13c":"#4cc470"):"";
}
function bmxVfRender(){
  const v=bmxVf;if(!v)return;
  const b=document.getElementById("bmxvf");
  b.textContent=v.on?"PLACE À LA VOIX ON":"PLACE À LA VOIX OFF";
  b.style.color=v.on?"#b3a5f0":"";
  if(!bmxVfDrag)document.getElementById("bmxvfam").value=v.amount;
  document.getElementById("bmxvfact").textContent=v.active?"♪ voix détectée":"";
  document.getElementById("bmxvfcuts").innerHTML=v.cuts_db.map(cd=>
    `<div style="width:14px;border-radius:2px;background:#e8b84b;
      height:${Math.max(2,Math.min(26,cd/Math.max(1,v.max_cut_db)*26))}px;"></div>`).join("");
}
let bmxTapSet=false;
function bmxFft(spec){
  const cv=document.getElementById("bmxfft");if(!cv)return;
  const ctx=cv.getContext("2d"),W=cv.width,H=cv.height;
  ctx.clearRect(0,0,W,H);
  ctx.strokeStyle="#1c232a";
  [0.25,0.5,0.75].forEach(g=>{const y=H*g;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();});
  if(!Array.isArray(spec))return;
  const N=spec.length;
  ctx.fillStyle="#3a7fb0";
  for(let x=0;x<W;x++){
    const v=spec[Math.min(N-1,Math.round(x/W*(N-1)))];
    const h=Math.max(0,Math.min(1,(v+90)/90))*H;
    ctx.fillRect(x,H-h,1,h);
  }
}
function bmxPoll(){
  if(window.CURPAGE!=="BANDMIX")return;
  if(!bmxTapSet){bmxTapSet=true;cmd({op:"set_tap",tap:3,kind:3,a:0,b:1});}
  cmd({op:"bandmix_status"}).then(r=>{if(r.ok){bmxSt=r;bmxRender();}});
  cmd({op:"get_meters"}).then(r=>{if(!r||!r.in)return;
    document.querySelectorAll("#bmxchans [data-vu]").forEach(v=>{
      const lv=r.in[+v.dataset.vu]||0;
      const db=lv>0?20*Math.log10(lv/2147483647):-60;
      v.style.width=Math.max(0,Math.min(100,(db+60)/60*100))+"%";});
    if(Array.isArray(r.analyzer)&&r.analyzer[3]&&Array.isArray(r.analyzer[3].s))
      bmxFft(r.analyzer[3].s);});
  cmd({op:"get_vfocus"}).then(r=>{if(r.ok){bmxVf=r;bmxVfRender();}});
  cmd({op:"get_automix"}).then(r=>{if(r.ok){bmxDugan=r.on===1;}});
  cmd({op:"master_eq"}).then(r=>{if(r.ok){meqSt=r;meqRender();}});
  cmd({op:"automix_tune"}).then(r=>{if(r.ok){atSt=r;atRender();}});
  cmd({op:"set_balance"}).then(r=>{if(r.ok){balSt=r;balRender();}});
  cmd({op:"set_vspatial"}).then(r=>{if(r.ok){vspSt=r;vspRender();}});
}
setInterval(bmxPoll,500);

/* V13.9 web : BALANCE AUTO + SPATIAL VOIX (parité LCD) */
let balSt=null,balDrag=false,vspSt=null,vspDrag=false;
function balRender(){
  if(!balSt)return;
  const b=document.getElementById("balon");
  b.textContent=balSt.on?"● ON":"OFF";
  b.style.color=balSt.on?"#4cc470":"";
  if(!balDrag){
    document.getElementById("ballufs").value=balSt.lufs_tgt;
    document.getElementById("bale").value=balSt.e_tgt;
    document.getElementById("ballufsv").textContent=balSt.lufs_tgt.toFixed(1)+" LUFS";
    document.getElementById("balev").textContent="+"+balSt.e_tgt.toFixed(1)+" dB";
    if(balSt.c_tgt!=null){
      document.getElementById("balc").value=balSt.c_tgt;
      document.getElementById("balcv").textContent="+"+balSt.c_tgt.toFixed(1)+" dB";
    }
  }
  const sg=v=>(v>=0?"+":"")+v.toFixed(1);
  document.getElementById("balst").textContent=
    `voix ${sg(balSt.voice_db)} · chœurs ${sg(balSt.choir_db||0)} · musique ${sg(balSt.music_db)} · ${balSt.lufs.toFixed(1)} LUFS`;
}
function vspRender(){
  if(!vspSt)return;
  const b=document.getElementById("vspon");
  b.textContent=vspSt.on?"● ON":"OFF";
  b.style.color=vspSt.on?"#4cc470":"";
  if(!vspDrag){
    document.getElementById("vspam").value=vspSt.amount;
    document.getElementById("vspdl").value=vspSt.delay_ms;
    document.getElementById("vspamv").textContent=vspSt.amount.toFixed(0);
    document.getElementById("vspdlv").textContent=vspSt.delay_ms.toFixed(0)+" ms";
  }
}
document.getElementById("balon").addEventListener("click",()=>
  cmd({op:"set_balance",on:balSt&&balSt.on?0:1}).then(r=>{if(r.ok){balSt=r;balRender();}}));
document.getElementById("vspon").addEventListener("click",()=>
  cmd({op:"set_vspatial",on:vspSt&&vspSt.on?0:1}).then(r=>{if(r.ok){vspSt=r;vspRender();}}));
[["ballufs","lufs_tgt","ballufsv",v=>(+v).toFixed(1)+" LUFS","set_balance",()=>balDrag,d=>balDrag=d],
 ["bale","e_tgt","balev",v=>"+"+(+v).toFixed(1)+" dB","set_balance",()=>balDrag,d=>balDrag=d],
 ["balc","c_tgt","balcv",v=>"+"+(+v).toFixed(1)+" dB","set_balance",()=>balDrag,d=>balDrag=d],
 ["vspam","amount","vspamv",v=>(+v).toFixed(0),"set_vspatial",()=>vspDrag,d=>vspDrag=d],
 ["vspdl","delay_ms","vspdlv",v=>(+v).toFixed(0)+" ms","set_vspatial",()=>vspDrag,d=>vspDrag=d]]
.forEach(([id,key,lbl,fmt,op,,setDrag])=>{
  const el=document.getElementById(id);
  el.addEventListener("pointerdown",()=>setDrag(true));
  el.addEventListener("pointerup",()=>setDrag(false));
  el.addEventListener("input",()=>{document.getElementById(lbl).textContent=fmt(el.value);});
  el.addEventListener("change",()=>{setDrag(false);cmd({op:op,[key]:+el.value});});
});

/* V13.9 web : réglages automix live (gel silence / mémoire crête / marge) */
let atSt=null, atDrag=false;
function atRender(){
  if(!atSt||atDrag)return;
  const g=document.getElementById("atgel"),ri=document.getElementById("atrisk"),m=document.getElementById("atmarg");
  if(atSt.freeze_db!=null){g.value=atSt.freeze_db;document.getElementById("atgelv").textContent="−"+(+atSt.freeze_db).toFixed(0)+" dB";}
  if(atSt.risk_decay!=null){ri.value=atSt.risk_decay;document.getElementById("atriskv").textContent=(+atSt.risk_decay).toFixed(2)+" dB/s";}
  if(atSt.risk_margin!=null){m.value=atSt.risk_margin;document.getElementById("atmargv").textContent="+"+(+atSt.risk_margin).toFixed(1)+" dB";}
  const g2=document.getElementById("atgate");
  if(atSt.gate_db!=null){g2.value=atSt.gate_db;document.getElementById("atgatev").textContent="−"+(+atSt.gate_db).toFixed(0)+" dB";}
}
[["atgel","freeze_db","atgelv",v=>"−"+(+v).toFixed(0)+" dB"],
 ["atrisk","risk_decay","atriskv",v=>(+v).toFixed(2)+" dB/s"],
 ["atmarg","risk_margin","atmargv",v=>"+"+(+v).toFixed(1)+" dB"],
 ["atgate","gate_db","atgatev",v=>"−"+(+v).toFixed(0)+" dB"]].forEach(([id,key,lbl,fmt])=>{
  const el=document.getElementById(id);
  el.addEventListener("pointerdown",()=>{atDrag=true;});
  el.addEventListener("pointerup",()=>{atDrag=false;});
  el.addEventListener("input",()=>{document.getElementById(lbl).textContent=fmt(el.value);});
  el.addEventListener("change",()=>{atDrag=false;cmd({op:"automix_tune",[key]:+el.value});});
});

/* V13.7 web : EQ master — courbe + makeup/LUFS live + sliders */
let meqSt=null, meqDrag=false;
function meqCurve(p){
  const cv=document.getElementById("meqcurve"); if(!cv)return;
  const ctx=cv.getContext("2d"), W=cv.width, H=cv.height, MID=H/2, SC=H/2/12;
  ctx.clearRect(0,0,W,H);
  ctx.strokeStyle="#242c34";
  [-6,0,6].forEach(g=>{const y=MID-g*SC;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(W,y);ctx.stroke();});
  const sh=(f,fc,g,hi)=>{const r=f/fc;return g*(hi? r/(1+r) : 1/(1+r));};
  const bell=(f,fc,g,q)=>{const bw=Math.max(1,fc/q),d=f-fc;return g/(1+Math.pow(d/(bw/2),2));};
  const f0=30,f1=20000;
  ctx.strokeStyle="#4cc470";ctx.lineWidth=1.6;ctx.beginPath();
  for(let x=0;x<W;x++){
    const f=f0*Math.pow(f1/f0,x/W);
    const db=sh(f,p.low_hz,p.low_db,false)+bell(f,p.mid_hz,p.mid_db,p.mid_q)+sh(f,p.air_hz,p.air_db,true);
    const y=Math.max(1,Math.min(H-1,MID-db*SC));
    x?ctx.lineTo(x,y):ctx.moveTo(x,y);
  }
  ctx.stroke();
}
function meqRender(){
  if(!meqSt)return;
  const s=meqSt, sg=v=>(v>0?"+":"")+v;
  document.getElementById("meqval").textContent=
    `sub ${sg(s.low_db)}@${s.low_hz|0} · méd ${sg(s.mid_db)}@${s.mid_hz|0} · air ${sg(s.air_db)}@${(s.air_hz/1000).toFixed(1)}k`;
  document.getElementById("meqmk").textContent=`${sg(+s.makeup_db.toFixed(1))} dB`;
  document.getElementById("meqlufs").textContent=`LUFS ${s.lufs.toFixed(1)}`;
  if(!meqDrag){
    document.getElementById("meqlow").value=s.low_db;
    document.getElementById("meqmid").value=s.mid_db;
    document.getElementById("meqair").value=s.air_db;
  }
  meqCurve(s);
}
["meqlow","meqmid","meqair"].forEach((id,k)=>{
  const el=document.getElementById(id), key=["low_db","mid_db","air_db"][k];
  el.addEventListener("pointerdown",()=>{meqDrag=true;});
  el.addEventListener("pointerup",()=>{meqDrag=false;});
  el.addEventListener("input",()=>{
    if(meqSt){meqSt[key]=+el.value;meqCurve(meqSt);
      document.getElementById("meqval").textContent="… réglage …";}
    cmd({op:"master_eq",[key]:+el.value});
  });
});
document.getElementById("bmxcalc").addEventListener("click",()=>cmd({op:"bandmix_calc"}).then(bmxPoll));
document.getElementById("bmxlock").addEventListener("click",()=>cmd({op:"bandmix_lock"}).then(bmxPoll));
document.getElementById("bmxauto").addEventListener("click",()=>
  cmd({op:"bandmix_autolive",on:bmxSt&&bmxSt.autolive?0:1}).then(bmxPoll));
document.getElementById("bmxlive").addEventListener("click",()=>
  cmd({op:"bandmix_live",on:bmxSt&&bmxSt.live?0:1}).then(bmxPoll));
document.getElementById("bmxdugan").addEventListener("click",()=>
  cmd({op:"set_automix_cfg",on:bmxDugan?0:1}).then(bmxPoll));
document.getElementById("bmxsoloauto").addEventListener("click",()=>
  cmd({op:"bandmix_solo",auto:bmxSt&&bmxSt.solo_auto?0:1}).then(bmxPoll));
document.getElementById("bmxvf").addEventListener("click",()=>
  cmd({op:"set_vfocus",on:bmxVf&&bmxVf.on?0:1}).then(bmxPoll));
const bmxAm=document.getElementById("bmxvfam");
bmxAm.addEventListener("pointerdown",()=>{bmxVfDrag=true;});
bmxAm.addEventListener("change",()=>{bmxVfDrag=false;
  cmd({op:"set_vfocus",amount:+bmxAm.value});});

/* ---------------- SCÈNE : automatismes + profils ---------------- */
let scnSt={al:0,alN:0,alF:"",dugan:0,live:0,corr:0,mast:0,chain:0,vf:0,vfa:0,vft:0};
function scnBtn(id,title,on,accent,state,info,vu,cb){
  /* V13-E2b : barre VU SIGNAL (id scnsig_*, maj par le poll meters 250 ms
   * sans reconstruire le bouton) + barre d'activité (couleur accent) */
  return `<div data-sb="${id}" style="height:120px;border-radius:10px;cursor:pointer;position:relative;
    display:flex;flex-direction:column;align-items:center;justify-content:flex-start;gap:4px;padding-top:12px;
    background:${on?"#1d1a10":"#14181c"};border:${on?"2px solid "+accent:"1px solid #39434b"};">
    <div style="font-size:11px;font-weight:700;letter-spacing:2px;color:${on?accent:"#8b959d"};">${title}</div>
    <div style="font-size:16px;font-weight:700;color:${on?"#e9e5da":"#5c666e"};">${state}</div>
    <div style="font-size:9px;color:var(--mut);font-family:monospace;">${info}</div>
    <div style="position:absolute;bottom:14px;left:12px;right:12px;height:8px;border-radius:4px;background:#0b0e11;overflow:hidden;">
      <div id="scnsig_${id}" style="height:100%;width:0;background:#4cc470;"></div></div>
    <div style="position:absolute;bottom:6px;left:12px;right:12px;height:4px;border-radius:2px;background:#0b0e11;overflow:hidden;">
      <div style="height:100%;width:${Math.round(Math.max(0,Math.min(1,vu))*100)}%;background:${accent};"></div></div>
  </div>`;
}
/* peak s32 → largeur % (-48..0 dB) + couleur */
function scnSig(el,peak){
  if(!el)return;
  const f=peak/2147483647;let w=0;
  if(f>0){const db=20*Math.log10(f);w=Math.max(0,Math.min(100,(db+48)/48*100));}
  el.style.width=w+"%";
  el.style.background=w>94?"#e05545":w>80?"#e8b84b":"#4cc470";
}
let scnRoles=[];
function scnMeters(){
  if(window.CURPAGE!=="SCENE")return;
  fetch("/api/meters").then(r=>r.json()).then(j=>{
    if(!j.ok)return;
    const mx=(a,b)=>{let m=0;for(let i=a;i<b&&i<j.in.length;i++)if(j.in[i]>m)m=j.in[i];return m;};
    scnSig(document.getElementById("scnsig_al"),mx(0,8));
    scnSig(document.getElementById("scnsig_amx"),mx(0,16));
    scnSig(document.getElementById("scnsig_mast"),Math.max(j.out[0]||0,j.out[1]||0));
    let v=0;scnRoles.forEach((r,i)=>{if((r==="lead"||r==="choir")&&j.in[i]>v)v=j.in[i];});
    scnSig(document.getElementById("scnsig_vf"),v);
    scnSig(document.getElementById("scnvul"),j.out[0]||0);
    scnSig(document.getElementById("scnvur"),j.out[1]||0);
  }).catch(()=>{});
}
setInterval(scnMeters,250);
function scnRender(){
  const s=scnSt,b=document.getElementById("scnbtns");
  if(!b)return;
  b.innerHTML=
    scnBtn("al","ANTI-LARSEN",s.al===1,"#e05545",s.al?"ON":"OFF",
      s.alN>0?s.alN+" notch · "+s.alF:(s.al?"veille — rien":"désactivé"),s.alN/6)
   +scnBtn("amx","AUTOMIX",s.live||s.dugan,s.dugan?"#e5a13c":"#4cc470",
      s.live?"MUSIQUE":(s.dugan?"VOIX":"OFF"),
      (s.live||s.dugan)?("corr "+(s.corr>=0?"+":"")+s.corr.toFixed(1)+" dB"):"tap : musique/voix",
      Math.abs(s.corr)/6)
   +scnBtn("mast","MASTERING",s.mast===1,"#5aa9e6",s.mast?"ON":"OFF",
      s.chain?"chaîne prête":"aucune chaîne",s.mast?0.4:0)
   +scnBtn("vf","VOIX DEVANT",s.vf===1,"#b3a5f0",
      s.vf?(s.vfa?"♪ ACTIF":"ON"):"OFF",
      s.vf?("creuse "+s.vft.toFixed(1)+" dB"):"place à la voix",s.vft/15);
  b.querySelector('[data-sb="al"]').addEventListener("click",()=>{
    fetch("/api/larsen",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({enable:s.al?0:1})}).then(()=>scnPoll());});
  b.querySelector('[data-sb="amx"]').addEventListener("click",()=>{
    if(!s.live&&!s.dugan){cmd({op:"bandmix_live",on:1});cmd({op:"set_automix_cfg",on:0});}
    else if(s.live){cmd({op:"bandmix_live",on:0});cmd({op:"set_automix_cfg",on:1});}
    else cmd({op:"set_automix_cfg",on:0});
    setTimeout(scnPoll,300);});
  b.querySelector('[data-sb="mast"]').addEventListener("click",()=>{
    cmd({op:"set_insert_bypass",on:s.mast?0:1}).then(scnPoll);});
  b.querySelector('[data-sb="vf"]').addEventListener("click",()=>{
    cmd({op:"set_vfocus",on:s.vf?0:1}).then(scnPoll);});
}
function scnSlots(list){
  const c=document.getElementById("scnslots");
  if(!c)return;
  c.innerHTML=list.map(sc=>`<div style="display:flex;align-items:center;gap:12px;
      border-radius:8px;padding:8px 10px;background:${sc.used?"#171c21":"#14181c"};
      border:1px solid ${sc.used?"#39434b":"#22282e"};">
    <div style="width:36px;height:36px;border-radius:18px;display:flex;align-items:center;justify-content:center;
      background:${sc.used?"#e5a13c":"#1b2126"};color:${sc.used?"#0b0e11":"#5c666e"};font-weight:700;">${sc.slot+1}</div>
    <span style="flex:1;font-weight:700;color:${sc.used?"#e9e5da":"#3a434b"};">${esc(sc.name)}</span>
    <button class="wbtn" data-sr="${sc.slot}" ${sc.used?"":"disabled"}>RAPPEL</button>
    <button class="wbtn ok" data-ss="${sc.slot}">SAUVER</button>
  </div>`).join("");
  /* V13.2 : RAPPEL = appui long avec anneau (parité LCD) ; SAUVER sur
   * slot OCCUPÉ = appui long (écrasement), slot vide = clic simple ;
   * endpoints /api/scene/* = scène COMPLÈTE (mixer + TAC/DSP + synthé) */
  c.querySelectorAll("[data-sr]").forEach(bt=>holdify(bt,()=>{
    fetch("/api/scene/recall",{method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({slot:+bt.dataset.sr})})
      .then(()=>{scnPoll();scnList();});
  },{color:"#e5a13c"}));
  const doSave=bt=>{
    const cur=(list.find(s=>s.slot===+bt.dataset.ss)||{}).name||"";
    const nm=prompt("Nom de la scène "+(+bt.dataset.ss+1)+" :",cur);
    if(nm===null)return;
    fetch("/api/scene/save",{method:"POST",
      headers:{"Content-Type":"application/json"},
      body:JSON.stringify({slot:+bt.dataset.ss,
                           name:nm.trim()||("Scène "+(+bt.dataset.ss+1))})})
      .then(scnList);
  };
  c.querySelectorAll("[data-ss]").forEach(bt=>{
    const used=(list.find(s=>s.slot===+bt.dataset.ss)||{}).used===1;
    if(used)holdify(bt,()=>doSave(bt),{color:"#4cc470"});
    else bt.addEventListener("click",()=>doSave(bt));
  });
}
function scnList(){cmd({op:"scene_list"}).then(r=>{if(r.ok)scnSlots(r.scenes);});}
function scnPoll(){
  if(window.CURPAGE!=="SCENE")return;
  cmd({op:"get_automix"}).then(r=>{if(!r.ok)return;
    scnSt.dugan=r.on;let m=0;
    r.gains_db.forEach((g,i)=>{if(r.members[i]===1&&g<m)m=g;});
    if(r.on)scnSt.corr=m;scnRender();});
  cmd({op:"bandmix_status"}).then(r=>{if(!r.ok)return;
    scnSt.live=r.live;
    scnRoles=r.chans.map(c=>c.role);   /* V13-E2b : VU voix */
    if(r.live){let m=0;r.chans.forEach(c=>{if(Math.abs(c.keeper_db)>Math.abs(m))m=c.keeper_db;});scnSt.corr=m;}
    scnRender();});
  cmd({op:"get_insert_bypass"}).then(r=>{if(r.ok){scnSt.mast=r.mastering_on;scnSt.chain=r.chain;scnRender();}});
  cmd({op:"get_vfocus"}).then(r=>{if(r.ok){scnSt.vf=r.on;scnSt.vfa=r.active;
    scnSt.vft=r.cuts_db.reduce((a,b)=>a+b,0);scnRender();}});
  fetch("/api/larsen").then(r=>r.json()).then(r=>{if(!r.ok)return;
    scnSt.al=r.enable;scnSt.alN=r.notches.length;
    scnSt.alF=r.notches.length?r.notches[r.notches.length-1].freq+" Hz":"";
    scnRender();}).catch(()=>{});
}
setInterval(scnPoll,600);
setInterval(()=>{if(window.CURPAGE==="SCENE")scnList();},3000);
scnRender();scnList();

/* ---------------- ANTI-LARSEN (page SYSTÈME) ---------------- */
function alPoll(){
  if(window.CURPAGE&&window.CURPAGE!=="SYSTEME")return;
  fetch("/api/larsen").then(r=>r.json()).then(r=>{
    const en=document.getElementById("al-en"),nl=document.getElementById("al-notches");
    if(!en||!nl)return;
    if(!r.ok){en.textContent="(daemon absent)";nl.textContent="—";return;}
    en.textContent=r.enable?"actif":"désactivé (enable=0)";
    nl.innerHTML=r.notches.length===0?"aucun notch posé — pas de larsen détecté"
      :r.notches.map(n=>`CH${n.ch+1} · BQ${n.bq} · <b style="color:#e05545">${n.freq} Hz</b> · ${n.depth} dB · depuis ${n.age<120?n.age+" s":Math.round(n.age/60)+" min"}`).join("<br>");
  }).catch(()=>{});
}
setInterval(alPoll,1000);alPoll();

/* --- éditeur de patch M1 (web) --- */
const XPAR=[["OSC — DETUNE (cts)","detune",-50,50],["OSC — BALANCE 1↔2","balance",0,99],
["VDF CUTOFF","cutoff",0,99],["VDF EG INT","eg_int",0,99],
["VDF ATTACK","vdf0",0,99],["VDF DECAY","vdf1",0,99],["VDF BREAK","vdf2",0,99],
["VDF SUSTAIN","vdf3",0,99],["VDF SLOPE","vdf4",0,99],["VDF RELEASE","vdf5",0,99],
["VDA ATTACK","vda0",0,99],["VDA DECAY","vda1",0,99],["VDA BREAK","vda2",0,99],
["VDA SUSTAIN","vda3",0,99],["VDA SLOPE","vda4",0,99],["VDA RELEASE","vda5",0,99],
["LFO RATE","lfo_rate",0,99],["LFO DEPTH","lfo_depth",0,99],["LFO DELAY","lfo_delay",0,99],
["VEL SENS","vel_sens",0,99],["LEVEL","level",0,99]];
const pval=(k)=>k.startsWith("vdf")&&k.length===4?pd.vdf[+k[3]]
             :k.startsWith("vda")&&k.length===4?pd.vda[+k[3]]:pd[k];
function xpdEdit(pi){
  const go=()=>cmd({op:"midix_ctl",line:"patch_get "+pi}).then(r=>{
    if(!r.ok)return; pd=r; editIdx=pi;
    document.getElementById("xpename").textContent="PATCH M1 · "+pd.name+" (n° "+pi+")";
    const b=document.getElementById("xpebody");
    const oscRow=(k,lbl)=>{
      const v=pd[k], nm=v<0?"OFF":(instNames[v]||("#"+v));
      return `<div style="display:flex;align-items:center;gap:8px;">
        <span style="width:150px;font-size:10px;font-weight:700;color:var(--mut);">${lbl}</span>
        <button class="wbtn" data-o="${k}" data-d="-1">&#8249;</button>
        <span style="width:250px;text-align:center;font-weight:700;color:#e9e5da;
          border:1px solid #5a4a7a;border-radius:5px;padding:6px 0;font-size:12px;
          overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${esc(nm)}</span>
        <button class="wbtn" data-o="${k}" data-d="1">&#8250;</button></div>`;};
    b.innerHTML=oscRow("osc1","OSC 1")+oscRow("osc2","OSC 2 (−1 = off)")
      +XPAR.map(([lbl,k,mn,mx])=>`<div style="display:flex;align-items:center;gap:8px;">
        <span style="width:150px;font-size:10px;font-weight:700;color:var(--mut);">${lbl}</span>
        <input type="range" data-k="${k}" min="${mn}" max="${mx}" value="${pval(k)}" style="flex:1;">
        <span style="width:40px;font-family:monospace;font-weight:700;" id="xv_${k}">${pval(k)}</span>
      </div>`).join("");
    b.querySelectorAll("[data-k]").forEach(sl=>sl.addEventListener("input",()=>{
      const k=sl.dataset.k, v=+sl.value;
      document.getElementById("xv_"+k).textContent=v;
      cmd({op:"midix_ctl",line:"patch_set "+editIdx+" "+k+" "+v});
    }));
    b.querySelectorAll("[data-o]").forEach(bt=>bt.addEventListener("click",()=>{
      const k=bt.dataset.o; let v=pd[k]+(+bt.dataset.d);
      const lo=k==="osc2"?-1:0;
      if(v<lo)v=instNames.length-1; if(v>=instNames.length)v=lo;
      cmd({op:"midix_ctl",line:"patch_set "+editIdx+" "+k+" "+v}).then(()=>xpdEdit(editIdx));
    }));
    document.getElementById("xpdedit").style.display="flex";
  });
  if(instNames.length===0)
    cmd({op:"midix_ctl",line:"inst_list"}).then(r=>{if(r.ok)instNames=r.inst;go();});
  else go();
}
document.getElementById("xpeclose").addEventListener("click",()=>{
  document.getElementById("xpdedit").style.display="none";editIdx=-1;});
document.getElementById("xpesave").addEventListener("click",()=>{
  if(editIdx>=0)cmd({op:"midix_ctl",line:"patch_save "+editIdx})
    .then(()=>{patchNames=[];xpdPoll();});});
})();

