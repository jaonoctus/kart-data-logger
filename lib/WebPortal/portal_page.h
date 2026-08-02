#ifndef PORTAL_PAGE_H
#define PORTAL_PAGE_H

#include <Arduino.h>

/* The whole UI, served from flash. Deliberately dependency-free: a browser
 * joined to the dash's AP has no route to the internet, so every byte a CDN
 * would normally supply has to be here. Charts are hand-drawn on canvas.
 *
 * Lap detection below mirrors lib/LapManager/LapManager.cpp exactly — 10 m
 * gate proximity, segment intersection with a direction check, 10 s cooldown,
 * and interpolation of the crossing instant. Keep the two in step: the log
 * carries no lap markers, so this is a reimplementation, not a readout. */

static const char PORTAL_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kart Dash</title><style>
:root{--bg:#050608;--sf:#0d1014;--sf2:#14181e;--fg:#f6f8fb;--fg2:#cbd0d8;--mut:#6b7280;--ru:#1c2026;--ac:#ffd400;--gd:#2ee07a;--bd:#ff3b3b}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
header{padding:14px 18px;border-bottom:1px solid var(--ru);display:flex;align-items:center;gap:14px;flex-wrap:wrap}
h1{font-size:16px;margin:0;letter-spacing:.5px}h2{font-size:13px;margin:0 0 10px;color:var(--fg2);text-transform:uppercase;letter-spacing:1px}
.mut{color:var(--mut)}.wrap{padding:18px;max-width:1100px;margin:0 auto}
.grid{display:grid;grid-template-columns:280px minmax(0,1fr);gap:18px}@media(max-width:820px){.grid{grid-template-columns:minmax(0,1fr)}}
/* Grid items default to min-width:auto, so a canvas whose *intrinsic* width is
   the backing store (clientWidth x devicePixelRatio) sets the column minimum.
   The column then grows, clientWidth grows, the next resize grows the backing
   store again - the canvas runs away and overflows. minmax(0,1fr) plus these
   two lines pin it to the available width instead. */
.grid>*{min-width:0}
.card{background:var(--sf);border:1px solid var(--ru);border-radius:8px;padding:14px;margin-bottom:18px}
.f{display:flex;justify-content:space-between;align-items:center;padding:7px 8px;border-radius:5px;cursor:pointer;gap:8px}
.f:hover{background:var(--sf2)}.f.sel{background:var(--sf2);outline:1px solid var(--ac)}
.f .n{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.f .s{color:var(--mut);font-size:12px;flex:none}
a,button{color:var(--fg);background:var(--sf2);border:1px solid var(--ru);border-radius:5px;padding:5px 9px;font-size:12px;cursor:pointer;text-decoration:none;font-family:inherit}
button:hover,a:hover{border-color:var(--ac)}button:disabled{opacity:.4;cursor:default}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}
.st{background:var(--sf2);border-radius:6px;padding:10px 12px}.st .v{font-size:20px;font-weight:600}.st .k{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}
th{text-align:left;font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.6px;padding:6px 8px;border-bottom:1px solid var(--ru)}
td{padding:6px 8px;border-bottom:1px solid var(--ru)}tr:last-child td{border-bottom:0}
.best{color:var(--gd);font-weight:600}
tr.bestrow td{background:rgba(46,224,122,.07)}.dn{color:var(--gd)}.up{color:var(--bd)}
canvas{width:100%;max-width:100%;display:block;border-radius:6px;background:var(--sf2)}
#msg{padding:9px 12px;border-radius:6px;background:var(--sf2);margin-bottom:14px;display:none}
#msg.err{color:var(--bd)}#drop{border:1px dashed var(--ru);border-radius:6px;padding:12px;text-align:center;color:var(--mut);font-size:12px;margin-top:10px}
#drop.on{border-color:var(--ac);color:var(--fg)}
</style></head><body>
<header><h1>KART DASH</h1><span class="mut" id="info">…</span></header>
<div class="wrap"><div id="msg"></div><div class="grid">
<div><div class="card"><h2>SD card</h2><div id="files" class="mut">loading…</div>
<div id="drop">drop a file here to upload</div></div></div>
<div><div id="empty" class="card mut">Select a <code>.csv</code> log to analyse.</div>
<div id="out" style="display:none">
<div class="card"><h2>Session</h2><div class="stats" id="stats"></div></div>
<div class="card"><h2>Laps</h2><div id="laps"></div></div>
<div class="card"><h2>Speed</h2><canvas id="spd" style="height:220px"></canvas></div>
<div class="card"><h2>Track <span class="mut" id="mapnote"></span></h2><canvas id="map" style="height:420px"></canvas></div>
</div></div></div></div>
<script>
const $=s=>document.querySelector(s);
let gate=null, s1=null, s2=null, sel=null, lastRows=null;
// Canvases are sized from clientWidth, so a window resize (or rotating a
// phone) has to redraw or the charts stay at the old geometry.
let rt=null;
addEventListener('resize',()=>{clearTimeout(rt);
  rt=setTimeout(()=>{if(lastRows)render(lastRows)},150)});
const msg=(t,e)=>{const m=$('#msg');m.textContent=t;m.className=e?'err':'';m.style.display=t?'block':'none'};
const fmt=ms=>{if(!isFinite(ms))return'—';const m=Math.floor(ms/60000),s=(ms%60000)/1000;return m?`${m}:${s.toFixed(3).padStart(6,'0')}`:s.toFixed(3)};
const kb=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(0)+' KB':(b/1048576).toFixed(1)+' MB';

// ---- device -------------------------------------------------------------
// /api/info is cheap (CSD register). Used/free space needs f_getfree(), which
// can rescan the whole FAT on a big card, so it is opt-in behind the link.
fetch('/api/info').then(r=>r.json()).then(d=>{
  $('#info').innerHTML=`SD ${d.card} MB · ${d.clients} client(s) · `;
  const a=document.createElement('a'); a.href='#'; a.textContent='free space';
  a.onclick=e=>{e.preventDefault(); a.textContent='checking…';
    fetch('/api/space').then(r=>r.json()).then(s=>{
      a.replaceWith(document.createTextNode(`${s.used} MB used of ${s.total} MB`))})
     .catch(()=>{a.textContent='unavailable'})};
  $('#info').append(a);
}).catch(()=>{$('#info').textContent='device info unavailable'});

// ---- file list ----------------------------------------------------------
function list(){
  fetch('/api/list?path=/').then(r=>r.json()).then(d=>{
    const el=$('#files'); el.innerHTML=''; el.className='';
    if(!d.length){el.innerHTML='<span class="mut">empty</span>';return}
    d.sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}));
    for(const f of d){
      const row=document.createElement('div'); row.className='f'+(f.name===sel?' sel':'');
      const n=document.createElement('span'); n.className='n'; n.textContent=f.name;
      const s=document.createElement('span'); s.className='s'; s.textContent=kb(f.size);
      row.append(n,s);
      row.onclick=e=>{ if(e.target.tagName==='A'||e.target.tagName==='BUTTON')return;
        if(f.name.toLowerCase().endsWith('.csv')){sel=f.name;list();load(f.name)}
        else msg('Only .csv logs can be analysed. Use Get to download.') };
      const a=document.createElement('a'); a.href='/api/file?path=/'+encodeURIComponent(f.name);
      a.download=f.name; a.textContent='Get'; a.onclick=e=>e.stopPropagation();
      const del=document.createElement('button'); del.textContent='×'; del.title='delete';
      del.onclick=e=>{e.stopPropagation(); if(!confirm('Delete '+f.name+'?'))return;
        fetch('/api/delete?path=/'+encodeURIComponent(f.name)).then(()=>{if(sel===f.name){sel=null;$('#out').style.display='none';$('#empty').style.display='block'}list()})};
      const box=document.createElement('span'); box.style.cssText='display:flex;gap:6px;flex:none';
      box.append(a,del); row.append(box); el.append(row);
    }
  }).catch(e=>{$('#files').textContent='failed to list'});
}
list();

