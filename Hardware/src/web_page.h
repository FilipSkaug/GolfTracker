#pragma once
#include <pgmspace.h>

// Single-page phone app served by the display during sync.
static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>GolfTracker</title>
<style>
:root{--bg:#f4f6f3;--card:#fff;--text:#14201a;--muted:#6b7a71;--line:#e3e8e4;--accent:#1f8a4c;
--eagle:#c99a06;--birdie:#0284c7;--bogey:#ea7a1d;--double:#e11d48;--danger:#c0262d}
@media (prefers-color-scheme:dark){:root{--bg:#0b100d;--card:#151d18;--text:#eef3ef;--muted:#8b998f;
--line:#26302a;--accent:#4ade80;--eagle:#facc15;--birdie:#38bdf8;--bogey:#fb923c;--double:#f43f5e;--danger:#f87171}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 -apple-system,system-ui,sans-serif;
padding:env(safe-area-inset-top) 16px calc(24px + env(safe-area-inset-bottom))}
a{color:var(--accent)}
h1{font-size:28px;margin:20px 0 12px}
.tabs{display:flex;gap:6px;background:var(--line);padding:4px;border-radius:12px;margin-bottom:16px}
.tabs button{flex:1;border:0;background:none;padding:9px;border-radius:9px;font:inherit;font-weight:600;color:var(--muted)}
.tabs button.on{background:var(--card);color:var(--text);box-shadow:0 1px 3px #0002}
.card{background:var(--card);border-radius:16px;padding:16px;margin-bottom:12px;border:1px solid var(--line)}
.row{display:flex;justify-content:space-between;align-items:baseline;gap:12px}
.title{font-weight:700;font-size:18px}
.muted{color:var(--muted);font-size:14px}
.err{color:var(--danger);font-size:14px;margin-top:12px}
.score{font-size:28px;font-weight:800}
table{width:100%;border-collapse:collapse;margin-top:12px;font-variant-numeric:tabular-nums;font-size:14px}
th,td{text-align:center;padding:5px 0;border-bottom:1px solid var(--line)}
th{color:var(--muted);font-weight:500}
td.lbl,th.lbl{text-align:left;color:var(--muted);font-weight:500;width:44px}
td.sum{font-weight:700}
.s{display:inline-block;min-width:24px;line-height:24px;border-radius:12px;font-weight:700}
.s.e{background:var(--eagle);color:#000}.s.b{border:2px solid var(--birdie);line-height:20px}
.s.o{border:2px solid var(--bogey);border-radius:4px;line-height:20px}.s.d{background:var(--double);color:#fff;border-radius:4px}
.btns{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap}
button.b{border:0;border-radius:10px;padding:10px 14px;font:inherit;font-weight:600;background:var(--line);color:var(--text)}
button.p{background:var(--accent);color:#fff}
@media (prefers-color-scheme:dark){button.p{color:#06240f}}
button.x{background:none;color:var(--danger);padding:10px 4px}
button.item{display:block;width:100%;text-align:left;border:0;background:var(--bg);border-radius:10px;padding:10px 12px;margin-top:8px;font:inherit;color:var(--text)}
.search{display:flex;gap:8px;margin-top:12px}.search input{flex:1;min-width:0}
label{display:block;font-size:14px;color:var(--muted);margin:12px 0 4px}
input,select{font:inherit;color:var(--text);background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:10px;width:100%}
.pars{display:grid;grid-template-columns:repeat(auto-fill,minmax(58px,1fr));gap:8px}
.pars div{font-size:12px;color:var(--muted);text-align:center}
.pars select{padding:8px 4px;text-align:center}
.seg{display:flex;gap:8px}.seg button{flex:1}
.empty{text-align:center;color:var(--muted);padding:32px 16px}
.toast{position:fixed;left:50%;bottom:calc(24px + env(safe-area-inset-bottom));transform:translateX(-50%);background:var(--text);color:var(--bg);
padding:10px 16px;border-radius:20px;font-weight:600;opacity:0;transition:opacity .2s;pointer-events:none;max-width:90vw;text-align:center}
.toast.on{opacity:1}
</style>
</head>
<body>
<h1>GolfTracker</h1>
<div class="tabs"><button id="t-rounds">Rounds</button><button id="t-courses">Courses</button><button id="t-settings">Settings</button></div>
<main id="view"></main>
<div class="toast" id="toast"></div>
<script>
const $=s=>document.querySelector(s);
const API='https://api.golfcourseapi.com';
let tab='rounds',courses=[],rounds=[],settings={},editing=null,search={q:''},gbLink=null;
const esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sum=a=>a.reduce((x,y)=>x+y,0);
const toPar=d=>d===0?'E':(d>0?'+'+d:''+d);
function toast(m){const t=$('#toast');t.textContent=m;t.classList.add('on');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('on'),2500)}

// The display has no clock, so remember the date the phone first saw each round.
function stamp(){let seen={};try{seen=JSON.parse(localStorage.seen||'{}')}catch(e){}
const today=new Date().toLocaleDateString();rounds.forEach(r=>{if(!seen[r.id])seen[r.id]=today;r.date=seen[r.id]});
try{localStorage.seen=JSON.stringify(seen)}catch(e){}}

async function api(path,body){const r=await fetch(path,body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||'Request failed');return j}

async function load(){try{[courses,settings]=await Promise.all([api('/api/courses'),api('/api/settings')]);
const h=await api('/api/rounds');rounds=h.rounds.slice().reverse();stamp()}
catch(e){$('#view').innerHTML='<div class="empty">Could not reach the display.<br>Is Phone sync running, and are you on the same network (hotspot or <b>GolfTracker</b> Wi-Fi)?</div>';return}render()}

// ---- Rounds ----
function played(r){const p=[],s=[];r.strokes.forEach((x,i)=>{if(x){p.push(r.par[i]);s.push(x)}});return{par:sum(p),strokes:sum(s),n:s.length}}
function cell(st,par){if(!st)return'<td>–</td>';const d=st-par;const c=d<=-2?'e':d==-1?'b':d==1?'o':d>=2?'d':'';return`<td><span class="s ${c}">${st}</span></td>`}
function half(r,a,b,label){const idx=[];for(let i=a;i<b&&i<r.par.length;i++)idx.push(i);if(!idx.length)return'';
const ps=sum(idx.map(i=>r.par[i])),ss=sum(idx.map(i=>r.strokes[i]));
return`<table><tr><th class="lbl">Hole</th>${idx.map(i=>`<th>${(r.first||1)+i}</th>`).join('')}<th>${label}</th></tr>
<tr><td class="lbl">Par</td>${idx.map(i=>`<td>${r.par[i]}</td>`).join('')}<td class="sum">${ps}</td></tr>
<tr><td class="lbl">Score</td>${idx.map(i=>cell(r.strokes[i],r.par[i])).join('')}<td class="sum">${ss||'–'}</td></tr></table>`}

function renderRounds(){if(!rounds.length)return'<div class="empty">No rounds yet.<br>Play a round on the display and it will show up here.</div>';
return rounds.map(r=>{const p=played(r);const n=r.par.length;
return`<div class="card"><div class="row"><div><div class="title">${esc(r.course)}</div>
<div class="muted">${esc(r.date)} · Round #${r.id}${p.n<n?` · ${p.n}/${n} holes`:''}${r.sent?' · <span style="color:var(--accent)">Sent to GolfBox</span>':r.skip?' · Not sent to GolfBox':''}</div></div>
<div style="text-align:right"><div class="score">${p.strokes}</div><div class="muted">${toPar(p.strokes-p.par)}</div></div></div>
${half(r,0,9,n>9?'Out':'Tot')}${half(r,9,18,'In')}
<div class="btns"><button class="b p" onclick="copyRound(${r.id})">Copy scores</button>
${r.sent||r.skip?`<button class="b" onclick="resendRound(${r.id})">${r.sent?'Send to GolfBox again':'Send to GolfBox'}</button>`:''}
<button class="b x" onclick="delRound(${r.id})">Delete</button></div></div>`}).join('')}

async function resendRound(id){try{await api('/api/rounds/resend?id='+id,{});const r=rounds.find(r=>r.id==id);if(r){r.sent=false;r.skip=false}render();
toast(settings.wifiSsid?'Sending now if the display is online, otherwise on the next sync':'Will be sent on the next sync with internet')}catch(e){toast(e.message)}}

async function delRound(id){if(!confirm('Delete this round from the display?'))return;
try{await api('/api/rounds/delete?id='+id,{});rounds=rounds.filter(r=>r.id!=id);render()}catch(e){toast(e.message)}}

function copyRound(id){const r=rounds.find(r=>r.id==id),p=played(r);
const lines=[`${r.course} – ${r.date}`];r.strokes.forEach((s,i)=>lines.push(`Hole ${(r.first||1)+i} (par ${r.par[i]}): ${s||'-'}`));
lines.push(`Total: ${p.strokes} (${toPar(p.strokes-p.par)})`);
// navigator.clipboard needs https, so use the textarea fallback that works over http on iOS.
const ta=document.createElement('textarea');ta.value=lines.join('\n');ta.setAttribute('readonly','');ta.style.position='fixed';ta.style.opacity='0';
document.body.appendChild(ta);ta.select();ta.setSelectionRange(0,99999);let ok=false;try{ok=document.execCommand('copy')}catch(e){}
ta.remove();toast(ok?'Copied – paste into GolfBox or Notes':'Copy failed')}

// ---- Course search (GolfCourseAPI, called straight from the phone) ----
async function gca(path){let r;
try{r=await fetch(API+path,{headers:{Authorization:'Bearer '+settings.apiKey}})}
catch(e){throw new Error('No internet on the phone. Add your home Wi-Fi or hotspot in Settings, then open the display from that network.')}
if(r.status==401||r.status==403)throw new Error('The API key was not accepted. Check it in Settings.');
if(r.status==429)throw new Error('Daily API limit reached. Try again tomorrow.');
if(!r.ok)throw new Error('Course search failed ('+r.status+')');return r.json()}

function courseName(c){const club=(c.club_name||'').trim(),course=(c.course_name||'').trim();
const n=!course||club.toLowerCase()==course.toLowerCase()?club:!club?course:`${club} - ${course}`;return n.slice(0,40)}
function place(c){const l=c.location||{};return[l.city,l.country].filter(Boolean).join(', ')||l.address||''}

// Tees usually share pars; merge identical ones so there is only a choice when it matters.
function parOptions(c){const t=c.tees||{},m=new Map();
const tees=[...(t.male||[]).map(x=>({...x,g:'Men'})),...(t.female||[]).map(x=>({...x,g:'Women'}))].filter(x=>Array.isArray(x.holes)&&x.holes.length);
for(const x of tees){const par=x.holes.slice(0,18).map(h=>+h.par),k=par.join(),label=`${x.tee_name||'Tee'} (${x.g})`;
if(m.has(k))m.get(k).labels.push(label);else m.set(k,{labels:[label],par})}
return[...m.values()].map(o=>({label:o.labels.join(', '),par:o.par,sub:`${o.par.length} holes · Par ${sum(o.par)}`}))}

async function doSearch(){const q=$('#q').value.trim();if(!q)return;search={q,loading:true};render();
try{const j=await gca('/v1/search?search_query='+encodeURIComponent(q));search={q,results:j.courses||[]}}
catch(e){search={q,error:e.message}}render()}

async function pickCourse(i){const hit=search.results[i];search.loading=true;search.error='';render();
try{const j=await gca('/v1/courses/'+encodeURIComponent(hit.id));const c=j.course||j;const options=parOptions(c);
search.loading=false;if(!options.length){search.error='No scorecard for this course. Add it manually instead.';return render()}
search.course={name:courseName(c),options};if(options.length==1)return importOption(0);render()}
catch(e){search.loading=false;search.error=e.message;render()}}

function importOption(i){const c=search.course,o=c.options[i];
const par=o.par.map(p=>[3,4,5,6].includes(p)?p:4);
editing={index:courses.findIndex(x=>x.name.toLowerCase()==c.name.toLowerCase()),name:c.name,par,source:o.label};render()}

function renderSearch(){
if(!settings.apiKey)return`<div class="card"><div class="title">Find a course</div>
<div class="muted" style="margin-top:4px">Search 30,000+ courses and import the pars automatically. Add a free GolfCourseAPI key in
<a href="#" onclick="go('settings');return false">Settings</a> first.</div></div>`;
const s=search;let body='';
if(s.loading)body='<div class="muted" style="margin-top:12px">Loading…</div>';
else if(s.error)body=`<div class="err">${esc(s.error)}</div>`;
else if(s.course)body=`<div class="muted" style="margin-top:12px">Which pars for <b>${esc(s.course.name)}</b>?</div>
${s.course.options.map((o,i)=>`<button class="item" onclick="importOption(${i})"><b>${esc(o.label)}</b><div class="muted">${o.sub}</div></button>`).join('')}
<div class="btns"><button class="b" onclick="search.course=null;render()">Back to results</button></div>`;
else if(s.results)body=s.results.length?s.results.map((c,i)=>`<button class="item" onclick="pickCourse(${i})"><b>${esc(courseName(c))}</b>
<div class="muted">${esc(place(c))}</div></button>`).join(''):'<div class="muted" style="margin-top:12px">No courses found. Try part of the club name, or add it manually.</div>';
return`<div class="card"><div class="title">Find a course</div>
<form class="search" onsubmit="doSearch();return false"><input id="q" type="search" placeholder="Club or course name" value="${esc(s.q)}">
<button class="b p">Search</button></form>${body}</div>`}

// ---- Courses ----
function renderCourses(){if(editing)return renderEditor();if(gbLink)return renderGbLink();
const list=courses.map((c,i)=>`<div class="card"><div class="row"><div><div class="title">${esc(c.name)}</div>
<div class="muted">${c.par.length} holes · Par ${sum(c.par)}</div>
<div class="muted">${c.gamebook?`GameBook: ${esc(c.gamebook.name)}`:'GameBook: not linked'}</div></div>
<div><button class="b" onclick="edit(${i})">Edit</button></div></div>
<div class="btns"><button class="b" onclick="gbStart(${i})">${c.gamebook?'Change GameBook link':'Link GameBook'}</button>
${c.gamebook?`<button class="b x" onclick="gbUnlink(${i})">Unlink</button>`:''}</div></div>`).join('');
return`<button class="b p" style="width:100%;margin-bottom:12px" onclick="gbStart(-1)">Import from GameBook</button>`+
renderSearch()+(list||'<div class="empty">No courses on the display yet.</div>')+
'<button class="b" style="width:100%" onclick="edit(-1)">Add course manually</button>'}

function renderEditor(){const e=editing;
return`<div class="card"><div class="title">${e.index<0?'New course':'Edit course'}</div>
${e.source?`<div class="muted">Imported from GolfCourseAPI (${esc(e.source)}). Check the pars, then tap Save.</div>`:''}
<label>Name</label><input id="name" maxlength="40" value="${esc(e.name)}" oninput="editing.name=this.value" placeholder="e.g. Oslo Golfklubb">
<label>Holes</label><div class="seg"><button class="b ${e.par.length==9?'p':''}" onclick="setHoles(9)">9</button>
<button class="b ${e.par.length==18?'p':''}" onclick="setHoles(18)">18</button></div>
<label>Par per hole · total ${sum(e.par)}</label><div class="pars">${e.par.map((p,i)=>`<div>Hole ${i+1}
<select onchange="editing.par[${i}]=+this.value;render()">${[3,4,5,6].map(v=>`<option ${v==p?'selected':''}>${v}</option>`).join('')}</select></div>`).join('')}</div>
<div class="btns"><button class="b p" onclick="saveCourse()">Save</button><button class="b" onclick="editing=null;render()">Cancel</button>
${e.index>=0?'<button class="b x" onclick="delCourse()">Delete course</button>':''}</div></div>`}

function edit(i){const c=i<0?{name:'',par:Array(18).fill(4)}:courses[i];editing={index:i,name:c.name,par:c.par.slice()};render()}
function setHoles(n){const p=editing.par.slice(0,n);while(p.length<n)p.push(4);editing.par=p;render()}
async function pushCourses(next){try{courses=await api('/api/courses',next);editing=null;search.course=null;render();toast('Saved to display')}catch(e){toast(e.message)}}
function saveCourse(){const name=editing.name.trim();if(!name)return toast('Give the course a name');
const next=courses.slice();const c={name,par:editing.par};
if(editing.index>=0&&courses[editing.index].gamebook)c.gamebook=courses[editing.index].gamebook;if(editing.index<0)next.push(c);else next[editing.index]=c;pushCourses(next)}
function delCourse(){if(!confirm('Delete this course?'))return;const next=courses.slice();next.splice(editing.index,1);pushCourses(next)}

// ---- GameBook course link (the display looks courses up in GameBook over the hotspot) ----
async function gbFetch(path){const r=await fetch(path);const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||'GameBook lookup failed');return j}
async function gbStart(i){gbLink={index:i,loading:true};render();
try{gbLink.favs=await gbFetch('/api/gamebook/favorites')}catch(e){gbLink.error=e.message}gbLink.loading=false;render()}
async function gbPick(k){const f=gbLink.favs[k];gbLink.loading=true;gbLink.error='';render();
try{gbLink.course=await gbFetch('/api/gamebook/course?id='+f.courseId);if(gbLink.course.tees.length==1)return gbSave(0)}catch(e){gbLink.error=e.message}
gbLink.loading=false;render()}
// GameBook's name for a course: the course name, with the club in front when the
// course name alone doesn't say which club (e.g. "Hovedbanen").
function gbCourseName(c){const club=(c.club||'').trim(),course=(c.course||'').trim(),first=club.split(' ')[0].toLowerCase();
return(course.toLowerCase().includes(first)?course:`${club} - ${course}`).slice(0,40)}
function gbSave(t){const c=gbLink.course,tee=c.tees[t];const next=courses.slice();
const link={courseId:c.courseId,teeboxId:tee.id,name:`${c.club} / ${c.course} - ${tee.name}`};
if(gbLink.index>=0){next[gbLink.index]={...next[gbLink.index],gamebook:link};gbLink=null;return pushCourses(next)}
// Import: a new course with GameBook's pars (or update one with the same name).
const par=(c.par||[]).slice(0,18);
if(!par.length||par.some(p=>p<3||p>6)){gbLink.error='GameBook has no usable pars for this course - add it manually and link it instead.';return render()}
const name=gbCourseName(c),i=next.findIndex(x=>x.name.toLowerCase()==name.toLowerCase());
const course={name,par,gamebook:link};if(i>=0)next[i]=course;else next.push(course);
gbLink=null;pushCourses(next)}
function gbUnlink(i){const next=courses.slice();const c={...next[i]};delete c.gamebook;next[i]=c;pushCourses(next)}
function renderGbLink(){const g=gbLink,importing=g.index<0,name=importing?'':courses[g.index].name;let body='';
if(g.loading)body='<div class="muted" style="margin-top:12px">Asking GameBook…</div>';
else if(g.error)body=`<div class="err">${esc(g.error)}</div>`;
else if(g.course)body=`<div class="muted" style="margin-top:12px">Which tee do you play on <b>${esc(g.course.course)}</b>?</div>
${importing?`<div class="muted">${(g.course.par||[]).length} holes · Par ${sum(g.course.par||[])}</div>`:''}
${g.course.tees.map((t,k)=>`<button class="item" onclick="gbSave(${k})"><b>${esc(t.name)}</b>${t.id==g.course.defaultTee?'<div class="muted">GameBook default</div>':''}</button>`).join('')}`;
else if(g.favs)body=g.favs.length?`<div class="muted" style="margin-top:12px">Your GameBook favourites - ${importing?'pick a course to add:':`pick the one that is <b>${esc(name)}</b>:`}</div>
${g.favs.map((f,k)=>`<button class="item" onclick="gbPick(${k})"><b>${esc(f.course)}</b><div class="muted">${esc(f.club)} · ${f.holes} holes</div></button>`).join('')}`
:'<div class="muted" style="margin-top:12px">You have no favourite courses in GameBook.</div>';
return`<div class="card"><div class="title">${importing?'Import from GameBook':`Link ${esc(name)} to GameBook`}</div>${body}
<div class="muted" style="margin-top:12px">Missing a course? Tap the star on it in the GameBook app to make it a favourite, then try again.</div>
<div class="btns"><button class="b" onclick="gbLink=null;render()">Cancel</button></div></div>`}

// ---- Settings ----
function renderSettings(){return`<div class="card"><div class="title">Course search</div>
<div class="muted" style="margin-top:4px">Get a free API key at <a href="https://golfcourseapi.com" target="_blank">golfcourseapi.com</a>.
The free plan allows 35 requests a day; importing one course uses 2.</div>
<label>GolfCourseAPI key</label><input id="key" value="${esc(settings.apiKey)}" autocomplete="off" autocapitalize="off" spellcheck="false">
<div class="btns"><button class="b p" onclick="saveSettings({apiKey:$('#key').value},'API key saved')">Save key</button></div></div>
<div class="card"><div class="title">Hotspot</div>
<div class="muted" style="margin-top:4px">Used by <b>Phone sync → Hotspot</b>: the display joins this network (your iPhone's
Personal Hotspot, or home Wi-Fi) and shows its address to open in Safari. Your phone keeps its internet, so course search
and GolfBox upload work. Turn on <i>Maximize Compatibility</i> on the iPhone - the display only sees 2.4 GHz.</div>
<label>Network name</label><input id="ssid" value="${esc(settings.wifiSsid)}" autocomplete="off" autocapitalize="off" spellcheck="false">
<label>Password</label><input id="pass" type="password" placeholder="${settings.hasWifiPass?'Unchanged':''}">
<div class="btns"><button class="b p" onclick="saveWifi()">Save Wi-Fi</button></div></div>
<div class="card"><div class="title">Golf GameBook</div>
<div class="muted" style="margin-top:4px">The display logs in to GameBook itself (over the hotspot) to link your courses and, later,
send scores. Use the email and password you log in to the GameBook app with.</div>
<label>Email</label><input id="gbemail" type="email" value="${esc(settings.gamebookEmail)}" autocomplete="off" autocapitalize="off" spellcheck="false">
<label>Password</label><input id="gbpass" type="password" placeholder="${settings.hasGamebookPass?'Saved - leave empty to keep':''}" autocomplete="off">
<div class="btns"><button class="b p" onclick="saveGamebook()">Save</button>
${settings.hasGamebookPass?'<button class="b x" onclick="saveSettings({gamebookEmail:\'\',gamebookPass:\'\'},\'GameBook login removed\')">Remove</button>':''}</div></div>
<div class="card"><div class="title">GolfBox auto-upload</div>
<div class="muted" style="margin-top:4px">When the display syncs via Hotspot, new rounds go to the
<b>GolfBox upload</b> GitHub Action, which registers them in GolfBox. Use a fine-grained GitHub token with
<i>Contents: read and write</i> on this repository only.</div>
<label>GitHub repository</label><input id="ghrepo" value="${esc(settings.githubRepo)}" placeholder="owner/name" autocomplete="off" autocapitalize="off" spellcheck="false">
<label>GitHub token</label><input id="ghtoken" type="password" placeholder="${settings.hasGithubToken?'Saved – leave empty to keep':'github_pat_…'}" autocomplete="off">
<div class="btns"><button class="b p" onclick="saveGithub()">Save</button>
${settings.hasGithubToken?'<button class="b x" onclick="saveSettings({githubToken:\'\'},\'Token removed\')">Remove token</button>':''}</div></div>`}

function saveGamebook(){const body={gamebookEmail:$('#gbemail').value.trim()},p=$('#gbpass').value;if(p)body.gamebookPass=p;
saveSettings(body,'GameBook login saved')}
function saveGithub(){const body={githubRepo:$('#ghrepo').value.trim()},t=$('#ghtoken').value.trim();if(t)body.githubToken=t;
saveSettings(body,'GolfBox upload saved')}

async function saveSettings(body,msg){try{settings=await api('/api/settings',body);render();toast(msg)}catch(e){toast(e.message)}}
function saveWifi(){const ssid=$('#ssid').value.trim(),pass=$('#pass').value;const body={wifiSsid:ssid};
if(pass||!ssid)body.wifiPass=pass;saveSettings(body,ssid?'Saved – used next time you choose Hotspot sync':'Hotspot removed')}

// ---- Shell ----
function render(){for(const t of['rounds','courses','settings'])$('#t-'+t).classList.toggle('on',tab==t);
$('#view').innerHTML=tab=='rounds'?renderRounds():tab=='courses'?renderCourses():renderSettings()}
function go(t){tab=t;editing=null;render()}
for(const t of['rounds','courses','settings'])$('#t-'+t).onclick=()=>go(t);
render();load();
</script>
</body>
</html>)HTML";