// ---- upload -------------------------------------------------------------
const dz=$('#drop');
dz.ondragover=e=>{e.preventDefault();dz.classList.add('on')};
dz.ondragleave=()=>dz.classList.remove('on');
dz.ondrop=e=>{e.preventDefault();dz.classList.remove('on');
  const f=e.dataTransfer.files[0]; if(!f)return;
  const fd=new FormData(); fd.append('f',f,f.name); msg('uploading '+f.name+'…');
  fetch('/api/upload',{method:'POST',body:fd}).then(r=>{msg(r.ok?'uploaded '+f.name:'upload failed',!r.ok);list()})
    .catch(()=>msg('upload failed',1))};

// ---- CSV + analysis -----------------------------------------------------
// Mirrors LapManager: 10 m gate proximity, segment intersection with a
// direction check, 10 s cooldown, interpolated crossing instant.
function dist(la1,lo1,la2,lo2){const p=0.017453292519943295;
  const a=0.5-Math.cos((la2-la1)*p)/2+Math.cos(la1*p)*Math.cos(la2*p)*(1-Math.cos((lo2-lo1)*p))/2;
  return 12742000*Math.asin(Math.sqrt(Math.max(0,Math.min(1,a))))}
function cross(Ax,Ay,Bx,By,Cx,Cy,Dx,Dy){
  const s1x=Bx-Ax,s1y=By-Ay,s2x=Dx-Cx,s2y=Dy-Cy,den=s1x*s2y-s2x*s1y;
  if(den===0)return null; const dp=den>0,s3x=Ax-Cx,s3y=Ay-Cy;
  const sn=s1x*s3y-s1y*s3x; if((sn<0)===dp||(sn>den)===dp)return null;
  const tn=s2x*s3y-s2y*s3x; if((tn<0)===dp||(tn>den)===dp)return null;
  if(s1x*(Cy-Dy)+s1y*(Dx-Cx)<=0)return null;   // crossed backwards
  return sn/den}

// Proximity is measured to the gate SEGMENT, not its centre — same rule as
// LapManager. A wide gate's half-width alone can exceed a centre radius, and
// the finish line and S2 can be collinear and abutting, which a centre
// distance cannot tell apart.
function nearSeg(la,lo,g){
  const k=Math.cos(g.ll*Math.PI/180);
  const bx=(g.rn-g.ln)*111320*k, by=(g.rl-g.ll)*110540;
  const px=(lo-g.ln)*111320*k,   py=(la-g.ll)*110540;
  const l2=bx*bx+by*by;
  let t=l2>0?((px*bx+py*by)/l2):0; t=Math.max(0,Math.min(1,t));
  return Math.hypot(px-t*bx, py-t*by)}

function laps(rows){
  if(!gate)return{rows:[],why:'no tracks.ini on the card — cannot locate the finish line'};
  // Gate order matches LapManager: sector n is closed by gate n.
  const gates=[]; if(s1&&s2){gates.push(s1,s2)} gates.push(gate);
  const END=gates.length-1;

  let prev=null, start=0, best=Infinity, out=[];
  let secOpen=null, secIdx=-1, cur=[null,null,null];
  const bestSec=[Infinity,Infinity,Infinity];

  for(const r of rows){
    if(!r.fix){continue}
    if(!prev){prev=r;continue}
    let hit=-1, f=null;
    for(let g=0;g<gates.length;g++){
      const G=gates[g];
      if(nearSeg(r.lat,r.lng,G)>=12)continue;
      const x=cross(prev.lng,prev.lat,r.lng,r.lat,G.ln,G.ll,G.rn,G.rl);
      if(x!==null){hit=g;f=x;break}
    }
    const tA=prev.t; prev=r;
    if(hit<0)continue;
    const ct=tA+f*(r.t-tA);

    if(hit!==END){
      // A split: close the sector it terminates, open the next.
      if(secIdx===hit&&secOpen!==null)cur[hit]=ct-secOpen;
      secIdx=hit+1; secOpen=ct;
      continue;
    }
    if(r.t-start<=10000)continue;                       // lap cooldown
    if(secIdx===END&&secOpen!==null)cur[END]=ct-secOpen;

    if(start!==0){const lap=ct-start;
      // Delta is against the best lap as it stood *before* this one — the time
      // you were chasing. Matches LapManager::getPreviousBestLapTime().
      const prevBest=best;
      const isBest=lap<best; if(isBest)best=lap;
      const sec=cur.slice();
      sec.forEach((v,i)=>{if(v&&v<bestSec[i])bestSec[i]=v});
      out.push({n:out.length+1,t:lap,best:isBest,prevBest:prevBest,at:ct,sec})}
    cur=[null,null,null]; secIdx=0; secOpen=ct; start=ct;
  }
  return{rows:out,best,bestSec,hasSec:gates.length===3}}

function load(name){
  msg('loading '+name+'…');
  fetch('/api/file?path=/'+encodeURIComponent(name)).then(r=>r.text()).then(txt=>{
    const rows=[];
    for(const line of txt.split('\n')){
      if(!line||line[0]==='#'||line[0]==='e')continue;
      const c=line.split(',');
      if(c.length<9)continue;
      const r={t:+c[0],spd:+c[1],gx:+c[3],gy:+c[4],sats:+c[6],lat:+c[7],lng:+c[8]};
      r.fix=r.sats>=3&&isFinite(r.lat)&&isFinite(r.lng);
      if(isFinite(r.t))rows.push(r);
    }
    if(rows.length<2){msg('no usable rows in '+name,1);return}
    msg('');
    $('#empty').style.display='none'; $('#out').style.display='';
    lastRows=rows;
    render(rows);
  }).catch(()=>msg('failed to load '+name,1));
}

function render(rows){
  const t0=rows[0].t, span=(rows[rows.length-1].t-t0)/1000;
  let mx=0,sum=0,d=0,prev=null;
  for(const r of rows){ if(r.spd>mx)mx=r.spd; sum+=r.spd;
    if(prev&&r.fix&&prev.fix)d+=dist(prev.lat,prev.lng,r.lat,r.lng); if(r.fix)prev=r}
  const L=laps(rows);
  const st=[['Duration',span>60?Math.floor(span/60)+'m '+Math.round(span%60)+'s':span.toFixed(0)+'s'],
    ['Distance',(d/1000).toFixed(2)+' km'],['Max speed',mx.toFixed(1)+' km/h'],
    ['Avg speed',(sum/rows.length).toFixed(1)+' km/h'],['Samples',rows.length],
    ['Best lap',L.rows.length?fmt(L.best):'—']];
  $('#stats').innerHTML=st.map(([k,v])=>`<div class="st"><div class="v">${v}</div><div class="k">${k}</div></div>`).join('');

  if(L.why) $('#laps').innerHTML=`<span class="mut">${L.why}</span>`;
  else if(!L.rows.length) $('#laps').innerHTML='<span class="mut">no finish-line crossings — is this log from the selected track?</span>';
  else $('#laps').innerHTML='<table><tr><th>Lap</th><th>Time</th><th>vs best</th>'+
    (L.hasSec?'<th>S1</th><th>S2</th><th>S3</th>':'')+'</tr>'+
    L.rows.map(l=>{const dl=isFinite(l.prevBest)?l.t-l.prevBest:null;
      // Best lap greens the whole row; a best sector greens only that cell, so
      // "my quickest S2" reads differently from "my quickest lap".
      const secs=L.hasSec?l.sec.map((v,i)=>
        `<td class="${v&&v===L.bestSec[i]?'best':(v?'':'mut')}">${v?fmt(v):'—'}</td>`).join(''):'';
      return `<tr class="${l.best?'bestrow':''}"><td>${l.n}</td>
      <td class="${l.best?'best':''}">${fmt(l.t)}${l.best?' ★':''}</td>
      <td class="${dl===null?'mut':dl<0?'dn':'up'}">${dl===null?'—':(dl<0?'−':'+')+(Math.abs(dl)/1000).toFixed(3)}</td>
      ${secs}</tr>`}).join('')+'</table>';

  trace(rows,t0,L.rows);
  map(rows,mx);
}

// Measure the PARENT, never the canvas: reading the canvas' own width after it
// has been sized from a backing store is how the runaway growth started. CSS
// height is set explicitly so layout never derives it from the intrinsic
// aspect ratio.
function fit(cv,cssH){
  const r=devicePixelRatio||1;
  const w=Math.max(120,(cv.parentElement?cv.parentElement.clientWidth:0)||600);
  cv.style.height=cssH+'px';
  cv.width=Math.round(w*r); cv.height=Math.round(cssH*r);
  const x=cv.getContext('2d');
  x.setTransform(r,0,0,r,0,0);          // absolute, so repeat calls don't stack
  return[x,w,cssH]}

function trace(rows,t0,lps){
  const [c,W,H]=fit($('#spd'),220), pad=28, span=rows[rows.length-1].t-t0;
  let mx=0; for(const r of rows)if(r.spd>mx)mx=r.spd; mx=Math.max(10,mx*1.05);
  c.clearRect(0,0,W,H);
  c.strokeStyle='#1c2026'; c.fillStyle='#6b7280'; c.font='10px sans-serif'; c.lineWidth=1;
  for(let i=0;i<=4;i++){const y=pad+(H-pad*1.4)*i/4, v=mx*(1-i/4);
    c.beginPath();c.moveTo(pad,y);c.lineTo(W,y);c.stroke(); c.fillText(v.toFixed(0),2,y+3)}
  // lap boundaries
  c.strokeStyle='#3a3f47'; c.setLineDash([3,3]);
  for(const l of lps){const x=pad+(W-pad)*((l.at-t0)/span);
    c.beginPath();c.moveTo(x,pad);c.lineTo(x,H-pad*0.4);c.stroke()}
  c.setLineDash([]);
  c.strokeStyle='#ffd400'; c.lineWidth=1.4; c.beginPath();
  rows.forEach((r,i)=>{const x=pad+(W-pad)*((r.t-t0)/span), y=pad+(H-pad*1.4)*(1-r.spd/mx);
    i?c.lineTo(x,y):c.moveTo(x,y)});
  c.stroke();
  c.fillStyle='#6b7280'; c.fillText('km/h',2,pad-10);
}

function map(rows,mx){
  let pts=rows.filter(r=>r.fix);
  const note=$('#mapnote');
  if(pts.length<2){note.textContent='— no GPS fix in this log'; return}

  // A single bad fix (GPS occasionally emits a valid-looking sample near 0,0)
  // would blow the bounding box up to continental scale and squash the whole
  // track into one pixel. Anchor on the median and drop anything absurd.
  const mid=a=>{const v=[...a].sort((x,y)=>x-y);return v[v.length>>1]};
  const mLat=mid(pts.map(p=>p.lat)), mLng=mid(pts.map(p=>p.lng));
  const kept=pts.filter(p=>dist(p.lat,p.lng,mLat,mLng)<20000);   // 20 km
  const dropped=pts.length-kept.length;
  if(kept.length>1)pts=kept;

  let la1=1e9,la2=-1e9,lo1=1e9,lo2=-1e9;
  for(const p of pts){la1=Math.min(la1,p.lat);la2=Math.max(la2,p.lat);lo1=Math.min(lo1,p.lng);lo2=Math.max(lo2,p.lng)}
  // keep aspect: longitude degrees shrink with latitude
  const kx=Math.cos((la1+la2)/2*Math.PI/180);
  const spanX=Math.max((lo2-lo1)*kx,1e-9), spanY=Math.max(la2-la1,1e-9);

  // Size the canvas to the track's own proportions instead of a fixed 420 px:
  // a wide circuit in a tall box wasted most of the area and shrank the trace
  // to a ribbon. Clamped so a very long or very square track stays usable.
  const cv=$('#map'), pad=14;
  const W0=Math.max(120,(cv.parentElement?cv.parentElement.clientWidth:0)||600);
  const wantH=Math.round((W0-pad*2)*(spanY/spanX))+pad*2;
  const [c,W,H]=fit(cv, Math.max(260, Math.min(wantH, Math.round(innerHeight*0.8))));
  const mDrop=dropped?` — ${dropped} outlier fix(es) excluded`:'';
  note.textContent='— coloured by speed'+mDrop+`  (${(spanX*111320*Math.cos(mLat*Math.PI/180)/1).toFixed(0)}×${(spanY*110540).toFixed(0)} m)`;

  const s=Math.min((W-pad*2)/spanX,(H-pad*2)/spanY);
  const ox=(W-spanX*s)/2, oy=(H-spanY*s)/2;
  const X=p=>ox+(p.lng-lo1)*kx*s, Y=p=>H-(oy+(p.lat-la1)*s);
  c.clearRect(0,0,W,H); c.lineWidth=2.5; c.lineCap='round';
  for(let i=1;i<pts.length;i++){
    const a=pts[i-1],b=pts[i];
    if(dist(a.lat,a.lng,b.lat,b.lng)>50)continue;      // skip GPS jumps
    const f=Math.max(0,Math.min(1,b.spd/Math.max(mx,1)));
    c.strokeStyle=`hsl(${(1-f)*210},85%,${38+f*20}%)`;
    c.beginPath();c.moveTo(X(a),Y(a));c.lineTo(X(b),Y(b));c.stroke();
  }
  if(gate){const g1={lat:gate.ll,lng:gate.ln},g2={lat:gate.rl,lng:gate.rn};
    c.strokeStyle='#f6f8fb';c.lineWidth=2;c.setLineDash([4,3]);
    c.beginPath();c.moveTo(X(g1),Y(g1));c.lineTo(X(g2),Y(g2));c.stroke();c.setLineDash([])}
}

// ---- finish line from the device's own tracks.ini -----------------------
fetch('/api/file?path=/tracks.ini').then(r=>r.ok?r.text():null).then(t=>{
  if(!t)return;
  const sel=+(t.match(/^selected=(\d+)/m)?.[1]??0), g={};
  const get=k=>{const m=t.match(new RegExp('^'+sel+'_'+k+'=(-?[\\d.]+)','m'));return m?+m[1]:null};
  const gt=p=>{const o={};
    o.ll=get(p+'left_lat'); o.ln=get(p+'left_lon');
    o.rl=get(p+'right_lat'); o.rn=get(p+'right_lon');
    return [o.ll,o.ln,o.rl,o.rn].every(v=>v!==null)?o:null};
  gate=gt(''); s1=gt('s1_'); s2=gt('s2_');
  if(lastRows)render(lastRows);   // tracks.ini may land after the log
}).catch(()=>{});
</script></body></html>)HTML";

#endif // PORTAL_PAGE_H
