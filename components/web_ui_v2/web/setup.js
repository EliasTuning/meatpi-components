/* web_ui_v2 on-demand chunk: the Quick Setup wizard (#/setup[/<screen>]).
   Loaded by the PAGES.setup stub in index.html the first time the page
   opens; shares the page's globals (h, ic, api, tryGet, toast, modal,
   banner, note, chip, page, conn, store, expectReboot, apDefaultPassword,
   vehicleProfilePicker, profileToPids). Design: TASK_quick_setup.md and the
   mockup artifact (2026-10-01).

   Ten screens in two halves. Screens 1 to 6 collect everything that is
   reboot-to-apply (AP password, home WiFi, MQTT or Home Assistant) and
   screen 6 saves it in ONE restart; the phone changes network there, so
   screens 7 to 10 rebuild from device state and resume from the URL
   (#/setup/checks), never from browser storage. */
(function(){
"use strict";

const STEPS=[
  {id:"safety",t:"Safety first",g:"Before the restart"},
  {id:"use",t:"How you use WiCAN"},
  {id:"details",t:()=>W.use==="mqtt"?"MQTT broker":"Home Assistant",hide:()=>W.use==="wifi"},
  {id:"ap",t:"Access point"},
  {id:"wifi",t:"Home WiFi"},
  {id:"usb",t:"USB devices"},
  {id:"review",t:"Review and restart"},
  {id:"reconnect",t:"Reconnect",g:"After the restart"},
  {id:"checks",t:"Checks"},
  {id:"vehicle",t:"Your vehicle"},
  {id:"power",t:"Battery and sleep"},
  {id:"polling",t:"Reading the car"},
  {id:"done",t:"Done"},
];
const IDS=STEPS.map(s=>s.id);
const PROTO={"6":"CAN 11-bit 500 kbit/s","7":"CAN 29-bit 500 kbit/s","8":"CAN 11-bit 250 kbit/s","9":"CAN 29-bit 250 kbit/s"};
/* how a car wants its legislated data asked (the vehicle store's `dialect`):
   only the newer one is worth a word, OBD-II is what everybody expects */
const DIALECT={uds:"OBD on UDS (ISO 27145)",j1939:"J1939"};
const HA_REPO="https://github.com/jay-oswald/ha-wican";
const SKIP_KEY="wican-setup-skip";

/* wizard state: survives in-page navigation and re-routes (module scope),
   not a reload on another address (the second half reads the device) */
const W={cur:null,use:"ha",agree:[false,false,false],haInstalled:false,mdataOk:false,
  apPw:"",apPw2:"",showPw:false,noWifi:false,alone:false,
  ssid:"",wifiAuth:"",wifiPw:"",manual:false,nets:null,scanErr:null,
  /* after it joins (2026-10-07): "sta" = Station only (recommended), "apsta"
     with apAuto = the access point off while WiCAN is on the network */
  wifiMode:"sta",apAuto:true,modeSeeded:false,
  /* the connection test (2026-10-06): tested = {ssid,pw,ip,rssi,took_ms,channel}
     for the credentials that passed; testNote = "ap" (no station interface
     until the restart) or "anyway" (a failure the user chose to keep) */
  tested:null,testNote:null,
  /* the USB connector (2026-10-07): kind = what the step found (null = not
     looked yet), live = the four routes' last answers, nlMode = how the
     ESPNetLink is used ("wifi_modem" | "usb"), ethFirst = the cable before
     WiFi, hostFix = switch a device-mode connector back to host */
  usb:{kind:null,live:null,nlMode:null,ethFirst:null,hostFix:false,apn:"",apnUser:"",apnPw:"",apnSeeded:false,pin:""},
  /* testing = {ssid,pw,t0} while a test runs: the page re-routes after the
     access point's blink (offline, then online again) and rebuilds this
     screen, so the new screen picks the running test up from here */
  testing:null,
  mqtt:{url:"",user:"",pw:"",prefix:"",period:5},
  applied:false,noVeh:false,
  car:null,noProfile:false,proto:"0",plugged:false,ignition:false,
  scan:"idle",scanStatus:null,scanResult:null,stdPick:null,stdDefault:[],
  vehPhase:null,veh:null,vehName:"",profChoice:null,profCar:null,profList:undefined,carChoice:"detect",vehNameAuto:false,
  poll:{rate:5,rateCustom:3,minEvent:1,pause:"sleep",pauseV:12.5,pauseAll:false,std:true,specific:true,dtc:false,dtcMin:60},pollSeeded:false,
  /* the OBD chip's firmware prompt on the vehicle step (2026-11-08):
     unknown | running | failed | done, the last error, the attempts */
  chip:{state:"unknown",err:"",tries:0},
  /* step 10, battery and sleep: the live trace, the two readings, the chosen pair */
  pwr:{phase:"idle",trace:[],base:null,charging:null,resting:null,tC:0,tD:0,tR:0,sleep:13.1,wake:13.2,touched:false,narrow:false,measured:false,skipped:false,misses:0}};
/* device documents fetched while the wizard is open */
const D={chip:null,wifi:null,mqtt:null,dest:null,autopid:null,autopidSchema:null,cfg:null,vehicle:null,vehicles:null,webhook:null,wifiStatus:null,sleep:undefined,sleepSchema:undefined,usbhost:undefined,espnl:undefined,acm:undefined};

const CSS=`
.qs{display:grid;grid-template-columns:232px minmax(0,1fr);background:var(--surface);border:1px solid var(--border);border-radius:12px;box-shadow:var(--shadow);overflow:hidden;min-height:560px}
.qs-rail{background:var(--surface-2);border-right:1px solid var(--border);padding:18px 14px 18px 18px;display:flex;flex-direction:column;gap:4px}
.qs-rail .grp{font-size:10.5px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--text-3);padding:10px 0 6px 6px}
.qs-step{display:grid;grid-template-columns:22px 1fr;gap:10px;align-items:center;padding:7px 8px;border-radius:9px;border:0;background:none;text-align:left;cursor:pointer;color:var(--text-2);font-weight:600;font-size:13px;font-family:inherit}
.qs-step:hover:not(:disabled){background:var(--surface-3)}
.qs-step:disabled{cursor:default}
.qs-step .n{width:22px;height:22px;border-radius:50%;border:2px solid var(--border-strong);display:grid;place-items:center;font-size:11px;font-weight:700;color:var(--text-3);background:var(--surface)}
.qs-step .n svg{width:12px;height:12px}
.qs-step.cur{background:var(--primary-tint);color:var(--primary)}
.qs-step.cur .n{border-color:var(--primary);color:var(--primary)}
.qs-step.done{color:var(--text)}
.qs-step.done .n{background:var(--success);border-color:var(--success);color:#fff}
.qs-rail .div{height:1px;background:var(--border);margin:8px 6px}
.qs-rail .restart{display:flex;align-items:center;gap:8px;font-size:11.5px;color:var(--warning);font-weight:700;padding:4px 6px 2px}
.qs-rail .restart svg{width:14px;height:14px;flex:none}
.qs-screen{padding:26px 30px 22px;display:flex;flex-direction:column;gap:18px;min-width:0}
.qs-screen h2{font-size:20px;font-weight:800;letter-spacing:-.015em;margin:0}
.qs-lead{color:var(--text-2);max-width:62ch;margin:0 0 8px}
.qs-body{display:flex;flex-direction:column;gap:14px;flex:1}
.qs-foot{display:flex;align-items:center;gap:10px;flex-wrap:wrap;border-top:1px solid var(--border);padding-top:16px;margin-top:4px}
.qs-foot .ghost{color:var(--text-3);font-size:12.5px}
.qs-agree{display:flex;gap:11px;align-items:flex-start;padding:12px 14px;border:1px solid var(--border);border-radius:10px;cursor:pointer;background:var(--surface)}
.qs-agree:has(input:checked){border-color:color-mix(in srgb,var(--success) 50%,transparent);background:var(--success-tint)}
.qs-agree input{width:18px;height:18px;margin:2px 0 0;flex:none;accent-color:var(--success)}
.qs-agree b{display:block;margin-bottom:2px}
.qs-agree span{color:var(--text-2);font-size:13px;line-height:1.45}
.qs-tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}
.qs-tile{position:relative;text-align:left;padding:16px 16px 14px;border:1px solid var(--border);border-radius:12px;background:var(--surface);cursor:pointer;display:flex;flex-direction:column;gap:8px;min-height:150px;font-family:inherit;color:var(--text)}
.qs-tile:hover:not(.later){border-color:var(--border-strong)}
.qs-tile.sel{border-color:var(--primary);box-shadow:0 0 0 3px var(--focus);background:var(--primary-tint)}
.qs-tile.later{opacity:.6;cursor:default}
.qs-tile.static{cursor:default}
.qs-nlcard{border:1px solid var(--border);border-radius:10px;background:var(--surface)}
.qs-nlcard .qs-check{border:0;background:none}
.qs-nlrows{grid-template-columns:90px 1fr;gap:6px 14px;padding:0 14px 12px 58px;font-size:12.5px}
.qs-nlrows dt{padding-top:3px}
.qs-nlrows dd .d2{font-size:12.5px}
.qs-nlrows .bars{font-family:var(--mono);color:var(--primary);letter-spacing:1px}
.qs-tile .ticon{width:34px;height:34px;border-radius:9px;display:grid;place-items:center;background:var(--surface-3);color:var(--primary)}
.qs-tile.sel .ticon{background:var(--surface)}
.qs-tile .ticon svg{width:18px;height:18px}
.qs-tile h3{font-size:14.5px;font-weight:700;margin:0}
.qs-tile p{color:var(--text-2);font-size:12.5px;line-height:1.45;margin:0}
.qs-tile .tag{position:absolute;top:12px;right:12px}
.qs-row{display:grid;grid-template-columns:180px minmax(0,1fr);gap:5px 20px;align-items:start;padding:7px 0}
.qs-row>label{color:var(--text-2);font-weight:600;font-size:13.5px;padding-top:9px;line-height:1.4}
.qs-row .ctl{min-width:0;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.qs-row .ctl input{max-width:360px}
.qs-row .help{grid-column:2;color:var(--text-3);font-size:12px;line-height:1.45}
.qs-row .help.err{color:var(--danger)}
.qs-strength{height:6px;border-radius:3px;background:var(--surface-3);overflow:hidden;width:360px;max-width:100%;margin-bottom:4px}
.qs-strength i{display:block;height:100%;background:var(--warning);transition:width .2s}
.qs-strength.good i{background:var(--success)}
.qs-nets{border:1px solid var(--border);border-radius:10px;overflow:hidden}
.qs-net{display:grid;grid-template-columns:1fr 60px 90px;gap:12px;align-items:center;padding:10px 14px;border-top:1px solid var(--border);cursor:pointer;background:var(--surface)}
.qs-net:first-child{border-top:0}
.qs-net:hover{background:var(--surface-2)}
.qs-net.sel{background:var(--primary-tint)}
.qs-net .ssid{font-weight:600;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.qs-net.sel .ssid::after{content:"Selected";margin-left:10px;font-size:11px;color:var(--primary);font-weight:700}
.qs-net .bars{font-family:var(--mono);color:var(--primary);letter-spacing:1px}
.qs-details{border:1px solid var(--border);border-radius:10px;padding:10px 14px;background:var(--surface-2)}
.qs-details summary{cursor:pointer;font-weight:600;color:var(--text-2);font-size:13px}
.qs-details[open] summary{margin-bottom:8px}
.qs-kv{display:grid;grid-template-columns:170px 1fr;gap:8px 18px;margin:0}
.qs-kv dt{color:var(--text-3);font-weight:600;font-size:12.5px;padding-top:2px}
.qs-kv dd{margin:0;display:flex;gap:8px;align-items:center;flex-wrap:wrap;min-width:0}
.qs-kv dd .d2{color:var(--text-2);font-size:12.5px}
.qs-rv{list-style:none;margin:0;padding:0;display:flex;flex-direction:column}
.qs-rv li{display:grid;grid-template-columns:30px 170px minmax(0,1fr);gap:0 16px;align-items:start;position:relative;padding:14px 0 15px}
.qs-rv li:first-child{padding-top:2px}
.qs-rv li::before{content:"";position:absolute;left:14px;top:0;bottom:0;width:2px;background:var(--border)}
.qs-rv li:first-child::before{top:6px}
.qs-rv li:last-child::before{bottom:auto;height:18px}
.qs-rv li+li::after{content:"";position:absolute;left:46px;right:0;top:0;height:1px;background:var(--border)}
.qs-rv .ci{width:30px;height:30px;border-radius:50%;display:grid;place-items:center;background:var(--success);color:#fff;position:relative;z-index:1}
.qs-rv .ci svg{width:16px;height:16px}
.qs-rv .ci.pend{background:var(--surface);border:2px solid var(--border-strong)}
.qs-rv .ci.warn{background:var(--warning)}
.qs-rv b{font-size:15px;color:var(--text);padding-top:5px}
.qs-rv .c{min-width:0;display:flex;flex-direction:column;gap:4px;padding-top:5px}
.qs-rv .v{display:flex;flex-wrap:wrap;align-items:baseline;gap:4px 12px;font-size:14px}
.qs-rv .d2{color:var(--text-2);font-size:12.5px;line-height:1.55}
.qs-rv .d2 i{font-style:normal;color:var(--text-3);padding:0 7px}
/* the popup rules (.qs-pop, .qs-pop-modal, .qs-spin) live in index.html since 2026-11-08: the Status page shows the chip firmware popup too */
.qs-steps{display:flex;flex-direction:column;gap:12px;counter-reset:s;margin:0;padding:0}
.qs-steps li{display:grid;grid-template-columns:26px minmax(0,1fr);gap:2px 12px;align-items:start;list-style:none}
.qs-steps li::before{counter-increment:s;content:counter(s);grid-column:1;grid-row:1;width:24px;height:24px;border-radius:50%;background:var(--primary);color:var(--on-primary);display:grid;place-items:center;font-size:12px;font-weight:700;margin-top:1px}
.qs-steps li>b{grid-column:2;grid-row:1;display:block;padding-top:3px}
.qs-steps li>span{grid-column:2;grid-row:2;display:block;color:var(--text-2);font-size:13px;line-height:1.45}
.qs-check{display:grid;grid-template-columns:32px minmax(0,1fr) auto;gap:12px;align-items:center;padding:12px 14px;border:1px solid var(--border);border-radius:10px;background:var(--surface)}
.qs-check .ci{width:30px;height:30px;border-radius:8px;display:grid;place-items:center;background:var(--surface-3);color:var(--text-2)}
.qs-check .ci svg{width:16px;height:16px}
.qs-check b{display:block;font-size:13.5px}
.qs-check span.d2{color:var(--text-2);font-size:12.5px;display:block}
.qs-check.ok .ci{background:var(--success-tint);color:var(--success)}
.qs-check.warn .ci{background:var(--warning-tint);color:var(--warning)}
.qs-check.crit .ci{background:var(--danger-tint);color:var(--danger)}
.qs-link{display:flex;flex-direction:column;gap:10px;padding:18px 20px;border:1px solid color-mix(in srgb,var(--primary) 35%,transparent);border-radius:12px;background:var(--primary-tint)}
.qs-link .url{font-family:var(--mono);font-size:15px;font-weight:600;overflow-wrap:anywhere;color:var(--text);user-select:all;-webkit-user-select:all}
.qs-link .alt{margin:0;color:var(--text-2);font-size:13px;line-height:1.5}
.qs-link .alt .u{font-family:var(--mono);font-weight:600;overflow-wrap:anywhere}
.qs-row input.qs-bad{box-shadow:0 0 0 2px var(--warning)}
.qs-link .acts{display:flex;gap:8px;flex-wrap:wrap}
.qs-sub{display:flex;align-items:center;gap:10px;margin-top:6px}
.qs-sub h3{font-size:13px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--text-3);margin:0}
.qs-sub::after{content:"";flex:1;height:1px;background:var(--border)}
.qs-pick{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.qs-pick .sel{display:inline-flex;align-items:center;gap:8px;padding:7px 12px;border:1px solid var(--border);border-radius:9px;background:var(--surface-2);font-weight:600}
.qs-pick .sel svg{width:16px;height:16px;flex:none;color:var(--primary)}
.qs-radios{display:flex;flex-direction:column;gap:6px}
.qs-radios label{display:flex;gap:9px;align-items:center;font-size:13.5px;cursor:pointer}
.qs-radios input{width:16px;height:16px;margin:0;accent-color:var(--primary)}
.qs-next{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
.qs-next a{display:flex;flex-direction:column;gap:3px;padding:12px 14px;border:1px solid var(--border);border-radius:10px;text-decoration:none;color:var(--text);background:var(--surface)}
.qs-next a b{color:var(--primary)}
.qs-next a span{font-size:12.5px;color:var(--text-2)}
.qs-inline{display:flex;gap:8px;flex-wrap:wrap;align-items:center;font-size:12.5px;color:var(--text-3)}
/* step 10: the live battery card, its trace and the two threshold rows */
.qs-live{display:grid;grid-template-columns:minmax(150px,auto) minmax(0,1fr);gap:14px 22px;align-items:start;padding:14px 16px;border:1px solid var(--border);border-radius:12px;background:var(--surface-2)}
.qs-live .big{font-family:var(--mono);font-size:34px;font-weight:600;line-height:1;letter-spacing:-.02em;font-variant-numeric:tabular-nums}
.qs-live .big small{font-size:15px;color:var(--text-3);margin-left:4px;font-weight:500}
.qs-live .lbl{font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--text-3);margin-bottom:6px}
.qs-live .st{margin-top:8px}
.qs-trace svg{width:100%;height:auto;display:block}
.qs-trace .g{stroke:var(--border);stroke-width:1}
.qs-trace .l{fill:none;stroke:var(--primary);stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.qs-trace .f{fill:var(--primary);opacity:.08}
.qs-trace .t{stroke-dasharray:4 4;stroke-width:1.5}
.qs-trace .t.sleep{stroke:var(--warning)}
.qs-trace .t.wake{stroke:var(--success)}
.qs-trace text{font-family:var(--mono);font-size:10.5px;fill:var(--text-3)}
.qs-trace text.sleep{fill:var(--warning)}
.qs-trace text.wake{fill:var(--success)}
.qs-trace .m{fill:var(--surface);stroke:var(--primary);stroke-width:2}
.qs-trace .m.e{fill:var(--primary)}
.qs-thr{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.qs-thr-row{display:flex;flex-direction:column;gap:6px;padding:12px 14px;border:1px solid var(--border);border-radius:10px;background:var(--surface)}
.qs-thr-row>b{display:flex;align-items:center;gap:8px;font-size:13.5px}
.qs-thr-row>b i{width:10px;height:3px;border-radius:2px;display:inline-block}
.qs-thr-row .val{font-family:var(--mono);font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.qs-thr-row input[type=range]{width:100%;max-width:none;padding:0;border:0;background:none;accent-color:var(--primary)}
.qs-thr-row>span{color:var(--text-2);font-size:12.5px;line-height:1.45}
@media (max-width:860px){
  .qs{grid-template-columns:1fr}
  .qs-live,.qs-thr{grid-template-columns:1fr}
  .qs-rail{flex-direction:row;flex-wrap:wrap;border-right:0;border-bottom:1px solid var(--border);padding:12px}
  .qs-rail .grp,.qs-rail .div,.qs-rail .restart{display:none}
  .qs-step{grid-template-columns:22px auto;padding:5px 8px}
  .qs-screen{padding:20px 18px}
  .qs-row,.qs-kv{grid-template-columns:1fr}
  .qs-rv li{grid-template-columns:30px minmax(0,1fr)}
  .qs-rv .c{grid-column:2;padding-top:0}
  .qs-row .help{grid-column:1}
  .qs-net{grid-template-columns:1fr 50px 70px}
}`;

/* ---------- small helpers ---------- */
const deviceId=()=>(conn.info&&conn.info.device_id)||(conn.status&&conn.status.device_id)||"";
const apName=()=>deviceId()?"WiCAN_"+deviceId():"the WiCAN access point";
const routerName=()=>"wican_"+(deviceId()||"");   /* the DHCP host name: a router's device list shows it */
/* this page rides the access point: a phone (or computer) on WiCAN's own
   network, which has no internet. The setting's address, the live status's
   before the settings are loaded, the factory default last. */
const onAp=()=>location.hostname===((D.wifi&&D.wifi.ap_ip)||(conn.wifi&&conn.wifi.ap_ip)||"192.168.0.10");
/* the one request of the phone (Ali, 2026-10-08: his phone dropped off the
   access point at the WiFi test and came back; one with mobile data on may
   not): the first screen carries it in full, the WiFi step repeats the
   short form before the test */
const mobileDataOff=()=>banner("info","phone",h("b",{},"On a phone: turn off mobile data until WiCAN is on your WiFi."),
  " WiCAN's network has no internet. With mobile data on, your phone may leave it for the internet during the two short pauses ahead (the WiFi test and the restart), and this page stops answering. The switch is under Settings > Mobile data (Cellular on some phones). A computer has nothing to turn off.");
const strip=o=>{const c={...(o||{})};delete c.degraded;delete c.pending_reboot;return c;};
const bars=r=>{const n=r>-55?4:r>-67?3:r>-78?2:1;return "▂▄▆█".slice(0,n).padEnd(4,"·");};
const netAuth=n=>String(n.auth_mode||n.auth||"").toUpperCase();
const isOpen=n=>!netAuth(n)||netAuth(n)==="OPEN";
const btn=(label,fn,cls,opt={})=>{const b=h("button",{class:"btn "+(cls||""),type:"button",disabled:!!opt.disabled,title:opt.title,onclick:fn},opt.icon?ic(opt.icon):null,label);return b;};
const check=(kind,icon,title,text,chipEl)=>h("div",{class:"qs-check "+(kind||"")},
  kind==="run"?h("span",{class:"qs-spin"}):h("span",{class:"ci"},ic(icon)),
  h("div",{},h("b",{},title),text?h("span",{class:"d2"},...(Array.isArray(text)?text:[text])):null),chipEl||h("span",{}));
const sub=t=>h("div",{class:"qs-sub"},h("h3",{},t));
const row=(label,ctl,help,err)=>h("div",{class:"qs-row"},h("label",{for:ctl.id||null},label),h("div",{class:"ctl"},ctl),help?h("div",{class:"help"+(err?" err":"")},help):null);
const steps=items=>h("ol",{class:"qs-steps"},...items.map(([b,s])=>h("li",{},h("b",{},b),h("span",{},...(Array.isArray(s)?s:[s])))));
const radio=(name,id,checked,title,text,onpick,extra)=>h("label",{class:"qs-agree",style:"padding:10px 14px"},
  h("input",{type:"radio",name,id,checked,onchange:onpick}),h("div",{},h("b",{},title),h("span",{},...(Array.isArray(text)?text:[text])),extra||null));
/* copying uses the page's copyText (index.html): the page is served over plain
   http, where navigator.clipboard does not exist (2026-10-06, Ali's phone) */
/* Is WiCAN reachable from this browser at the link? A no-CORS fetch answers
   (opaque) on any HTTP reply and rejects when the name does not resolve or
   nothing listens: enough to know this phone has arrived on the home WiFi and
   WiCAN is there. Nothing to add on the device for it. */
/* a GET that gives up after ms: a poll must not hang on a link that is
   down for the moment (the AP blinks, or the page rides the station the
   test drops: through a tunnel such a request sat for 40 s) */
async function tryGetTimed(p,ms){
  const ac=new AbortController();const t=setTimeout(()=>ac.abort(),ms);
  try{return await api(p,{signal:ac.signal});}catch(_){return null;}finally{clearTimeout(t);}
}
async function linkAnswers(origin){
  const ac=new AbortController();const t=setTimeout(()=>ac.abort(),2500);
  try{await fetch(origin+"/api/info",{mode:"no-cors",cache:"no-store",signal:ac.signal});return true;}
  catch(_){return false;}
  finally{clearTimeout(t);}
}
function skipSetup(){try{sessionStorage.setItem(SKIP_KEY,"1");}catch(_){}location.hash="#/status";}
/* the resume hash: the checks step, and the use case when it is not the
   default (the page state is lost at the new origin; without this the
   screens after the restart would wait for Home Assistant on every run) */
const resumeHash=()=>"#/setup/checks"+(W.use==="ha"?"":"/"+W.use);

/* ---------- navigation ---------- */
let railEl=null,scrEl=null,timers=[];
function every(ms,fn){const t=setInterval(()=>{if(!scrEl||!scrEl.isConnected){clearInterval(t);return;}fn();},ms);timers.push(t);return t;}
function stopTimers(){timers.forEach(clearInterval);timers=[];}
function go(id){
  if(!IDS.includes(id))id="safety";
  W.cur=id;
  try{history.replaceState(null,"","#/setup/"+id);}catch(_){}
  paint();
}
function paintRail(){
  const curIdx=IDS.indexOf(W.cur);
  railEl.replaceChildren();
  let lastG=null,n=0;
  STEPS.forEach((s,i)=>{
    if(s.hide&&s.hide())return;   /* a step this run does not have (the use-case screen of a WiFi-only run) */
    n++;
    if(s.g&&s.g!==lastG){
      if(lastG){railEl.append(h("div",{class:"div"}),h("div",{class:"restart"},ic("power"),"WiCAN restarts, your phone changes network"));}
      railEl.append(h("div",{class:"grp"},s.g));lastG=s.g;}
    const k=i===curIdx?"cur":i<curIdx?"done":"";
    const label=typeof s.t==="function"?s.t():s.t;
    /* earlier steps can be revisited (Fix WiFi, change the broker); later ones wait their turn */
    railEl.append(h("button",{class:"qs-step "+k,type:"button",disabled:i>curIdx,"data-step":s.id,onclick:()=>{if(i<=curIdx)go(s.id);}},
      h("span",{class:"n"},k==="done"?ic("check"):String(n)),h("span",{},label)));
  });
}
function paint(){
  stopTimers();
  paintRail();
  const fn=SCREENS[W.cur]||SCREENS.safety;
  scrEl.replaceChildren(h("div",{class:"loading"},h("div",{class:"spinner"})));
  Promise.resolve().then(()=>fn()).then(el=>{if(el)scrEl.replaceChildren(el);})
    .catch(e=>scrEl.replaceChildren(banner("crit","alert","This step could not be shown: "+(e.message||e))));
}
const screen=(title,lead,body,foot)=>h("div",{},h("h2",{},title),lead?h("p",{class:"qs-lead"},lead):null,h("div",{class:"qs-body"},...body),foot?h("div",{class:"qs-foot"},...foot):null);
const back=id=>btn("Back",()=>go(id),"");
const grow=()=>h("span",{class:"grow"});

/* ---------- screens ---------- */
const SCREENS={};

SCREENS.safety=async()=>{
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  const items=[
    ["Private networks only","I will connect WiCAN only to a private WiFi network I control, such as my home network. Never a public, hotel, cafe, office or guest network."],
    ["Keep the access point password private","I will set my own access point password and will not share the WiCAN network with people I do not trust."],
    ["My vehicle, my responsibility","I will use diagnostic features such as clearing trouble codes only on vehicles I own or am authorised to work on."]];
  const next=btn("Continue",()=>go("use"),"pri",{disabled:!W.agree.every(Boolean)});
  const boxes=items.map((it,i)=>h("label",{class:"qs-agree"},
    h("input",{type:"checkbox",id:"qs-agree"+i,checked:W.agree[i],onchange:e=>{W.agree[i]=e.target.checked;next.disabled=!W.agree.every(Boolean);}}),
    h("div",{},h("b",{},it[0]),h("span",{},it[1]))));
  const fresh=apDefaultPassword();
  return screen("Keep your WiCAN private",
    "WiCAN sits on your car's diagnostic port and on your WiFi. Read these three rules before it goes online. Each one needs a tick.",
    [...(onAp()?[mobileDataOff()]:[]),...boxes,note("","The web interface has no login by default. Anyone on the same private network can open it. You can add a password later under Settings.")],
    [btn(fresh?"Skip setup for now":"Exit setup",fresh?skipSetup:()=>{location.hash="#/status";},"gh sm"),grow(),next]);
};

SCREENS.use=()=>{
  const tile=(id,icon,title,body,tag)=>h("button",{class:"qs-tile"+(W.use===id?" sel":""),type:"button","data-use":id,onclick:()=>{W.use=id;paint();}},
    tag?h("span",{class:"tag chip ok"},tag):null,h("span",{class:"ticon"},ic(icon)),h("h3",{},title),h("p",{},body));
  const later=(icon,title,body)=>h("div",{class:"qs-tile later","aria-disabled":"true"},
    h("span",{class:"tag chip"},"Not in Quick Setup yet"),h("span",{class:"ticon"},ic(icon)),h("h3",{},title),h("p",{},body));
  /* WiFi only is a tile like the two others (Ali, 2026-10-07: "for users who
     want just to connect their car to WiFi without Home Assistant"): the
     use-case screen is skipped, and the screens after the restart learn the
     choice from the resume link */
  return screen("How will you use WiCAN?","Pick the one that matters most. Everything else stays available in the full interface afterwards.",
    [h("div",{class:"qs-tiles"},
      tile("ha","home","Home Assistant","Live vehicle data as Home Assistant entities through the WiCAN integration. Nothing to type here: Home Assistant finds WiCAN on your network.","Most common"),
      tile("mqtt","cloud","My own MQTT broker","Publish vehicle data to Mosquitto, HiveMQ, Node-RED or any broker. You enter the broker address and login on the next screen."),
      tile("wifi","wifi","Just connect WiCAN to my WiFi","No Home Assistant or broker. WiCAN joins your WiFi; OBD apps and this web interface reach it there. Add Home Assistant or MQTT any time by running Quick Setup again."),
      later("plug","OBD app over WiFi or Bluetooth","Car Scanner, Torque and similar apps. Works out of the box on the access point; set up in Settings."),
      later("route","ABRP, HTTP endpoints, data logger","Data destinations and the SD card logger live under Automate and Logger."))],
    [back("safety"),grow(),btn("Continue",()=>go(W.use==="wifi"?"ap":"details"),"pri")]);
};

SCREENS.details=async()=>{
  if(W.use==="wifi")return screen("WiFi only","Nothing to set up for this choice. The next two screens secure the access point and join your WiFi.",
    [note("","Run Quick Setup again from the sidebar whenever you want to add Home Assistant or an MQTT broker.")],
    [back("use"),grow(),btn("Continue",()=>go("ap"),"pri")]);
  if(W.use==="mqtt"){
    if(!D.mqtt){D.mqtt=strip(await tryGet("/api/settings/mqtt_manager"));
      if(!W.mqtt.url&&D.mqtt.url)W.mqtt.url=D.mqtt.url;
      if(!W.mqtt.user&&D.mqtt.username)W.mqtt.user=D.mqtt.username;
      if(!W.mqtt.prefix&&D.mqtt.topic_prefix)W.mqtt.prefix=D.mqtt.topic_prefix;}
    if(!D.dest){D.dest=strip(await tryGet("/api/settings/data_destinations"));
      const r=(D.dest.destinations||[]).find(d=>d.type==="mqtt"&&String(d.url||"").replace(/\s/g,"")==="~/autopid");
      if(r&&r.period_s)W.mqtt.period=r.period_s;}
    const hadPw=!!D.mqtt.enabled;
    const next=btn("Continue",()=>go("ap"),"pri",{disabled:!W.mqtt.url.trim()});
    const inp=(id,key,attrs={})=>h("input",{id,value:W.mqtt[key]||"",...attrs,oninput:e=>{W.mqtt[key]=key==="period"?Math.max(1,Math.min(3600,Number(e.target.value)||5)):e.target.value;next.disabled=!W.mqtt.url.trim();}});
    return screen("Your MQTT broker","WiCAN connects to the broker once it is on your WiFi. The connection is tested after the restart.",
      [row("Broker address",inp("qs-mq-url","url",{placeholder:"mqtt://broker.local:1883"}),"mqtt:// for plain, mqtts:// for TLS (public certificates work out of the box)."),
       row("Username",inp("qs-mq-user","user",{placeholder:"optional"})),
       row("Password",inp("qs-mq-pw","pw",{type:"password",placeholder:hadPw?"unchanged, type to replace":"optional",autocomplete:"new-password"})),
       row("Topic prefix",inp("qs-mq-prefix","prefix",{placeholder:"wican/"+(deviceId()||"<device id>")+" (default)"}),
         ["WiCAN publishes ",h("code",{},"<prefix>/status")," (online or offline) and the live vehicle data on ",h("code",{},"<prefix>/autopid"),"."]),
       row("Send vehicle data every",h("div",{class:"rowflex"},inp("qs-mq-period","period",{type:"number",min:1,max:3600,style:"max-width:110px"}),h("span",{},"seconds")),"Retained, so a dashboard that connects later still sees the last values.")],
      [back("use"),grow(),next]);
  }
  /* Home Assistant */
  const id=deviceId()||"<device id>";
  const copyBtn=btn("Copy repository link",()=>copyText(HA_REPO,"Repository link copied"),"sm",{icon:"copy"});
  return screen("Install the WiCAN integration in Home Assistant","Do this part in Home Assistant, on any device, before or after WiCAN joins your WiFi. Nothing to type on WiCAN itself.",
    [h("label",{class:"qs-agree"},h("input",{type:"checkbox",id:"qs-ha-installed",checked:W.haInstalled,onchange:e=>{W.haInstalled=e.target.checked;}}),
       h("div",{},h("b",{},"The WiCAN integration is already installed in my Home Assistant"),h("span",{},"Then only step 5 applies."))),
     steps([
       ["Install HACS if you do not have it",["HACS is the community store for Home Assistant. Guide: ",h("code",{},"hacs.xyz/docs/use"),"."]],
       ["Add the WiCAN repository to HACS",["HACS > three-dot menu > Custom repositories. Repository ",h("code",{},HA_REPO),", type ",h("b",{},"Integration"),", Add."]],
       ["Download the integration",["Search HACS for ",h("b",{},"WiCAN"),", open it, press Download."]],
       ["Restart Home Assistant","Settings > System > Restart. Home Assistant asks for this after a download."],
       ["Add WiCAN when it is discovered",["After WiCAN joins your WiFi (next screens), Settings > Devices & services shows ",h("b",{},"Discovered: WiCAN "+id),". Press Add. If it does not appear, press Add Integration, search WiCAN and enter WiCAN's address on your network: the Checks screen shows it."]]]),
     banner("info","info","Home Assistant then registers itself with WiCAN and receives status and vehicle data every 60 s, and right away when a value changes. Both must be on the same network: discovery uses mDNS, which home routers pass and guest networks do not."),
     h("div",{class:"qs-inline"},copyBtn,h("span",{},"The checks screen after the restart shows when Home Assistant has connected."))],
    [back("use"),grow(),btn("Continue",()=>go("ap"),"pri")]);
};

SCREENS.ap=()=>{
  const fresh=apDefaultPassword();
  const valid=pw=>pw.length>=8&&pw.length<=63&&pw!=="@meatpi#";
  const next=btn("Continue",()=>go("wifi"),"pri");
  const meter=h("div",{class:"qs-strength"},h("i",{}));
  const help1=h("div",{class:"help"}),help2=h("div",{class:"help err"});
  const type=()=>W.showPw?"text":"password";
  const pw=h("input",{id:"qs-ap-pw",type:type(),value:W.apPw,autocomplete:"new-password",placeholder:"8 to 63 characters"});
  const pw2=h("input",{id:"qs-ap-pw2",type:type(),value:W.apPw2,autocomplete:"new-password"});
  const show=btn(W.showPw?"Hide":"Show",()=>{W.showPw=!W.showPw;pw.type=pw2.type=type();show.textContent=W.showPw?"Hide":"Show";},"sm");
  const update=()=>{
    const v=W.apPw,ok=valid(v),match=ok&&v===W.apPw2;
    const score=Math.min(100,Math.round(v.length*6+(/[0-9]/.test(v)?12:0)+(/[^A-Za-z0-9]/.test(v)?14:0)+(/[A-Z]/.test(v)?10:0)));
    meter.className="qs-strength"+(score>60?" good":"");meter.firstChild.style.width=(v?Math.max(8,score):0)+"%";
    help1.textContent=v&&!ok?(v==="@meatpi#"?"That is the factory password.":"Use 8 to 63 characters."):"A phrase of three or four words is easy to remember and hard to guess.";
    help1.className="help"+(v&&!ok?" err":"");
    help2.textContent=W.apPw2&&!match?"The two passwords differ.":"";
    /* a device that already has its own password may keep it (both blank) */
    next.disabled=fresh?!match:!(match||(!v&&!W.apPw2));};
  pw.oninput=e=>{W.apPw=e.target.value;update();};pw2.oninput=e=>{W.apPw2=e.target.value;update();};
  update();
  return screen("Secure the access point",fresh?"The access point is how you are connected right now. Give it a password only you know.":"The access point already has your own password. Leave both fields blank to keep it, or set a new one.",
    [fresh?banner("warn","lock",h("b",{},apName()+" still has the factory password (@meatpi#)."),
        " Everyone who knows WiCAN knows it. WiCAN will not save WiFi settings until it is replaced.")
      :banner("ok","lock",h("b",{},apName()+" uses your own password."),"Nothing to do here unless you want a new one."),
     h("div",{class:"qs-row"},h("label",{for:"qs-ap-pw"},"New password"),h("div",{class:"ctl"},pw,show),h("div",{class:"help"},meter,help1)),
     h("div",{class:"qs-row"},h("label",{for:"qs-ap-pw2"},"Confirm password"),h("div",{class:"ctl"},pw2),help2),
     note("",["Your phone will ask for this new password the next time it joins ",h("b",{},apName()),". Write it down now."])],
    [back(W.use==="wifi"?"use":"details"),grow(),next]);
};

SCREENS.wifi=async()=>{
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  if(!W.ssid&&D.wifi.sta_ssid)W.ssid=D.wifi.sta_ssid;
  if(!W.modeSeeded){W.modeSeeded=true;if(!apDefaultPassword()&&D.wifi.sta_ssid&&D.wifi.mode==="apsta")W.wifiMode="apsta";}
  const stored=D.wifi.sta_ssid||"";
  const list=h("div",{class:"qs-nets"});
  const count=chip("…","");
  const openWarn=h("div",{});
  const pwInp=h("input",{id:"qs-wifi-pw",type:"password",value:W.wifiPw,autocomplete:"off"});
  const pwLabel=h("label",{for:"qs-wifi-pw"},"Password");
  const pwHelp=h("div",{class:"help"});
  const testBox=h("div",{});
  /* no home WiFi (2026-10-13): a dongle with a SIM on the cable is an internet
     of its own; the offer shows while the connector says so */
  const nlOffer=h("div",{});
  const paintOffer=L=>{
    const e=L&&L.e,d=(e&&e.dongle)||{};
    const ok=L&&L.kind==="espnetlink"&&d.valid&&d.sim!=="missing";
    if(!ok){nlOffer.replaceChildren();return;}
    const net=d.operator?d.operator+(d.network_type?" ("+d.network_type+")":""):"";
    if(nlOffer.childElementCount)return;
    nlOffer.replaceChildren(h("div",{class:"qs-nlcard qs-offer"},check("","signal","No home WiFi here? Use the ESPNetLink.",
      ["WiCAN found an ESPNetLink with a SIM on the cable"+(net?" ("+net+")":"")+". Skip this step: WiCAN gets its internet over LTE wherever the car is, and this phone keeps reaching it on ",h("code",{},apName()),"."],
      btn("Use the ESPNetLink, no home WiFi",()=>{W.noWifi=true;W.ssid="";W.wifiPw="";W.tested=null;W.wifiMode="apsta";W.apAuto=false;go("usb");},"sm",{icon:"signal"}))));
  };
  usbLive().then(paintOffer);every(3000,()=>usbLive().then(paintOffer));
  /* after it joins (Ali, 2026-10-07): Station only, recommended, or Access
     point + Station with the access point off while WiCAN is on the network
     (wifi_manager's ap_auto_disable, back whenever the station is lost). A
     station that cannot join is never a lockout: a 5 s press of the button
     brings the configured access point up for 10 minutes (button_manager's
     config mode), so the choice is free even without a passed test. */
  const modeBox=h("div",{});
  const paintMode=()=>{
    const net=W.ssid||"your WiFi";
    modeBox.replaceChildren(sub("After it joins"),h("div",{class:"qs-radios",style:"gap:8px"},
      radio("qs-mode","qs-mode-sta",W.wifiMode==="sta","Station only (recommended)",
        "WiCAN's own access point stays off; you reach WiCAN at its address on "+net+". If it ever cannot join, hold its button for 5 seconds: the access point comes up for 10 minutes with your password.",
        ()=>{W.wifiMode="sta";paintMode();}),
      radio("qs-mode","qs-mode-apsta",W.wifiMode==="apsta","Access point + Station","WiCAN keeps its own network as a second way in.",()=>{W.wifiMode="apsta";paintMode();}),
      W.wifiMode==="apsta"?h("label",{class:"qs-agree",style:"margin-left:28px;padding:8px 12px"},
        h("input",{type:"checkbox",id:"qs-ap-auto",checked:W.apAuto,onchange:e=>{W.apAuto=e.target.checked;}}),
        h("div",{},h("b",{},"Turn the access point off while WiCAN is on "+net+" (recommended)"),h("span",{},"It comes back by itself whenever "+net+" is lost."))):null));
  };
  /* Continue tests first (2026-10-06, Ali: "test the station connect before
     storing and rebooting"): WiCAN joins the network with these credentials
     while the phone is still on the access point, reports, lets go. A pass
     for these very credentials is remembered, so Back and forth does not
     retest; a blank password that keeps the stored one cannot be tested. */
  const passed=()=>!!(W.tested&&W.tested.ssid===W.ssid&&W.tested.pw===W.wifiPw);
  /* the phone's mobile data, asked once per visit at the moment it matters
     (Ali, 2026-10-08, after the tick on his phone: "a pop up when the user
     clicks Test and continue: make sure your mobile data is off, and a
     continue button"); over the access point only, where the first screen
     said it in full. The press is the user's: Continue starts the test. */
  const askMobileData=()=>{
    modal({title:null,
      body:h("div",{class:"qs-pop"},h("span",{class:"ci info"},ic("phone")),
        h("div",{},h("h3",{},"Make sure mobile data is off on this phone"),
          h("p",{},"The test moves WiCAN's radio to "+(W.ssid||"the network")+"'s channel: your phone drops off "+apName()+" for a few seconds and rejoins by itself, unless mobile data (or another saved network) pulls it away."),
          h("p",{class:"help"},"Settings > Mobile data (Cellular on some phones). Nothing to do on a computer."))),
      actions:[{label:"Continue",kind:"pri",fn:()=>{W.mdataOk=true;testWifi();}}]});
    const m=document.querySelector("#modal-root .modal");if(m)m.classList.add("qs-pop-modal");
  };
  const next=btn("Test and continue",()=>{W.noWifi=false;if(passed()||(W.ssid===stored&&!W.wifiPw))go("usb");else if(onAp()&&!W.mdataOk)askMobileData();else testWifi();},"pri",{icon:"wifi"});
  const selNet=()=>(W.nets||[]).find(n=>n.ssid===W.ssid);
  const update=()=>{
    const n=selNet();const open=n?isOpen(n):false;W.wifiAuth=n?netAuth(n):"";
    pwLabel.textContent="Password for "+(W.ssid||"the network");
    pwInp.disabled=open;pwInp.classList.remove("qs-bad");
    const keep=W.ssid&&W.ssid===stored&&!W.wifiPw;
    pwHelp.replaceChildren(keep?"Blank keeps the password already stored for "+stored+". WiCAN treats this network as trusted: you can manage it from any device on it."
      :"WiCAN treats this network as trusted: you can manage it from any device on it.");
    openWarn.replaceChildren(open?banner("crit","alert",h("b",{},W.ssid+" has no password."),
      " Quick Setup does not join open networks: anyone nearby could reach WiCAN. Pick a protected network."):null);
    next.disabled=!W.ssid||open||!(W.wifiPw.length>=8||keep);
    next.replaceChildren(ic(passed()||keep?"check":"wifi"),passed()||keep?"Continue":"Test and continue");
    list.querySelectorAll(".qs-net").forEach(el=>el.classList.toggle("sel",el.dataset.ssid===W.ssid));
    paintMode();};
  pwInp.oninput=e=>{W.wifiPw=e.target.value;update();};
  const anyway=()=>btn("Continue anyway",()=>{W.tested=null;W.testNote="anyway";go("usb");},"sm gh");
  const failCard=(title,text)=>{
    W.tested=null;
    testBox.replaceChildren(check("warn","alert",title,text,anyway()));
    next.disabled=false;next.replaceChildren(ic("refresh"),"Test again");};
  async function testWifi(resume){
    const ssid=W.ssid,pw=W.wifiPw;
    if(!resume){W.tested=null;W.testNote=null;W.testing={ssid,pw,t0:Date.now()};}
    pwInp.classList.remove("qs-bad");
    next.disabled=true;next.replaceChildren(h("span",{class:"qs-spin",style:"width:14px;height:14px;border-width:2px"}),"Testing");
    const cw=conn.wifi||{};
    const overSta=!!(cw.sta_connected&&location.hostname!==(cw.ap_ip||"192.168.0.10"));
    testBox.replaceChildren(check("run",null,"Trying "+ssid,["Up to 20 seconds. WiCAN joins ",h("b",{},ssid)," with this password and lets go again; nothing is saved. ",
      overSta?"WiCAN leaves its current network for the test: this page loses it for a moment and reconnects after.":"Your phone may drop off "+apName()+" for a few seconds while WiCAN's radio changes channel, and rejoins by itself: stay on this screen, it keeps asking."]));
    if(!resume){
      const ac=new AbortController();const tmo=setTimeout(()=>ac.abort(),4000);
      /* the scan row's channel goes with the request (2026-10-08): the
         station scans that one channel instead of all thirteen, during
         which the access point cannot beacon and the phone on it gives
         up; a name typed by hand has none */
      const sn=selNet();
      try{await api("/api/wifi/try",{method:"POST",body:{ssid,password:pw,channel:(sn&&sn.channel)||0},signal:ac.signal});}
      catch(e){
        const m=e.message||"";
        if(/station is off/i.test(m)){W.testing=null;W.testNote="ap";go("usb");return;}   /* AP only until the restart: nothing to test with */
        /* the answer was lost with the link (the station the page rides drops for the test, the AP blinks), or a test is already under way: the poll below tells */
        const lost=e.name==="AbortError"||(e instanceof TypeError)||/failed to fetch|networkerror|load failed/i.test(m);
        if(!lost&&!/already running/i.test(m)){W.testing=null;failCard("The test could not start",m);return;}
      }
      finally{clearTimeout(tmo);}}
    const t0=(W.testing&&W.testing.t0)||Date.now();let st=null;
    /* a resumed test starts while the rebuilt screen is not in the DOM yet
       (paint attaches it after this function has run): give it a moment,
       or the loop below never runs and nobody acts on the verdict (found
       2026-10-07 when the USB step shifted the probe's timing by a few ms) */
    for(let i=0;i<40&&!testBox.isConnected;i++)await new Promise(r=>setTimeout(r,50));
    while(Date.now()-t0<30000&&testBox.isConnected){
      await new Promise(r=>setTimeout(r,1000));
      const s=await tryGetTimed("/api/wifi/try",3000);
      if(s&&s.state==="done"&&s.ssid===ssid){st=s;break;}
      if(s&&s.state==="idle"){st=s;break;}}
    if(!testBox.isConnected)return;   /* the screen was rebuilt: its successor carries on */
    W.testing=null;
    if(ssid!==W.ssid||pw!==W.wifiPw){update();testBox.replaceChildren();return;}   /* edited meanwhile */
    if(!st||st.state!=="done"){failCard("Lost contact with WiCAN during the test",["If this phone dropped off ",h("code",{},apName())," and did not come back, join it again (mobile data off), then test again."]);return;}
    const r=st.result,reason=st.reason?" (reason "+st.reason+")":"";
    if(r==="connected"){
      W.tested={ssid,pw,ip:st.ip||"",rssi:st.rssi,took_ms:st.took_ms,channel:st.channel};W.testNote=null;
      go("usb");return;}
    if(r==="password"){pwInp.classList.add("qs-bad");pwInp.focus();
      failCard(ssid+" did not accept the password",["WiCAN found the network, the handshake failed",reason,". Nine times out of ten a typo: fix it and test again. Continue anyway keeps this password; afterwards you would fix it from the access point (Station only: hold WiCAN's button for 5 seconds to bring it up)."]);return;}
    if(r==="not_found"){failCard(ssid+" was not found",["WiCAN cannot see this network from where it is",reason,": out of range, 5 GHz only, or a hidden name typed differently (case matters). Move WiCAN closer or check the name, then test again."]);return;}
    if(r==="no_ip"){failCard("Joined "+ssid+", but got no address",["The router accepted WiCAN and then gave it no IP address within 8 s. A full DHCP table or a MAC filter does this. Check the router, then test again."]);return;}
    if(r==="timeout"){failCard("No answer from "+ssid+" in 20 s",["WiCAN could not finish joining in time. Move it closer to the router, or test again."]);return;}
    failCard(ssid+" refused the connection",["The router dropped WiCAN before it had an address",reason,". A MAC filter, a full router, or a band WiCAN cannot use. Check the router, then test again."]);
  }
  const render=()=>{
    const scanning=W.nets===null;
    const nets=W.nets||[];
    count.replaceChildren(h("span",{class:"d"}),scanning?"Scanning":W.scanErr?"Scan failed":nets.length+" network"+(nets.length===1?"":"s")+" found");
    count.className="chip "+(scanning?"":W.scanErr?"crit":nets.length?"ok":"");
    list.replaceChildren(...(scanning?[h("div",{class:"empty"},h("span",{class:"qs-spin",style:"display:inline-block;vertical-align:middle;margin-right:8px"}),"Looking for networks nearby (a few seconds)")]
      :nets.length?nets.map(n=>h("div",{class:"qs-net"+(n.ssid===W.ssid?" sel":""),"data-ssid":n.ssid,role:"button",tabindex:"0",
        onclick:()=>{W.ssid=n.ssid;W.manual=false;update();},onkeydown:e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();W.ssid=n.ssid;update();}}},
        h("span",{class:"ssid",title:n.ssid},n.ssid),h("span",{class:"bars"},bars(n.rssi)),
        isOpen(n)?chip("Open","warn"):chip(netAuth(n).includes("WPA3")?"WPA2/3":"WPA2","")))
      :[h("div",{class:"empty"},W.scanErr?"Scan failed: "+W.scanErr:"No networks found. Scan again in a few seconds, or enter the name by hand below.")]));
    update();};
  /* Scanning from the access point flips the radio to AP+STA for the
     duration and can drop this very connection for a moment (bench
     2026-10-01: one answer in three came back empty or not at all): retry
     twice, quietly, before showing an empty list or an error. */
  const scan=async(attempt=0)=>{
    rescan.disabled=true;rescan.replaceChildren(h("span",{class:"qs-spin",style:"width:14px;height:14px;border-width:2px"}),attempt?"Scanning again":"Scanning");
    let nets=null,err=null;
    try{const r=await api("/api/wifi/scan");const raw=(r&&r.networks)||[];
      /* one row per name, strongest signal wins (mesh systems repeat names) */
      const best=new Map();for(const n of raw){if(!n.ssid)continue;const o=best.get(n.ssid);if(!o||(n.rssi||-999)>(o.rssi||-999))best.set(n.ssid,n);}
      nets=[...best.values()].sort((a,b)=>(b.rssi||-999)-(a.rssi||-999));}
    catch(e){err=e.message;}
    if((err||!nets.length)&&attempt<2&&list.isConnected){await new Promise(r=>setTimeout(r,2500));return scan(attempt+1);}
    W.nets=nets||[];W.scanErr=err;
    rescan.disabled=false;rescan.replaceChildren(ic("refresh"),"Scan again");render();};
  const rescan=btn("Scan again",scan,"sm",{icon:"refresh"});
  const manual=h("input",{id:"qs-man-ssid",placeholder:"exact name, case matters",value:W.manual?W.ssid:"",oninput:e=>{W.ssid=e.target.value.trim();W.manual=true;update();}});
  const el=screen("Join your home WiFi","Pick the network your phone, Home Assistant or broker are on. WiCAN joins it after the restart.",
    [nlOffer,h("div",{class:"rowflex",style:"flex-wrap:wrap"},count,grow(),rescan),list,openWarn,
     h("details",{class:"qs-details",open:W.manual||null},h("summary",{},"Network not listed? Enter it by hand"),
       row("Network name (SSID)",manual,"Hidden networks and 5 GHz-only names do not appear in the scan. WiCAN connects on 2.4 GHz.")),
     h("div",{class:"qs-row"},pwLabel,h("div",{class:"ctl"},pwInp),pwHelp),modeBox,testBox],
    [back("ap"),grow(),next]);
  if(W.nets)render();else{W.nets=null;render();scan();}
  /* a test that was running when this screen was rebuilt (the AP's blink
     re-routes the page): carry on with it, within its own 30 s */
  if(W.testing&&W.testing.ssid===W.ssid&&W.testing.pw===W.wifiPw&&Date.now()-W.testing.t0<30000)testWifi(true);
  else W.testing=null;
  return el;
};


/* ---- the USB connector (2026-10-07, Ali: "quick setup for usb devices such as
   espnetlink, usb to ethernet and gps"). The host side is on by default, so
   whatever is plugged in is enumerated live, before the restart: the step
   watches /api/usb, /api/espnetlink, /api/usb/acm and /api/gps, names what it
   found and asks the one question each device has. Nothing plugged in is a
   plain Continue. The screens after the restart read the same routes, so a
   page that lost its state at the new address still knows. ---- */
const USB_DRV={asix:"ASIX",rtl8152:"Realtek RTL8152",cdc_ecm:"CDC-ECM",cdc_ncm:"CDC-NCM",rndis:"RNDIS"};
const USB_KINDS=[
  ["espnetlink","signal","ESPNetLink","Internet over LTE and GPS in the car. Pairs with WiCAN by itself over the cable."],
  ["eth","route","USB Ethernet adapter","A wired network: ASIX, Realtek RTL8152 or a CDC class adapter. Gets its address from the router."],
  ["gps","navigation","USB GPS receiver","A u-blox or other NMEA receiver. Position and speed join the vehicle data."]];
/* what the four routes say is on the connector. `device` on /api/usb is the
   enumerated device whatever its class; `vid`/`pid` only know the one behind
   an Ethernet driver (older firmware) */
function usbKind(u,e,acm,g){
  u=u||{};const eu=(e&&e.usb)||{},dev=u.device||{};
  const vid=String(dev.vid||u.vid||"").toLowerCase(),pid=String(dev.pid||u.pid||"").toLowerCase();
  const known=vid&&vid!=="0000";
  const isNl=vid==="303a"&&pid==="4007";
  const st=eu.pair_state||"idle",nlBusy=st!=="idle"&&st!=="foreign";   /* foreign = the link task looked and it was not a dongle */
  if(u.eth_connected&&known&&!isNl)return "eth";
  if(e&&(isNl||eu.attached||nlBusy))return "espnetlink";
  if(u.eth_connected)return "eth";
  if(vid==="1546"||/gps|gnss|nmea/i.test(dev.product||"")||(acm&&acm.connected&&g&&g.valid))return "gps";
  if(u.device_present&&USB_DRV[u.driver])return "eth";   /* an adapter without a cable in it */
  if(u.device_present)return "unknown";
  return "none";
}
async function usbLive(){
  const[u,e,acm,g]=await Promise.all([tryGet("/api/usb"),tryGet("/api/espnetlink"),tryGet("/api/usb/acm"),tryGet("/api/gps")]);
  return {u:u||{},e,acm,g,kind:usbKind(u,e,acm,g)};
}
const nlUsbMode=m=>m==="usb_ncm"||m==="usb_rndis";
const sigWord=r=>r>=-65?"good":r>=-80?"fair":r>=-95?"weak":"very weak";
/* the dongle's basics, one row each: SIM, network, signal, internet, GPS, APN.
   `dongle` is WiCAN's last health reading of it (over the cable during pairing,
   then over whichever link is up); nothing until there is one */
function nlRows(e){
  const d=e.dongle||{};if(!d.valid)return null;
  const sim=d.sim||(d.attached||d.lte_connected?"ready":"unknown");
  const rows=[];
  rows.push(["SIM",sim==="ready"?[chip("Found","ok")]:sim==="missing"?[chip("Not found","crit"),h("span",{class:"d2"},"Insert a SIM, then plug the dongle in again.")]
    :sim==="pin"?[chip("PIN locked","warn"),h("span",{class:"d2"},"Enter the SIM's PIN under SIM PIN, below.")]
    :sim==="pin_wrong"?[chip("PIN rejected","crit"),h("span",{class:"d2"},"The SIM refused the PIN. Check it below; nothing is retried by itself, a third wrong try locks the SIM.")]:[h("span",{class:"d2"},"Not read yet")]]);
  if(sim!=="missing"){
    rows.push(["Network",d.operator?[h("span",{},d.operator+(d.network_type?" ("+d.network_type+")":""))]:[chip(d.attached?"Attached":"Searching","warn")]]);
    rows.push(["Signal",d.rssi_dbm?[h("span",{class:"bars"},bars(d.rssi_dbm)),h("span",{},d.rssi_dbm+" dBm, "+sigWord(d.rssi_dbm)),d.rssi_dbm<-80?h("span",{class:"d2"},"A window or an external antenna helps."):null]:[h("span",{class:"d2"},"No reading yet")]]);
    rows.push(["Internet",d.lte_connected?[chip("Connected","ok"),d.ip?h("code",{},d.ip):null]:[chip(d.stage==="connecting"||d.attached?"Connecting":"Not connected","warn")]]);}
  const sats=(e.gps||{}).satellites;
  const nf=(e.gps_hint&&e.gps_hint.no_fix_s)||0;
  rows.push(["GPS",d.gps_fix?[chip("Fix","ok"),sats?h("span",{},sats+" satellites"):null]:nf>=600?[chip("No fix after "+Math.round(nf/60)+" min","warn"),h("span",{class:"d2"},"On USB Ethernet the cable can disturb the reception: switch to WiFi modem, below.")]:[chip("No fix yet",""),h("span",{class:"d2"},"Needs a view of the sky."+(nlUsbMode(e.mode)?" If it never gets one on USB Ethernet, switch to WiFi modem.":""))]]);
  if(e.apn)rows.push(["APN",[h("code",{},e.apn)]]);
  return h("dl",{class:"qs-kv qs-nlrows"},...rows.flatMap(([k,v])=>[h("dt",{},k),h("dd",{},...v.filter(Boolean))]));
}
/* the Checks card on the no-home-WiFi path (2026-10-13): the dongle is the internet */
const nlNetCard=L=>{const e=(L&&L.e)||{},d=e.dongle||{};
  const net=[d.operator||"",d.network_type||"",d.rssi_dbm?d.rssi_dbm+" dBm, "+sigWord(d.rssi_dbm):""].filter(Boolean).join(", ");
  return d.lte_connected?check("ok","signal","Internet via ESPNetLink: connected",[net+". Address ",h("code",{},d.ip||""),". "+(d.gps_fix?"GPS fix.":"GPS: no fix yet, it needs a view of the sky.")],chip("Connected","ok"))
    :check("warn","signal","Internet via ESPNetLink: not connected yet",[d.sim==="missing"?"No SIM in the dongle.":d.sim==="pin"?"The SIM asks for its PIN: enter it below.":d.sim==="pin_wrong"?"The SIM refused the PIN: check it below.":"The dongle is still attaching to the network (usually under a minute). If it stays this way, the APN from your SIM provider is the first thing to check: Settings > USB > ESPNetLink."],chip("Connecting","warn"));};
/* one reading of the connector for the step's card, the Checks card and the
   Done row: state ok / warn / run / pend (settled after the restart) */
function usbSummary(L){
  const{u,e,acm,g,kind}=L;
  if(kind==="espnetlink"){
    const eu=e.usb||{},st=eu.pair_state||"idle",d=e.dongle||{};
    const fw=e.dongle_fw?" (firmware "+e.dongle_fw+")":"";
    const old=!!(e.dongle_api&&e.dongle_api_min&&e.dongle_api<e.dongle_api_min);
    if(st==="unsupported"||old)
      return {state:"warn",icon:"signal",title:"ESPNetLink needs a firmware update",text:["This WiCAN expects the dongle's API "+e.dongle_api_min+", the dongle has "+(e.dongle_api||"an older one")+fw+". Update the dongle, then plug it in again."],chip:["Update needed","warn"]};
    const noSim=d.valid&&d.sim==="missing";
    if(e.pair_blocked_factory_pw||st==="hold")
      return {state:noSim?"warn":"pend",icon:"signal",title:noSim?"ESPNetLink found, no SIM in it":"ESPNetLink found, pairs after the restart",
        text:[noSim?"The dongle"+fw+" is on the cable but has no SIM: no internet over LTE until one is in. ":"The dongle"+fw+" is on the cable. ","Pairing waits for the access point password you set"+(apDefaultPassword()?" on the Access point step":"")+": it completes by itself after the restart, and WiCAN restarts once more to store the dongle's key."],
        chip:noSim?["No SIM","warn"]:["Pairs after the restart",""],rows:nlRows(e)};
    if(st==="done"||st==="ncm_up"||e.paired){
      const onIt=e.on_link||e.uplink==="espnetlink"||e.uplink==="espnetlink_usb";
      const usbUp=e.uplink==="espnetlink_usb"||(nlUsbMode(e.mode)&&onIt);
      const text=onIt?["WiCAN is on the dongle "+(usbUp?"over the cable":"over its WiFi ")+(usbUp?"":""),!usbUp&&e.ssid?h("code",{},e.ssid):null,"."]
        :["Paired as "+(nlUsbMode(e.mode)?"USB Ethernet":"WiFi modem")+". WiCAN uses it whenever your home WiFi is out of range, in the car: internet over LTE, and GPS. Right now WiCAN is on your home WiFi, so the dongle stands by."];
      return {state:noSim?"warn":"ok",icon:"signal",title:noSim?"ESPNetLink found, no SIM in it":(usbUp?"ESPNetLink connected":"ESPNetLink paired"),text,chip:noSim?["No SIM","warn"]:[onIt?"In use":"Standing by","ok"],rows:nlRows(e)};}
    return {state:"run",icon:"signal",title:"Pairing with the ESPNetLink",text:["WiCAN reads the dongle's key over the cable"+(nlUsbMode(e.mode)?"":", then leaves the cable as power only")+". When the key is new WiCAN restarts once more by itself; this page waits."],chip:["Pairing","warn"],rows:nlRows(e)};
  }
  if(kind==="eth"){
    const drv=USB_DRV[u.driver]||u.driver||((u.device||{}).product)||"adapter";
    const first=W.usb.ethFirst!==null?W.usb.ethFirst:!!(D.usbhost&&D.usbhost.prefer_usb_route);
    if(u.eth_connected&&u.ip)return {state:"ok",icon:"route",title:"USB Ethernet connected",text:["The "+drv+" adapter has the address ",h("code",{},u.ip),". "+(first?"The cable carries WiCAN's traffic whenever it has an address.":"WiFi first: the cable carries WiCAN's traffic while WiFi is not connected.")],chip:["Connected","ok"]};
    return {state:"run",icon:"route",title:"USB Ethernet adapter found, no link yet",text:["The "+drv+" adapter is on the connector. Plug the network cable in: WiCAN asks the router for an address."],chip:["No link","warn"]};
  }
  if(kind==="gps"){
    const name=((u.device||{}).product)||"The receiver";
    if(g&&g.valid)return {state:"ok",icon:"navigation",title:"GPS fix",text:[name+": "+g.satellites+" satellites"+(g.accuracy?", about "+g.accuracy+" m":"")+". Position, speed, heading and altitude ride along with the vehicle data as ",h("code",{},"gps_latitude"),", ",h("code",{},"gps_longitude"),", ",h("code",{},"gps_speed")," and friends."],chip:["Fix","ok"]};
    /* a receiver is read by default (issue #978, 2026-10-11); the one way it is not is
       the USB GPS receiver switch under USB > Settings, which the wizard does not offer */
    if(acm&&acm.gps===false)return {state:"warn",icon:"navigation",title:"USB GPS receiver found, switched off",text:[name+" is on the connector, but USB GPS receiver is off under USB > Settings, so WiCAN ignores it. Turn it on there after setup if you want its position."],chip:["Switched off","warn"]};
    return {state:"run",icon:"navigation",title:"GPS receiver found, waiting for a fix",text:[name+" is being read"+(acm&&acm.nmea_sentences?" ("+acm.nmea_sentences+" sentences so far)":"")+". It needs a view of the sky; a cold start takes up to a minute."],chip:["No fix yet","warn"]};
  }
  if(kind==="unknown"){const dev=u.device||{};
    return {state:"warn",icon:"plug",title:"A USB device WiCAN does not support",text:["Something is on the connector"+(dev.product?": "+dev.product:dev.vid?" ("+dev.vid+":"+dev.pid+")":"")+". WiCAN works with the ESPNetLink, USB Ethernet adapters (ASIX, Realtek RTL8152, CDC) and NMEA GPS receivers."],chip:["Not supported","warn"]};}
  return null;
}
/* the card: one check line, and the dongle's rows under it in the same box */
function usbCard(s,kind){
  const c=check(kind,s.icon,s.title,s.text,chip(s.chip[0],s.chip[1]));
  return s.rows?h("div",{class:"qs-nlcard"},c,s.rows):c;
}
/* the Review row: what the save does for the connector, if anything */
function usbReview(item,line){
  const L=W.usb.live,k=L&&L.kind;
  if(W.usb.hostFix)return [item("pend","USB connector","Host mode after the restart",null,line("Plug the device in afterwards","the Checks screen shows it"))];
  if(!k||k==="none"||k==="unknown")return [];
  if(k==="espnetlink"){const e=L.e||{},pst=(e.usb||{}).pair_state,paired=!!(e.paired||pst==="done"||pst==="ncm_up");
    const switching=nlUsbMode(e.mode)!==(W.usb.nlMode==="usb");
    const apn=W.usb.apn.trim(),apnNew=apn!==((D.espnl&&D.espnl.apn)||"")||!!W.usb.apnPw||W.usb.apnUser!==((D.espnl&&D.espnl.apn_user)||"")||!!W.usb.pin;
    return [item(paired&&!switching&&!apnNew?"ok":"pend","USB connector","ESPNetLink",line(...[W.usb.nlMode==="usb"?"USB Ethernet":"WiFi modem",apn?"APN "+apn:"the dongle's default APN"].filter(Boolean)),
      paired?(switching?line("Switches at the restart","the dongle reboots once to change its USB class"):pst==="ncm_up"?line("Connected over the cable","nothing to do at the restart"):line("Paired",e.ssid||""))
        :line("Pairs after the restart","WiCAN restarts once more by itself to store the dongle's key"),
      !paired&&switching?line("Then "+(W.usb.nlMode==="usb"?"USB Ethernet":"WiFi modem"),"the dongle reboots once to change its USB class"):null,
      apnNew?line("Carrier settings sent to the dongle","it restarts once to apply them"):null)];}
  if(k==="eth"){const u=L.u||{};
    return [item(u.eth_connected?"ok":"pend","USB connector","USB Ethernet adapter",line(USB_DRV[u.driver]||u.driver||"",W.usb.ethFirst?"cable first":"WiFi first"),u.ip?line("Address "+u.ip):line("No link yet","plug the network cable in"))];}
  if(k==="gps"){const g=L.g,dev=(L.u&&L.u.device)||{};
    const off=!!(L.acm&&L.acm.gps===false);
    return [item(g&&g.valid?"ok":off?"warn":"pend","USB connector","USB GPS receiver",dev.product?line(dev.product):null,g&&g.valid?line(g.satellites+" satellites","published with the vehicle data"):off?line("Switched off","USB > Settings"):line("Waiting for a fix","needs a view of the sky"))];}
  return [];
}

async function saveAlone(saveBtn){
  const L=W.usb.live;
  if(!L||L.kind!=="espnetlink"||!D.espnl){toast("No ESPNetLink on the cable: nothing to save","ok");return;}
  const nv=strip(D.espnl);const toUsb=W.usb.nlMode==="usb";
  const apn=W.usb.apn.trim(),apnUser=W.usb.apnUser.trim();
  const apnNew=apn!==(nv.apn||"")||apnUser!==(nv.apn_user||"")||!!W.usb.apnPw;
  const pinNew=!!W.usb.pin.trim();
  const modeNew=toUsb!==nlUsbMode(nv.mode);
  if(nv.enabled&&!modeNew&&!apnNew&&!pinNew){toast("Nothing to change: the dongle is set up","ok");return;}
  nv.enabled=true;
  if(modeNew)nv.mode=toUsb?"usb_rndis":"wifi_modem";
  if(apnNew){nv.apn=apn;nv.apn_user=apnUser;if(W.usb.apnPw)nv.apn_password=W.usb.apnPw;}
  if(pinNew)nv.sim_pin=W.usb.pin.trim();
  saveBtn.disabled=true;
  try{store.stage("espnetlink",nv);await store.commit("ESPNetLink: saving and restarting");D.espnl=nv;W.usb.pin="";W.usb.apnPw="";}
  catch(e){store.unstage("espnetlink");saveBtn.disabled=false;toast("WiCAN did not accept the settings: "+(e.message||e),"err");}
}

/* the ESPNetLink screen's APN section (Ali, 2026-10-28: a Simbase SIM left on
   the dongle's default APN never attached while the field sat folded under
   "optional"): the SIM PIN row when the SIM asks for one, the APN asked on its
   own with where the provider publishes it, a live verdict under the field from
   the dongle's health (refreshed with the connector poll, the inputs untouched),
   the username and password folded as rarely needed */
const APN_HELP="Every SIM provider has an APN (access point name) the dongle needs to get online. You find it with the SIM: on its packaging, in your account or on the provider's support pages under APN settings. Type it exactly as written; WiCAN sends it to the dongle, which restarts once to apply it.";
function apnVerdict(ed,e){
  if(!ed.valid||ed.sim==="missing")return null;
  if(ed.lte_connected)return [chip("Online","ok"),h("span",{},e.apn?" with APN "+e.apn+".":" on the dongle's own default APN: you can leave this blank.")];
  if(ed.sim==="pin"||ed.sim==="pin_wrong")return [chip("Waiting for the PIN","warn")];
  return [chip("Not online yet","warn"),h("span",{},(e.apn?" The dongle has APN "+e.apn+". ":" ")+"The APN is the usual reason: enter the one from your SIM provider.")];
}
function paintApnVerdict(ed,e){const el=document.getElementById("qs-apn-verdict");if(!el)return;const v=apnVerdict(ed,e);el.replaceChildren(...(v||[]));el.style.display=v?"":"none";}
function apnSection(ed,e,pinLocked,inp){
  return h("div",{class:"qs-apnsec"},
    ...(pinLocked?[sub("SIM PIN"),row("SIM PIN",inp("qs-pin","pin",{type:"password",maxlength:"8",inputmode:"numeric",placeholder:D.espnl.sim_pin?"stored, type to replace":"4 to 8 digits",autocomplete:"off"}),"WiCAN sends it to the dongle once. A wrong PIN is never tried again by itself: three wrong tries lock the SIM (the PUK is on the SIM's card).")]:[]),
    sub("Your SIM's APN"),h("p",{class:"help"},APN_HELP),
    row("APN",inp("qs-apn","apn",{placeholder:"as your SIM provider writes it",autocapitalize:"none",autocorrect:"off",spellcheck:"false"}),h("span",{id:"qs-apn-verdict",class:"qs-verdict"},...(apnVerdict(ed,e)||[]))),
    h("details",{class:"qs-details"},h("summary",{},"Username and password (only if your provider gave you them)"),
      row("Username",inp("qs-apn-user","apnUser",{placeholder:"optional"})),
      row("Password",inp("qs-apn-pw","apnPw",{type:"password",placeholder:D.espnl.apn_password?"unchanged, type to replace":"optional",autocomplete:"new-password"}))));
}
SCREENS.usb=async()=>{
  if(D.usbhost===undefined){const s=await tryGet("/api/settings/usb_host_manager");D.usbhost=s?strip(s):null;}
  if(D.espnl===undefined){const s=await tryGet("/api/settings/espnetlink");D.espnl=s?strip(s):null;}   /* null: not in this build */
  /* USB Ethernet is the recommendation (Ali, 2026-10-07); a dongle already
     paired keeps the mode it was paired with (the status says paired and the
     mode the device runs; seeded at the first reading, in refresh) */
  if(!W.usb.apnSeeded){W.usb.apnSeeded=true;if(D.espnl){W.usb.apn=D.espnl.apn||"";W.usb.apnUser=D.espnl.apn_user||"";}}
  if(W.usb.ethFirst===null)W.usb.ethFirst=!!(D.usbhost&&D.usbhost.prefer_usb_route);
  const hostOff=!!(D.usbhost&&(D.usbhost.enabled===false||D.usbhost.role==="device"));
  const tilesEl=h("div",{class:"qs-tiles"}),card=h("div",{}),choice=h("div",{});
  const paintTiles=kind=>{tilesEl.replaceChildren(...USB_KINDS.map(([id,icon,title,body])=>h("div",{class:"qs-tile static"+(kind===id?" sel":" later"),"data-usb":id},
    kind===id?h("span",{class:"tag chip ok"},"Found"):null,h("span",{class:"ticon"},ic(icon)),h("h3",{},title),h("p",{},body))));};
  const paintChoice=kind=>{
    if(kind==="espnetlink"&&D.espnl){
      const inp=(id,key,attrs={})=>h("input",{id,value:W.usb[key]||"",...attrs,oninput:e=>{W.usb[key]=e.target.value;}});
      const live=W.usb.live||{},ed=((live.e||{}).dongle)||{},nf=(((live.e||{}).gps_hint)||{}).no_fix_s||0;
      const pinLocked=ed.sim==="pin"||ed.sim==="pin_wrong";
      const nudgeCard=()=>(nf>=600&&W.usb.nlMode==="usb")?check("warn","navigation","No GPS fix after "+Math.round(nf/60)+" minutes on USB Ethernet",["The cable can disturb the dongle's GPS reception. WiFi modem gives it the best reception: the cable carries power only and WiCAN joins the dongle's own WiFi."+(W.alone?" WiCAN restarts once.":"")],btn("Switch to WiFi modem",()=>{W.usb.nlMode="wifi_modem";paintChoice(kind);},"sm")):null;
      choice.replaceChildren(...[nudgeCard()].filter(Boolean),sub("How WiCAN uses it"),h("div",{class:"qs-radios",style:"gap:8px"},
        radio("qs-nl","qs-nl-usb",W.usb.nlMode==="usb","USB Ethernet (recommended)","The dongle is a wired network adapter on the cable: always on, nothing to join, internet over LTE the moment the cable is in. If the GPS never gets a fix this way (the USB link can disturb its reception), switch to WiFi modem under Settings > USB.",()=>{W.usb.nlMode="usb";paintChoice(kind);}),
        radio("qs-nl","qs-nl-wifi",W.usb.nlMode==="wifi_modem","WiFi modem","The cable carries power only. WiCAN joins the dongle's own WiFi whenever your home WiFi is out of range. The GPS gets its best reception this way.",()=>{W.usb.nlMode="wifi_modem";paintChoice(kind);})),
        apnSection(ed,live.e||{},pinLocked,inp));
      return;}
    if(kind==="eth"){
      choice.replaceChildren(sub("Internet over the cable"),h("div",{class:"qs-radios",style:"gap:8px"},
        radio("qs-eth","qs-eth-wifi",!W.usb.ethFirst,"When WiFi is not connected (default)","WiFi first. The cable carries WiCAN's traffic while there is no WiFi.",()=>{W.usb.ethFirst=false;paintChoice(kind);}),
        radio("qs-eth","qs-eth-first",W.usb.ethFirst,"Always, even when WiFi is connected","The cable first. For a WiCAN that lives on a wired network.",()=>{W.usb.ethFirst=true;paintChoice(kind);})),
        h("p",{class:"help"},"A fixed address instead of one from the router: Settings > USB, after the setup."));
      return;}
    choice.replaceChildren();
  };
  const paintCard=L=>{
    if(hostOff){
      card.replaceChildren(check("warn","plug","The USB connector is in device mode","WiCAN presents itself to a PC as a network or serial device. Dongles, adapters and GPS receivers need the connector as a host.",null),
        h("label",{class:"qs-agree"},h("input",{type:"checkbox",id:"qs-usb-host",checked:W.usb.hostFix,onchange:e=>{W.usb.hostFix=e.target.checked;}}),
          h("div",{},h("b",{},"Switch the connector to host mode at the restart"),h("span",{},"Plug the device in afterwards: the Checks screen shows it."))));
      choice.replaceChildren();return;}
    const s=usbSummary(L);
    if(!s){card.replaceChildren(check("","plug","Nothing on the USB connector","Plug a device in now and this screen names it within a few seconds, or continue without one. Settings > USB covers all of this later.",chip("Empty","")));return;}
    card.replaceChildren(usbCard(s,s.state==="pend"?"":s.state));
  };
  const refresh=async()=>{
    const L=await usbLive();if(W.cur!=="usb")return;   /* the first paint runs before the screen is in the DOM */
    if(W.usb.nlMode===null){const e=L.e||{};W.usb.nlMode=(e.paired&&!nlUsbMode(e.mode))?"wifi_modem":"usb";}
    const prev=W.usb.kind;W.usb.live=L;W.usb.kind=L.kind;
    paintTiles(L.kind);paintCard(L);if(prev!==L.kind||!choice.childElementCount)paintChoice(L.kind);
    if(L.kind==="espnetlink")paintApnVerdict(((L.e||{}).dongle)||{},L.e||{});
  };
  await refresh();every(3000,refresh);
  const nwNote=W.noWifi?note("",["WiCAN keeps its own access point on, ",h("code",{},apName()),": with no home network it is the way to reach WiCAN. Internet comes over the dongle wherever the car is."]):null;
  const simNow=((((W.usb.live||{}).e||{}).dongle)||{}).sim,pinNow=simNow==="pin"||simNow==="pin_wrong";
  /* the screen on its own saves itself: the espnetlink object staged and
     committed like the wizard's save (reboot-to-apply); nothing changed =
     no write, no restart */
  const saveBtn=btn(pinNow?"Send PIN and save":"Save",()=>saveAlone(saveBtn),"pri",{icon:"check"});
  const foot=W.alone?[btn("Not now",()=>{location.hash="#/status";},"gh sm"),grow(),saveBtn]
    :[back("wifi"),grow(),btn("Continue",()=>go("review"),"pri")];
  return screen(W.alone?"On the cable now":W.noWifi?"Your ESPNetLink":"What is on the USB connector?",
    W.alone?"Read live, every 3 seconds.":W.noWifi?"It is WiCAN's internet: LTE wherever the car is, and GPS. One choice to make.":"WiCAN's USB port takes an ESPNetLink, a USB Ethernet adapter or a GPS receiver. Plug it in now: WiCAN names it here. Nothing to plug in? Just continue.",
    [W.alone?null:tilesEl,card,choice,nwNote],foot);
};

SCREENS.review=()=>{
  const err=h("div",{});
  const tested=(W.tested&&W.tested.ssid===W.ssid&&W.tested.pw===W.wifiPw&&W.tested.ip)?W.tested:null;
  const save=btn("Save and restart",()=>applyAll(save,err),"pri",{icon:"power"});
  /* a checklist: the state is the circle (settled by this save, a warning,
     or still to come after the restart), the label is the title, the value
     under it, the details as muted lines joined by middle dots */
  const line=(...parts)=>h("span",{class:"d2"},...parts.flatMap((p,i)=>i?[h("i",{},"\u00b7"),p]:[p]));
  /* circle | title | value (with its short attributes beside it) and the
     lines under it */
  const item=(state,title,value,beside,...lines)=>h("li",{},
    h("span",{class:"ci "+(state==="ok"?"":state)},state==="ok"?ic("check"):state==="warn"?ic("alert"):null),
    h("b",{},title),
    h("div",{class:"c"},h("span",{class:"v"},h("span",{},value),beside),...lines));
  const auth=W.wifiAuth&&W.wifiAuth!=="OPEN"?(W.wifiAuth.includes("WPA3")?"WPA2/3":"WPA2"):"protected";
  const kept=!W.wifiPw&&!!(D.wifi&&W.ssid===D.wifi.sta_ssid);
  const wifiState=tested||kept?"ok":W.testNote==="anyway"?"warn":"pend";
  const wifiVerdict=tested?line("Password accepted",tested.ip)
    :kept?line("Stored password kept")
    :W.testNote==="anyway"?line("Test failed","continuing anyway: the next screen says whether WiCAN joined")
    :line("Not tested","access point only until the restart");
  const useItem=W.use==="ha"?item("ok","Home Assistant","Automatic discovery",null,line("Home Assistant will register WiCAN after the restart."))
    :W.use==="mqtt"?item("ok","MQTT broker",W.mqtt.url.trim(),null,line("As "+(W.mqtt.user||"anonymous"),"vehicle data every "+W.mqtt.period+" s"))
    :item("pend","Home Assistant","Skipped for now",null,line("Run Quick Setup again to add one."));
  const sta=W.wifiMode==="sta",auto=!sta&&W.apAuto;
  const link=tested?"http://"+tested.ip+"/"+resumeHash():"";
  const kv=h("ul",{class:"qs-rv"},
    item("ok","Access point",apName(),line(W.apPw?"New password":"Password kept"),
      W.noWifi?line("Stays on","the way to reach WiCAN without a home network"):sta?line("Off after the restart","hold the button 5 s to bring it up when needed"):auto?line("Off while WiCAN is on "+W.ssid,"back whenever "+W.ssid+" is lost"):null),
    W.noWifi?item("ok","Home WiFi","None",line("Internet over the ESPNetLink"),line("Access point + Station","the access point stays on")):item(wifiState,"Home WiFi",W.ssid,line(auth,sta?"Station only":"Access point + Station"),wifiVerdict),
    useItem,
    ...usbReview(item,line),
    item("pend","Vehicle data","Profile, protocol and standard PIDs",null,line("Configured after the restart.")));
  const apLinkR="http://"+((D.wifi&&D.wifi.ap_ip)||"192.168.0.10")+"/"+resumeHash();
  const step2=W.noWifi?[W.apPw?"Your phone drops off "+apName():"Your phone may drop off "+apName()+" for a moment","Join it again with your new password: the access point is back within about 20 seconds."]:sta?["Your phone drops off "+apName()+" for good","The access point turns off; WiCAN lives on "+W.ssid+" from now on."]
    :auto?["Your phone drops off "+apName()+" once WiCAN is on "+W.ssid,"The access point turns off while WiCAN is on your network and comes back whenever "+W.ssid+" is lost."]
    :[W.apPw?"Your phone drops off "+apName():"Your phone may drop off "+apName()+" for a moment",W.apPw?"The access point comes back with the new password.":"The access point comes back with the same password."];
  const step3=W.noWifi?["Quick Setup continues here","Same address: "+apLinkR+". The next screen waits for WiCAN to come back."]:link?["Connect your phone to "+W.ssid+" and open "+link,"Quick Setup continues there; the next screen has the link and a Copy button."]
    :["Connect your phone to "+W.ssid+" and open WiCAN's address there","The next screen says how to find it."];
  return screen("Review, then restart","Everything below is saved together in one restart. Nothing has been written to WiCAN yet.",
    [kv,err,sub("What happens next"),
     steps([["WiCAN saves and restarts","About 15 seconds."],step2,step3])],
    [back("usb"),grow(),save]);
};

async function applyAll(saveBtn,errEl){
  saveBtn.disabled=true;errEl.replaceChildren();
  const staged=[];
  try{
    if(!D.wifi)D.wifi=strip(await api("/api/settings/wifi_manager"));
    const wf=strip(D.wifi);
    if(W.noWifi){
      /* no home WiFi (2026-10-13): the access point is the way in, so it
         stays on; the station holds no network (WiFi modem's fallback join
         still has it); auto-off would count the dongle's own WiFi as a
         network and switch the access point off */
      wf.mode="apsta";wf.ap_auto_disable=false;wf.sta_ssid="";}
    else{
      wf.mode=W.wifiMode==="sta"?"sta":"apsta";
      if(W.wifiMode==="apsta")wf.ap_auto_disable=!!W.apAuto;
      wf.sta_ssid=W.ssid;wf.sta_trusted=true;
      if(W.wifiPw)wf.sta_password=W.wifiPw;}
    if(W.apPw)wf.ap_password=W.apPw;
    store.stage("wifi_manager",wf);staged.push("wifi_manager");
    D.wifi=wf;   /* the screens after the restart read the mode from here until the reload */
    if(W.use==="mqtt"){
      const mv=strip(D.mqtt||await api("/api/settings/mqtt_manager"));
      mv.enabled=true;mv.url=W.mqtt.url.trim();mv.username=W.mqtt.user||"";mv.topic_prefix=(W.mqtt.prefix||"").trim();
      if(W.mqtt.pw)mv.broker_password=W.mqtt.pw;
      store.stage("mqtt_manager",mv);staged.push("mqtt_manager");
      const dd=strip(D.dest||await api("/api/settings/data_destinations"));
      dd.enabled=true;
      const list=(dd.destinations||[]).map(d=>({...d}));
      let r=list.find(d=>d.type==="mqtt"&&String(d.url||"").replace(/\s/g,"")==="~/autopid");
      if(r){r.enabled=true;r.period_s=W.mqtt.period;}
      else{
        if(list.length>=8)throw new Error("The data destinations table is full (8 rows): remove one under Automate > Data destinations first");
        const names=new Set(list.map(d=>d.name));let nm="autopid",k=2;while(names.has(nm))nm="autopid"+(k++);
        list.push({name:nm,type:"mqtt",enabled:true,url:"~/autopid",period_s:W.mqtt.period,auth:"none",auth_token:"",auth_name:"",basic_username:"",basic_password:"",api_key:"",query:"",cert_set:"",car_model:"",retain:true,full_first:true});}
      dd.destinations=list;
      store.stage("data_destinations",dd);staged.push("data_destinations");
    }
    /* the USB connector (2026-10-07): only what the step changed */
    const uk=W.usb.live?W.usb.live.kind:null;
    if(W.usb.hostFix&&D.usbhost){const uv=strip(D.usbhost);uv.enabled=true;uv.role="host";store.stage("usb_host_manager",uv);staged.push("usb_host_manager");D.usbhost=uv;}
    if(uk==="espnetlink"&&D.espnl){const nv=strip(D.espnl);const toUsb=W.usb.nlMode==="usb";
      const apn=W.usb.apn.trim(),apnUser=W.usb.apnUser.trim();
      const apnNew=apn!==(nv.apn||"")||apnUser!==(nv.apn_user||"")||!!W.usb.apnPw;
      const pinNew=!!W.usb.pin;   /* the SIM PIN (2026-10-13): write-only here */
      if(!nv.enabled||toUsb!==nlUsbMode(nv.mode)||apnNew||pinNew){
        nv.enabled=true;
        if(toUsb!==nlUsbMode(nv.mode))nv.mode=toUsb?"usb_rndis":"wifi_modem";   /* a dongle already on NCM keeps it */
        if(apnNew){nv.apn=apn;nv.apn_user=apnUser;if(W.usb.apnPw)nv.apn_password=W.usb.apnPw;}
        if(pinNew)nv.sim_pin=W.usb.pin.trim();
        store.stage("espnetlink",nv);staged.push("espnetlink");D.espnl=nv;}}
    if(uk==="eth"&&D.usbhost&&!W.usb.hostFix){const uv=strip(D.usbhost);
      if(!!uv.prefer_usb_route!==!!W.usb.ethFirst){uv.prefer_usb_route=!!W.usb.ethFirst;store.stage("usb_host_manager",uv);staged.push("usb_host_manager");D.usbhost=uv;}}
    /* a GPS receiver needs nothing staged: it is read by default (issue #978) */
    W.applied=true;W.cur="reconnect";
    try{history.replaceState(null,"","#/setup/reconnect");}catch(_){}
    paint();
    await store.commit("Quick Setup: saving and restarting");
  }catch(e){
    staged.forEach(c=>store.unstage(c));
    W.cur="review";try{history.replaceState(null,"","#/setup/review");}catch(_){}
    paint();
    /* the review screen is rebuilt: put the message where the user looks.
       A network-level failure (the AP drops its client for a few seconds
       around a scan) is not a refusal: say so and invite a second press */
    const lost=(e instanceof TypeError)||/failed to fetch|networkerror|load failed/i.test(e.message||"");
    setTimeout(()=>{const slot=scrEl.querySelector(".qs-body > div:empty");
      const b=lost?banner("warn","alert",h("b",{},"Lost contact with WiCAN for a moment. "),"Nothing was changed. Press Save and restart again.")
                  :banner("crit","alert",h("b",{},"WiCAN did not accept the settings: "),e.message||String(e));
      if(slot)slot.replaceWith(b);else scrEl.querySelector(".qs-body").prepend(b);},50);
  }
}

/* This page was loaded from the access point. When the AP password changes
   the phone drops off and joins its known home network by itself, and the
   AP address is then out of reach for good: the page's own connection state
   can never say "online" again (2026-10-06, Ali: stuck at "Restarting" until
   he typed the link by hand). So the screen watches two places: its own
   origin, for a phone or PC that stays on the AP, and the address link, for the
   phone that has moved. When only the link answers it says so and offers
   the way on as a button: the user leaves when it looks stable to them, and
   this screen (the instructions, the fallback) stays until they do. A jump
   on a timer could strand them on a browser error with no way back here
   (the origin is the AP), and an opaque answer cannot prove it is WiCAN
   (Ali, 2026-10-06). */
const RECONNECT_PATIENCE_S=40;   /* a restart (about 15 s) plus the join */
/* the no-home-WiFi path (2026-10-13): the phone stays on the access point;
   WiCAN restarts and the AP comes back with the new password */
async function reconnectAp(){
  const apLink="http://"+((D.wifi&&D.wifi.ap_ip)||"192.168.0.10")+"/"+resumeHash();
  const live=h("div",{});const t0=Date.now();const since=()=>Math.round((Date.now()-t0)/1000);
  const paintLive=()=>{
    if(conn.state==="online"&&since()>=12){live.replaceChildren(check("ok","wifi","WiCAN is back",["This phone is on ",h("code",{},apName())," again. Quick Setup continues."],btn("Continue",()=>go("checks"),"sm pri")));return;}
    if(since()<RECONNECT_PATIENCE_S)live.replaceChildren(check("run",null,"Restarting","About 15 s, then "+apName()+" is back with your new password. If this phone does not rejoin it by itself, join it from the WiFi list."));
    else live.replaceChildren(check("warn","alert","Still waiting for "+apName(),["Open the WiFi list on this phone and join ",h("code",{},apName())," with the password you set; a phone sometimes goes back to another saved network. Then open ",h("a",{href:apLink},apLink),"."]));
  };
  paintLive();every(3000,paintLive);
  return screen("Join WiCAN's network again",["WiCAN restarts and ",h("b",{},apName())," comes back with your new password. Reconnect this phone to it; this page continues by itself."],
    [h("div",{class:"qs-link"},h("span",{class:"url"},apLink)),live],
    [h("span",{class:"ghost"},"Stay on this screen until WiCAN is back.")]);
}
SCREENS.reconnect=async()=>{
  {const L0=W.usb.live||await usbLive();if(W.noWifi||(!W.ssid&&!(D.wifi&&D.wifi.sta_ssid)&&L0.kind==="espnetlink"))return reconnectAp();}
  const ssid=W.ssid||(D.wifi&&D.wifi.sta_ssid)||"your home WiFi";
  const sta=W.wifiMode==="sta";
  const apLink="http://"+((D.wifi&&D.wifi.ap_ip)||"192.168.0.10")+"/"+resumeHash();
  /* the address the network gave WiCAN: the connection test's (usually the
     same after the restart: same MAC, same lease), or the live station
     address when the device is on that network already (a re-run; fetched
     here when the page has none yet: a screen entered by its URL). No mDNS
     name (Ali, 2026-10-07: "it's simpler to just use IP"). */
  const cw=conn.wifi||(await tryGet("/api/wifi/status"))||{};
  const ip=(W.tested&&W.tested.ssid===ssid&&W.tested.ip)?W.tested.ip
    :(cw.sta_connected&&cw.ip&&(!W.ssid||W.ssid===(D.wifi&&D.wifi.sta_ssid)))?cw.ip:"";
  const origin=ip?"http://"+ip:"",link=origin?origin+"/"+resumeHash():"";
  const name=routerName();
  /* the way back for a Station only device that cannot join: button_manager's
     config mode (fresh nodes each time: a node has one place) */
  const buttonWay=()=>["hold WiCAN's button for 5 seconds: its access point ",h("code",{},apName())," comes up for 10 minutes (LED blinking yellow and blue); join it with your password and open ",h("a",{href:apLink},apLink)];
  const live=h("div",{});
  /* the moment WiCAN is on the network, once per visit: a popup with the way
     on (Ali, 2026-10-07: "popup when the device is connected to the desired
     network and ask the user to press to go to the address"; a green check
     and "successfully connected"; one button, since the address is the only
     way on). The press is the user's, never a timer; a tap beside the popup
     closes it and the card behind carries the same button. */
  const onAp=location.hostname===((D.wifi&&D.wifi.ap_ip)||"192.168.0.10");
  let popped=false;
  const offerPopup=()=>{
    if(popped||!link)return;popped=true;
    modal({title:null,
      body:h("div",{class:"qs-pop"},h("span",{class:"ci"},ic("check")),
        h("div",{},h("h3",{},"WiCAN successfully connected to "+ssid),
          h("p",{},"Its address there is ",h("code",{},ip),". Quick Setup continues at ",h("code",{},link),"."),
          h("p",{class:"help"},conn.state==="online"&&onAp?"This phone is still on "+apName()+": join "+ssid+", then press Continue.":"Press Continue to open it there."))),
      actions:[{label:"Continue on "+ssid,kind:"pri",fn:()=>{location.href=link;}}]});
    /* Ali's sketch (2026-10-07): wider, a large check, the button with the
       open-in-new mark */
    const m=document.querySelector("#modal-root .modal");
    if(m){m.classList.add("qs-pop-modal");const b=m.querySelector(".acts button.pri");if(b)b.prepend(ic("ext"));}
  };
  const t0=Date.now();let found=false,probing=false;
  const since=()=>Math.round((Date.now()-t0)/1000);
  const paintLive=async()=>{
    if(found)return;
    if(conn.state==="online"){
      const w=await tryGet("/api/wifi/status");
      if(!w){live.replaceChildren(check("run",null,"Reconnecting","Waiting for WiCAN."));return;}
      if(w.sta_connected&&w.ip){
        live.replaceChildren(check("ok","wifi","WiCAN joined "+ssid,["Address ",h("code",{},w.ip),
          ...(w.ip!==ip?[". On that network open ",h("a",{href:"http://"+w.ip+"/"+resumeHash()},"http://"+w.ip+"/"+resumeHash())]:[]),"."],chip("Connected","ok")));
        if(since()>=12)offerPopup();   /* after the restart's time: the join seen at the moment of Save is the old one */
        return;}
      const at=w.sta_attempt||{};
      if(at.fail_count>0||(at.reason&&at.reason!==0)){
        live.replaceChildren(check("warn","alert","WiCAN could not join "+(at.ssid||ssid)+" yet",
          ["Last attempt failed (reason "+at.reason+", "+at.fail_count+" tries). A wrong password is the usual cause. "],
          btn("Fix WiFi",()=>go("wifi"),"sm")));return;}
      live.replaceChildren(check("run",null,"Joining "+ssid,"Usually a few seconds."));
      return;}
    /* the access point is out of reach from here: WiCAN is still restarting,
       or this device has moved to the home WiFi. Look for WiCAN at its
       address there, once the restart has had its time */
    if(since()>=12&&!probing&&origin){
      probing=true;
      const ok=await linkAnswers(origin);
      probing=false;
      if(ok&&!found&&conn.state!=="online"){
        found=true;
        live.replaceChildren(check("ok","wifi","WiCAN answers on "+ssid,["Quick Setup continues at ",h("code",{},ip),"."],
          h("a",{class:"btn pri sm",href:link},ic("send"),"Continue on "+ssid)));
        offerPopup();
        return;}}
    if(found)return;
    if(since()<RECONNECT_PATIENCE_S)
      live.replaceChildren(check("run",null,"Restarting",["About 15 s, then WiCAN joins ",h("b",{},ssid),". Join ",h("b",{},ssid)," on this device meanwhile",link?": this page tells you when it finds WiCAN there.":"."]));
    else if(sta)
      live.replaceChildren(check("warn","alert","Still looking for WiCAN on "+ssid,
        ["Is this device on ",h("b",{},ssid),"? Your router's device list shows WiCAN as ",h("code",{},name)," with its address. If WiCAN did not join (a wrong password is the usual cause), ",...buttonWay(),": that page says so and lets you fix it."]));
    else
      live.replaceChildren(check("warn","alert","Still looking for WiCAN on "+ssid,
        ["Is this device on ",h("b",{},ssid),"? If it is and WiCAN did not join (a wrong password is the usual cause), join ",h("code",{},apName())," with your new password and open ",h("a",{href:apLink},apLink),": that page says so and lets you fix it."]));
  };
  paintLive();every(3000,paintLive);
  const copyBtn=btn("Copy link",()=>{if(copyText(link,"Link copied")){copyBtn.textContent="Copied";setTimeout(()=>{copyBtn.replaceChildren(ic("copy"),"Copy link");},1500);}},"",{icon:"copy"});
  return screen("Now switch networks",["Connect this phone or PC to ",h("b",{},ssid),", then open "+(link?"the link":"WiCAN's address")+". Quick Setup picks up where it left off."],
    [h("div",{class:"qs-link"},
       link?h("span",{class:"url"},link)
         :h("p",{class:"alt"},"WiCAN's address on "+ssid+" is not known yet. This page shows it as soon as WiCAN joins, while this phone stays on "+apName()+"; your router's device list shows it too, as ",h("b",{},name),"."),
       link?h("div",{class:"acts"},h("a",{class:"btn pri",href:link},ic("send"),"Open WiCAN"),copyBtn):null),
     live,
     h("details",{class:"qs-details"},h("summary",{},"The link does not open?"),
       steps(sta?[["Give it a moment","WiCAN needs a few seconds to join."],
           ["Find the address",["Your router's device list shows WiCAN as ",h("code",{},name),"; the address is usually the one from the test."]],
           ["Wrong WiFi password?",["In Station only mode WiCAN has no access point of its own, so ",...buttonWay(),": that page says so and lets you fix it."]]]
         :[["Give it a moment","WiCAN needs a few seconds to join."],
           ["Use the access point instead",["Join ",h("code",{},apName())," with your new password and open ",h("code",{},apLink),". That page shows the address WiCAN received on "+ssid+", and whether the join worked."]],
           ["Wrong WiFi password?","The same page says so and lets you fix it. WiCAN keeps its access point on, so you are never locked out."]]))],
    [h("span",{class:"ghost"},"Stay on this screen until you have switched networks.")]);
};

SCREENS.checks=async()=>{
  const cards=h("div",{class:"qs-body"});
  if(!D.mqtt)D.mqtt=strip(await tryGet("/api/settings/mqtt_manager"));
  if(!D.wifi){const ws=await tryGet("/api/settings/wifi_manager");if(ws)D.wifi=strip(ws);}   /* the mode, after the restart's reload */
  const mqttOn=!!(D.mqtt&&D.mqtt.enabled)||W.use==="mqtt";
  const haOn=W.use==="ha"||(!mqttOn&&W.use!=="wifi")||W.use==="wifi"&&false;
  const L0=await usbLive();const nw=W.noWifi||(!(D.wifi&&D.wifi.sta_ssid)&&L0.kind==="espnetlink");
  const refresh=async()=>{
    const[w,wh,ds,L]=await Promise.all([tryGet("/api/wifi/status"),haOn?tryGet("/api/webhook"):null,mqttOn?tryGet("/api/destinations"):null,usbLive()]);
    const b=(conn.status&&conn.status.bits)||{};
    const ssid=W.ssid||(w&&w.sta_attempt&&w.sta_attempt.ssid)||(D.wifi&&D.wifi.sta_ssid)||"your WiFi";
    const out=[];
    if(nw)out.push(nlNetCard(L));else if(w&&w.sta_connected)out.push(check("ok","wifi","WiFi: joined "+ssid,["Address ",h("code",{},w.ip||""),"; your router's device list shows it as ",h("code",{},routerName()),"."],chip("Connected","ok")));
    else out.push(check("warn","wifi","WiFi: not connected"+(w&&w.sta_attempt&&w.sta_attempt.ssid?" to "+w.sta_attempt.ssid:""),
      w&&w.sta_attempt&&w.sta_attempt.fail_count?"Last attempt failed (reason "+w.sta_attempt.reason+"). Check the password.":"WiCAN is still trying, or the network is out of range.",btn("Fix WiFi",()=>go("wifi"),"sm")));
    const wm=(D.wifi&&D.wifi.mode)||"apsta",autoOff=!!(D.wifi&&D.wifi.ap_auto_disable);
    if(nw)out.push(check("ok","lock","Access point on, secured",[h("code",{},apName())," uses your password and stays on: it is the way to reach WiCAN."],chip("On","ok")));
    else if(wm==="sta")out.push(check("ok","lock","Access point off: Station only",["WiCAN is reached at its address on "+ssid+". Hold its button for 5 seconds whenever you need ",h("code",{},apName())," back for a while."],chip("Off","")));
    else if(autoOff&&w&&w.sta_connected)out.push(check("ok","lock","Access point off while WiCAN is on "+ssid,[h("code",{},apName())," comes back by itself whenever "+ssid+" is lost, with your password."],chip("Off for now","")));
    else if(w&&w.ap_default_password===false||(!w&&!apDefaultPassword()))out.push(check("ok","lock","Access point secured",[h("code",{},apName())," now uses your password. It stays on as a fallback way in."],chip("Done","ok")));
    else out.push(check("warn","lock","Access point still has the factory password","Set your own password so nobody else can join.",btn("Set password",()=>go("ap"),"sm")));
    /* the USB connector, from the device: a page that arrived by the link has no step state */
    const us=L.kind!=="unknown"?usbSummary(L):null;
    if(us)out.push(usbCard(us,us.state==="pend"?"warn":us.state));
    if(haOn&&nw){out.push(check("warn","home","Home Assistant needs the VPN away from home",["WiCAN is on the internet over LTE; Home Assistant is on your home network. Set up WiCAN's VPN under Settings > VPN after this, and Home Assistant finds WiCAN there. Nothing to do now."],h("a",{class:"btn sm",href:"#/settings/vpn"},"VPN settings")));}
    else if(haOn){
      const url=wh&&wh.url;
      if(url){const host=(()=>{try{return new URL(url).origin;}catch(_){return url;}})();
        const age=wh.last_post?Math.max(0,Math.round((Date.now()-Date.parse(wh.last_post))/1000)):null;
        const ok=wh.status==="ok"||(wh.success_count||0)>0;
        out.push(check(ok?"ok":"warn","home",ok?"Home Assistant is connected":"Home Assistant registered, first push pending",
          ["Registered from ",h("code",{},host),". ",age!=null&&ok?"Last push "+age+" s ago, ":"",(wh.success_count||0)+" pushes, "+(wh.fail_count||0)+" failed."+(wh.last_error?" Last error: "+wh.last_error:"")],chip(ok?"Connected":"Waiting",ok?"ok":"warn")));}
      else out.push(check("warn","home","Waiting for Home Assistant",["In Home Assistant open Settings > Devices & services. ",h("b",{},"Discovered: WiCAN "+(deviceId()||""))," should be there. Press Add. This card turns green within a minute. If it does not appear, press Add Integration, search WiCAN and enter ",h("code",{},(w&&w.ip)||"WiCAN's address"),"."],chip("Waiting","warn")));
    }
    if(mqttOn){
      const url=(D.mqtt&&D.mqtt.url)||W.mqtt.url;
      if(b.mqtt_connected)out.push(check("ok","cloud","MQTT broker connected",[h("code",{},url)," as "+((D.mqtt&&D.mqtt.username)||W.mqtt.user||"anonymous")+". Status topic published; vehicle data starts after the next step."+(ds&&ds.destinations?" "+(ds.destinations.filter(d=>d.enabled).length)+" destination(s) active.":"")],chip("Connected","ok")));
      else out.push(check("crit","cloud","Could not connect to the broker",[h("code",{},url||"(no address)"),": not connected. Check the address and port, and that the broker allows this user. "],btn("Fix broker settings",()=>go("details"),"sm")));
    }
    cards.replaceChildren(...out);
  };
  await refresh();every(3000,refresh);
  return screen(nw?"Connected through the ESPNetLink":"Connected through "+(W.ssid||(D.wifi&&D.wifi.sta_ssid)||"your WiFi"),nw?"WiCAN is on the internet over LTE. Quick checks before the vehicle part.":"You are now reaching WiCAN over your home WiFi. Quick checks before the vehicle part.",
    [cards],
    [btn("Finish without vehicle data",()=>{W.noVeh=true;go("done");},"gh sm"),grow(),btn("Set up my vehicle",()=>go("vehicle"),"pri",{icon:"car"})]);
};

/* ---- the vehicle step, second pass (2026-10-01): detect, identify, scan, THEN profile ----
   The device keeps a store of cars (GET /api/autopid/vehicles, keyed by VIN
   or by the set of answering ECUs). Detection is one job (the std scan with
   phases); a known car comes back with its profile, an unknown one gets
   standard PIDs only until a profile is chosen here. Nothing is saved
   until Finish. */
const WMI={KMH:"Hyundai",KM8:"Hyundai",KMF:"Hyundai",KNA:"Kia",KND:"Kia",KNE:"Kia",KNC:"Kia",
  WVW:"Volkswagen",WV1:"Volkswagen",WV2:"Volkswagen",WVG:"Volkswagen","3VW":"Volkswagen","1VW":"Volkswagen","9BW":"Volkswagen",
  WAU:"Audi",WA1:"Audi",TRU:"Audi",WUA:"Audi",WBA:"BMW",WBS:"BMW",WBX:"BMW",WBY:"BMW","5UX":"BMW","4US":"BMW",
  WP0:"Porsche",WP1:"Porsche",LGX:"BYD",LC0:"BYD",SAJ:"Jaguar",SAD:"Jaguar",JN1:"Nissan",JN8:"Nissan","1N4":"Nissan",SJN:"Nissan",VSK:"Nissan",
  W0L:"Opel",W0V:"Opel",VF1:"Renault",VF2:"Renault",JTM:"Toyota",JTD:"Toyota",JTN:"Toyota",JTE:"Toyota",JTH:"Lexus",SB1:"Toyota","2T3":"Toyota","4T1":"Toyota","5TD":"Toyota",
  "5YJ":"Tesla",LRW:"Tesla",XP7:"Tesla","7SA":"Tesla",TMB:"Skoda",VSS:"Seat",YV1:"Volvo",LYV:"Volvo",LPS:"Polestar",
  WDD:"Mercedes",WDC:"Mercedes",W1K:"Mercedes",W1N:"Mercedes",SAL:"Land Rover",ZFA:"Fiat",VR3:"Peugeot",VF3:"Peugeot",VR7:"Citroen",VF7:"Citroen",
  LSJ:"MG",SDP:"MG",JHM:"Honda","1HG":"Honda","2HG":"Honda","19X":"Honda",SHH:"Honda",WF0:"Ford","1FA":"Ford","1FT":"Ford","3FA":"Ford",
  "1G1":"Chevrolet","1GC":"Chevrolet",KL1:"Chevrolet","1C4":"Jeep","2C3":"Chrysler",JM1:"Mazda",JM3:"Mazda",JA3:"Mitsubishi",JA4:"Mitsubishi",JF1:"Subaru",JF2:"Subaru"};
const MAKE_ALIAS={volkswagen:["vw"],mercedes:["mercedes-benz","mb"],"land rover":["landrover","range rover"]};
const wmiMake=vin=>{const v=String(vin||"").toUpperCase();return v.length>=3?(WMI[v.slice(0,3)]||""):"";};
const sameMake=(a,b)=>{a=String(a||"").toLowerCase().trim();b=String(b||"").toLowerCase().trim();if(!a||!b)return false;
  if(a===b||a.startsWith(b)||b.startsWith(a))return true;
  const al=(MAKE_ALIAS[a]||[]).concat(MAKE_ALIAS[b]||[]);return al.includes(a)||al.includes(b);};
const defaultName=v=>{const mk=wmiMake(v.vin);const tail=v.vin?v.vin.slice(-4):(v.fingerprint||"").slice(0,4);
  return mk?mk+" ("+tail+")":(v.vin?"Vehicle ("+tail+")":(v.fingerprint?"Vehicle "+tail:""));};
const fmtAge=ts=>{if(!ts)return "";const d=Math.round(Date.now()/1000)-ts;if(d<0||d>1e9)return "";
  if(d<120)return "just now";if(d<7200)return Math.round(d/60)+" min ago";if(d<172800)return Math.round(d/3600)+" h ago";return Math.round(d/86400)+" days ago";};

SCREENS.vehicle=async()=>{
  if(!D.autopid){D.autopid=strip(await tryGet("/api/settings/autopid"));D.autopidSchema=await tryGet("/api/settings/autopid/schema");}
  D.vehicles=await tryGet("/api/autopid/vehicles"); /* null = firmware without the store */
  D.chip=await tryGet("/api/obd_chip");   /* the chip's firmware against the device's image (2026-11-08) */
  const maxName=(D.autopidSchema&&D.autopidSchema.properties&&D.autopidSchema.properties.vehicle&&D.autopidSchema.properties.vehicle.maxLength)||63;
  const err=h("div",{});
  const body=h("div",{class:"qs-body"});
  const finish=btn("Continue",()=>go("power"),"pri");
  /* the profile's make and model as the entry's name ("Toyota: Sienna 2021-"
     -> "Toyota Sienna 2021"), the maker's trailing dash dropped */
  const profileName=car=>{const sp=picker.splitName(car&&car.car_model);return(sp.make+" "+sp.model).replace(/\s*-\s*$/,"").replace(/\s+/g," ").trim().slice(0,maxName);};
  const profileSays=car=>{const pm=profileProtocol(car);const n=(car.pids||[]).length;
    const names=(car.pids||[]).flatMap(p=>(p.parameters||[]).map(x=>x.name)).filter(Boolean).slice(0,5).join(", ");
    return(pm?protoName(pm)+" (the profile's init), ":"")+n+" request"+(n===1?"":"s")+(names?": "+names:"")+".";};
  /* picked before the probe (section 1, 2026-10-26): the choice is the
     profile, the name follows it until the user types one */
  const picker=vehicleProfilePicker({current:()=>W.profCar?W.profCar.car_model:(W.veh&&W.veh.profile)||"",onPick:car=>{W.profCar=car;W.profChoice="car";
    if(W.vehPhase==="idle"||W.vehPhase==="noanswer"){W.carChoice="profile";if(!W.vehName||W.vehNameAuto){W.vehName=profileName(car);W.vehNameAuto=true;}}
    render();}});
  const entries=()=>(D.vehicles&&Array.isArray(D.vehicles.vehicles))?D.vehicles.vehicles:[];
  const protoName=p=>PROTO[String(p)]||(String(p)==="0"||!p?"Automatic":"protocol "+p);
  const norm=c=>String(c||"").replace(/\s+/g,"").toUpperCase();
  const stdRows=()=>((W.scanResult&&W.scanResult.supported)||[]).filter(r=>r&&r.cmd);
  const stdPick=()=>W.stdPick||W.stdDefault||[];
  /* the popup is the page's standard-PID picker, the one Automate >
     Parameters opens on a scan (Ali, 2026-10-07: "like in the autopid"):
     every PID the scan found, the chosen ones ticked (none for a new car) */
  async function openStdPicker(){
    const sup=stdRows();
    const picked=await pickStdRows(sup,{title:"Standard PIDs this car answers",preset:new Set(stdPick()),
      lead:sup.length+" supported PIDs reported by the vehicle. Tick the ones WiCAN should read:"});
    if(picked){W.stdPick=picked.map(r=>norm(r.cmd));render();}
  }
  /* "Same car as..." (2026-10-26): a car seen without its VIN folded into
     the entry it belongs to; its ECU set stays with that entry as a face,
     so the next contact that answers the same way lands on the right car */
  async function sameCar(e){
    const others=entries().filter(x=>x.key!==e.key);
    if(!others.length)return;
    let into=others[0].key;
    const choices=h("div",{class:"qs-radios"},...others.map((x,i)=>h("label",{},h("input",{type:"radio",name:"qs-same",checked:i===0,onchange:()=>{into=x.key;}})," "+(x.name||defaultName(x)||x.key)+(x.vin?" · "+x.vin:" · no VIN")+(x.profile?" · "+x.profile:""))));
    const ok=await new Promise(res=>modal({title:"Same car as...",
      body:h("div",{},h("p",{class:"help",style:"margin:0 0 8px;font-size:12.5px"},"\""+(e.name||defaultName(e)||e.key)+"\" is folded into the car you pick: its ECU set is remembered as a face of that car, its own rows are dropped."),choices),
      actions:[{label:"Cancel",fn:()=>res(false)},{label:"Fold it in",kind:"pri",fn:()=>res(true)}]}));
    if(!ok)return;
    try{await api("/api/autopid/vehicles/"+encodeURIComponent(e.key)+"/merge",{method:"POST",body:{into}});toast("Folded in","ok");D.vehicles=await tryGet("/api/autopid/vehicles");render();}
    catch(ex){toast(ex.message,"err");}
  }
  /* The OBD chip's firmware (2026-11-08, Ali). On chip firmware V2.3.22 the
     warm start the bring-up sends kills the chip's automatic protocol search
     until the device is unplugged (the finding of 2026-11-08): the detection
     here, and every app's ATSP0, cannot find the car. This device carries
     V2.3.24, which does not have the fault. So the step asks for the update
     before anything else: the page's shared flow (chipFw in index.html: the
     popup with one button, the job with its progress, Done, the banner),
     the same the Status page shows a device that was set up before. Closed
     by a tap beside it, the offer stays as the banner and Detect waits for
     the chip. WiCAN stays awake meanwhile (the sleep entry waits for the
     job). */
  const chipOld=()=>chipFw.outdated(D.chip)&&W.chip.state!=="done";
  function chipFailed(why){W.chip.state="failed";W.chip.err=why;W.chip.tries++;render();}
  async function startChipUpdate(){
    if(W.chip.state==="running")return;
    W.chip.state="running";W.chip.err="";render();
    const r=await chipFw.run(D.chip);
    if(!r.ok){chipFailed(r.why);return;}
    D.chip=r.st;W.chip.state="done";render();
    chipFw.done(r.st,()=>render());
  }
  const render=()=>{
    const out=[];
    const v=W.veh;
    const phase=W.vehPhase;
    if(chipOld()){
      /* the offer stays on the step (the popup closed, or an attempt failed) */
      out.push(chipFw.banner(D.chip,W.chip,startChipUpdate,"qs-chip-banner"));
    }
    if(phase==="idle"||phase==="noanswer"){
      /* 1. which car (2026-10-26, discussion #1018, Ali: the profile before
         the probe): a published profile, or no profile and detection as
         before; the action of the first choice at the right edge of its row */
      out.push(sub("1. Which car is this?"));
      out.push(h("p",{class:"qs-lead",style:"font-size:13px"},"WiCAN knows the published profiles (what each car answers beyond the standard PIDs). Say which car this is first: the detection then uses the profile's protocol and requests, and the car is stored under this name even when it answers no standard request."));
      const car1=W.carChoice==="profile"?W.profCar:null;
      const pickProfile=()=>{W.carChoice="profile";picker.open();};
      out.push(h("div",{class:"qs-radios"},
        h("div",{style:"display:flex;align-items:center;gap:10px;flex-wrap:wrap"},
          h("label",{},h("input",{type:"radio",name:"qs-car",id:"qs-car-profile",checked:W.carChoice==="profile",onchange:()=>{if(W.profCar){W.carChoice="profile";render();}else pickProfile();}})," A published profile"),
          h("div",{class:"qs-pick",style:"margin:0 0 0 auto;padding:0"},...(car1?[h("span",{class:"sel"},ic("car"),car1.car_model),btn("Change profile",pickProfile,"sm",{icon:"search"})]:[btn("Choose profile",pickProfile,"sm",{icon:"search"})]))),
        h("div",{class:"help",style:"font-size:12.5px;margin:-4px 0 8px 26px"},car1?profileSays(car1):"The make and model from the published list: what the car answers beyond the standard PIDs."),
        h("div",{},h("label",{},h("input",{type:"radio",name:"qs-car",id:"qs-car-detect",checked:W.carChoice!=="profile",onchange:()=>{W.carChoice="detect";render();}})," No profile for this car: detect it")),
        h("div",{class:"help",style:"font-size:12.5px;margin:-4px 0 8px 26px"},"WiCAN learns the protocol, reads the VIN and finds the standard PIDs it answers. A profile can still be picked after the scan.")));
      out.push(h("dl",{class:"qs-kv"},h("dt",{},"Name"),h("dd",{},
        h("input",{id:"qs-veh-name",value:W.vehName,placeholder:"e.g. Family car",style:"max-width:260px",oninput:e=>{W.vehName=e.target.value;W.vehNameAuto=false;}}),
        h("span",{class:"help",style:"font-size:12px"},car1&&W.vehNameAuto?"from the profile; change it if you like":"shown on the Status page and in Home Assistant"))));
      out.push(sub("2. Detect the vehicle"));
      if(phase==="noanswer")out.push(banner("warn","alert",h("b",{},"The car did not answer. "),
        (W.scanStatus&&W.scanStatus.error?W.scanStatus.error+". ":"")+"Usually the ignition is off or the plug is not seated. Turn the ignition on (an EV in ready mode) and try again. You can also skip: WiCAN detects the car by itself the first time it answers, and the Status page tells you when it has."));
      const detBtn=btn(phase==="noanswer"?"Try again":"Detect my vehicle",startDetect,"pri",{icon:"refresh",disabled:!(W.plugged&&W.ignition)||chipOld()});
      const ack=(key,id,title,text)=>h("label",{class:"qs-agree"},h("input",{type:"checkbox",id,checked:W[key],onchange:e=>{W[key]=e.target.checked;detBtn.disabled=!(W.plugged&&W.ignition)||chipOld();}}),h("div",{},h("b",{},title),h("span",{},text)));
      out.push(ack("plugged","qs-plugged","WiCAN is plugged into the OBD port","Under the dashboard, driver side on most cars."));
      out.push(ack("ignition","qs-ignition","Ignition is on","The engine can stay off. On an EV, \"ready\" mode."));
      out.push(h("div",{class:"rowflex",style:"gap:12px"},detBtn,chipOld()?chip("Update the chip first","warn"):null));
      out.push(note("",W.carChoice==="profile"&&W.profCar
        ?"About 10 to 20 s. WiCAN pins the protocol the profile expects, reads the VIN and the standard PIDs, and sends the profile's first requests. A car that answers no standard request is still confirmed by those and stored under the name above."
        :"About 15 to 30 s. WiCAN learns the protocol, reads the VIN and finds the standard PIDs, then remembers all of it under this vehicle. Plug it into another car later and it tells the cars apart."));
      if(!D.vehicles)out.push(note("","This firmware keeps one vehicle only (no vehicle store): the result is stored, but moving WiCAN between cars needs a new scan each time."));
    }else if(phase==="detecting"){
      out.push(sub("2. Detecting the vehicle"));
      const phases=[["protocol","Detecting the OBD protocol","WiCAN asks on each CAN protocol in turn, first the OBD-II way (0100), then the way of newer vans and trucks (ISO 27145: 22 F400). A few seconds."],["vin","Reading the VIN","Mode 09 PID 02, then UDS 22 F190 (22 F802 on an ISO 27145 vehicle). Without either, the car is identified by the set of ECUs that answered."],["pids","Finding the standard PIDs","The support bitmaps of every ECU that answers (0100, 0120 and up, or 22 F400 and up). Stored under this vehicle."],["network","Listening to the vehicle network","Is this a J1939 network (trucks, buses, agricultural machines)? WiCAN only listens here: the controllers that broadcast, the groups they send, the VIN when one is broadcast."]];
      phases.push(["probe","Asking the profile's own requests","Nothing standard answered: the profile's first requests confirm the car and give the ECUs it is identified by."]);
      const ph=W.scanStatus&&W.scanStatus.phase;const idx=Math.max(0,phases.findIndex(p=>p[0]===ph));
      phases.forEach((p,i)=>out.push(check(i<idx?"ok":i===idx?"run":"","info",p[1],p[2],i<idx?chip("Done","ok"):i===idx?chip("Working",""):null)));
    }else{
      /* result */
      out.push(sub("2. Vehicle detected"));
      const known=!!(v.known&&v.profile);
      const j1939Only=v.dialect==="j1939";
      if(known)out.push(banner("ok","check",h("b",{},"Welcome back: "+(v.name||defaultName(v))+". "),"WiCAN set this car up before and already switched to its profile, PIDs and protocol. Nothing to do here unless you want to change something."));
      else if(j1939Only)out.push(banner("ok","check",h("b",{},"J1939 vehicle. "),"This vehicle speaks SAE J1939, the network of trucks, buses and agricultural machines. Its controllers broadcast their values; WiCAN listens and never asks. "+(v.std_supported||0)+" standard parameters were heard and stored under this vehicle."+(v.vin?"":" No VIN was broadcast: it is remembered by the set of controllers on its network.")));
      else if(v.confirmed_by==="profile")out.push(banner("ok","check",h("b",{},"New vehicle: "+(W.vehName||v.name||"the profile's car")+". "),"This car answers no standard request (no 0100, no VIN): WiCAN confirmed it with the profile's own request"+(v.probeAnswered?" ("+v.probeAnswered+")":"")+" and stored it under this name, identified by the ECUs that answered. There is no standard set to choose from."));
      else if(!v.vin)out.push(banner("info","info",h("b",{},"New vehicle. "),"This car gave no VIN, so WiCAN remembers it by the set of ECUs that answered (that works for one car; two identical models would look the same). If it answers its VIN another day, a second entry appears: \"Same car as...\" in the list below folds the two into one. Give it a name below."));
      else out.push(banner("ok","check",h("b",{},v.known?"Vehicle recognised. ":"New vehicle. "),"WiCAN stored the protocol and VIN under this car and found "+(v.std_supported||0)+" standard PIDs it answers. Choose the ones to read below, and a profile for the vehicle-specific values."));
      if(v.j1939&&!v.j1939Listening)out.push(banner("warn","alert",h("b",{},"One more restart for the J1939 listener. "),"The J1939 network is read by WiCAN's own CAN controller, which is off on a new device. Finish turns it on in listen-only mode (it never transmits) together with the J1939 listener; the values arrive after that restart."));
      else if(v.j1939&&!j1939Only)out.push(banner("info","info",h("b",{},"A J1939 network as well. "),"This vehicle answers OBD requests and broadcasts J1939 groups. Both sets of parameters are stored under it."));
      const nameInp=h("input",{id:"qs-veh-name",value:W.vehName,placeholder:"e.g. Family car",style:"max-width:260px",oninput:e=>{W.vehName=e.target.value;}});
      const d2=t=>h("span",{class:"d2"},t);
      out.push(h("dl",{class:"qs-kv"},
        h("dt",{},"VIN"),h("dd",{},v.vin?[h("code",{},v.vin),d2(j1939Only?"broadcast by the vehicle":"read from the car")]:[h("span",{},v.confirmed_by==="profile"?"not available":"not answered"),j1939Only||v.confirmed_by==="profile"?null:chip("No VIN","warn"),d2((j1939Only?"identified by its controllers":v.confirmed_by==="profile"?"identified by the ECUs the profile's request answered from":"identified by its ECUs")+(v.fingerprint?" ("+v.fingerprint+")":""))]),
        h("dt",{},j1939Only?"Network":"OBD protocol"),h("dd",{},j1939Only?[h("span",{id:"qs-veh-dialect"},"SAE J1939"+(v.busKbps?", "+v.busKbps+" kbit/s":"")),d2("heard")]:[protoName(v.protocol),DIALECT[v.dialect]?h("span",{id:"qs-veh-dialect"},DIALECT[v.dialect]):null,d2(PROTO[String(v.protocol)]?"detected":"automatic"),v.j1939?d2("and a J1939 network"):null]),
        h("dt",{},j1939Only?"Standard parameters":"Standard PIDs"),h("dd",{},(v.std_supported||0)+(j1939Only?" heard":" answered"),d2(j1939Only?"stored under this vehicle":stdPick().length?stdPick().length+" chosen":"none chosen yet")),
        h("dt",{},"Name"),h("dd",{},nameInp,h("span",{class:"help",style:"font-size:12px"},"shown on the Status page and in Home Assistant"))));
      /* 2. the standard PIDs: the scan found what the car answers, the user
         chooses what WiCAN reads (nothing ticked for a new car, none is
         allowed: Ali, 2026-10-07); a J1939-only vehicle has no requests */
      if(!j1939Only&&v.confirmed_by!=="profile"){
        out.push(sub("3. Standard PIDs"));
        const sup=stdRows(),pick=stdPick();
        const names=pick.map(c=>(sup.find(r=>norm(r.cmd)===c)||{}).name||c);
        out.push(h("p",{class:"qs-lead",style:"font-size:13px"},"The scan found "+sup.length+" standard PIDs this car answers: speed, RPM, coolant, fuel level and the like. Choose the ones WiCAN should read. Each one is a request every polling round, so fewer means faster updates for the ones you keep; the rest can be added any time under Automate > Parameters."));
        out.push(h("div",{class:"qs-pick"},btn(pick.length?"Change":"Choose PIDs",openStdPicker,pick.length?"sm":"pri sm",{icon:"check"}),
          h("span",{class:"help",style:"font-size:12.5px"},pick.length?pick.length+" of "+sup.length+" chosen: "+names.slice(0,4).join(", ")+(names.length>4?" and "+(names.length-4)+" more":"")
            :"None chosen yet. That is allowed: WiCAN then reads nothing from the standard set, only what the profile asks for.")));
      }
      /* 2. profile (a J1939-only vehicle has no OBD requests for a profile to add) */
      if(j1939Only&&!known){
        if(W.profChoice===null){W.profChoice="none";W.profCar=null;}
        out.push(sub("3. Vehicle profile"));
        out.push(note("","Profiles carry OBD requests, which a J1939 vehicle does not answer. The standard J1939 parameters are stored already; more groups can be added as PGN rows under Automate > Parameters."));
      }
      const chosen=W.profChoice==="car"?W.profCar:null;
      if(!(j1939Only&&!known)){
      out.push(sub(((j1939Only||v.confirmed_by==="profile")?"3":"4")+". Vehicle profile"));
      const testBtn=btn("Test profile",()=>testProfile(chosen||(known?{car_model:v.profile,fromStore:true}:null)),"sm",{icon:"refresh",disabled:!(chosen||known)});
      if(W.carChoice==="profile"&&chosen){
        /* named before the probe (section 1): the profile is attached to the
           entry already; change it, test it, or scan again */
        out.push(h("div",{class:"qs-pick"},h("span",{class:"sel"},ic("car"),chosen.car_model),btn("Change profile",()=>picker.open(),"sm",{icon:"search"}),testBtn,btn("Scan again",startDetect,"sm",{icon:"refresh"})));
        out.push(note("","The profile is saved under this car: WiCAN applies it again whenever this car answers, and never to another car. Test profile sends each of its requests once and shows the values before anything is saved."));
      }else if(known&&W.profChoice===null){
        out.push(h("div",{class:"qs-pick"},h("span",{class:"sel"},ic("car"),v.profile),btn("Change profile",()=>picker.open(),"sm",{icon:"search"}),testBtn,btn("Scan again",startDetect,"sm",{icon:"refresh"})));
        out.push(note("","The profile and the PIDs you had for this car are back in place. Scan again only if the car behaves differently than before."));
      }else{
        const mk=wmiMake(v.vin);
        const sug=(W.profList||[]).filter(c=>mk&&sameMake(picker.splitName(c.car_model).make,mk)).slice(0,6);
        out.push(h("p",{class:"qs-lead",style:"font-size:13px"},
          mk&&sug.length?["The VIN says ",h("b",{},mk)," (manufacturer code "+v.vin.slice(0,3)+"). Profiles for "+mk+":"]
          :mk?["The VIN says ",h("b",{},mk),", but the published list has no "+mk+" profile yet. Search all profiles, or keep the standard PIDs for now."]
          :W.profList===null?"The published profile list could not be fetched (this phone needs internet). You can still keep the standard PIDs for now, or drop a profile file on Automate > Parameters later."
          :v.vin?["The manufacturer code ",h("code",{},v.vin.slice(0,3))," is not one WiCAN knows: search all profiles, or keep the standard PIDs for now."]
          :"No VIN to go on: search the published profiles, or keep the standard PIDs for now."));
        const radios=h("div",{class:"qs-radios"});
        sug.forEach(c=>radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",checked:chosen===c,onchange:()=>{W.profCar=c;W.profChoice="car";render();}})," "+c.car_model)));
        radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",id:"qs-prof-other",checked:chosen&&!sug.includes(chosen),onchange:()=>picker.open()})," Another profile: ",btn("Choose profile",()=>picker.open(),"sm",{icon:"search"}),chosen&&!sug.includes(chosen)?h("span",{class:"sel",style:"padding:3px 9px;margin-left:6px"},chosen.car_model):null));
        radios.append(h("label",{},h("input",{type:"radio",name:"qs-prof",id:"qs-prof-none",checked:W.profChoice==="none",onchange:()=>{W.profChoice="none";W.profCar=null;render();}})," Keep it without a profile (standard PIDs only). You can add one later from Automate > Parameters."));
        out.push(radios);
        out.push(h("div",{class:"qs-pick",style:"margin-top:6px"},testBtn,h("span",{class:"help",style:"font-size:12.5px"},chosen?"Sends each of the profile's requests to the car once and shows what comes back, before anything is saved.":"Pick a profile to test it against the car.")));
        if(chosen){const pm=profileProtocol(chosen);if(pm&&PROTO[String(v.protocol)]&&pm!==String(v.protocol))out.push(banner("warn","alert",h("b",{},"Protocol mismatch. "),"This profile expects "+protoName(pm)+" but the car answered on "+protoName(v.protocol)+". It may still work for some requests; test it first."));}
        out.push(note("","A profile adds the values the standard PIDs do not carry (battery state of charge, charging power). It is saved under this car: WiCAN applies it again whenever this car answers, and never to another car."));
      }
      }
    }
    /* the store */
    const list=entries();
    if(D.vehicles){
      out.push(sub("Vehicles this WiCAN knows"));
      if(list.length){
        out.push(h("div",{class:"qs-nets"},...list.map(e=>h("div",{class:"qs-net",style:"grid-template-columns:1fr auto auto;cursor:default"},
          h("span",{},h("b",{},e.name||defaultName(e)||"Unnamed vehicle"),h("br"),h("span",{class:"help"},(e.vin||("no VIN, "+(e.dialect==="j1939"?"controller set ":"ECU set ")+(e.fingerprint||"")))+" · "+(e.dialect==="j1939"?"SAE J1939":protoName(e.protocol)+(DIALECT[e.dialect]?", "+DIALECT[e.dialect]:"")+(e.j1939?", J1939 network":""))+" · "+(e.profile||"no profile")+" · "+(e.std_supported||0)+(e.dialect==="j1939"?" std parameters":" std PIDs")+(e.last_seen?" · seen "+fmtAge(e.last_seen):""))),
          e.current?chip("Current","pri"):(!e.vin&&e.dialect!=="j1939")?chip("No VIN","warn"):e.pending_profile?chip("Profile pending","warn"):h("span",{}),
          h("span",{style:"display:flex;gap:6px;align-items:center;justify-content:flex-end"},
            (!e.vin&&e.dialect!=="j1939"&&list.length>1)?btn("Same car as...",()=>sameCar(e),"sm",{icon:"car"}):null,
            btn("Forget",async()=>{if(!await confirmModal("Forget "+(e.name||e.vin||"this vehicle")+" and its PIDs?","Forget vehicle"))return;
              try{await api("/api/autopid/vehicles/"+encodeURIComponent(e.key),{method:"DELETE"});toast("Forgotten","ok");D.vehicles=await tryGet("/api/autopid/vehicles");render();}catch(ex){toast(ex.message,"err");}},"sm gh"))))));
      }else out.push(h("div",{class:"empty"},"No vehicle stored yet. Detect one above."));
      out.push(note("","Up to "+(D.vehicles.max||8)+" vehicles. The current one is whichever answered last; its PIDs, protocol and profile are what WiCAN polls. Forget removes a car and its PIDs."));
    }
    out.push(err);
    body.replaceChildren(...out);
    finish.disabled=!(phase==="result"&&(W.profChoice!==null||(v&&v.known&&v.profile)));
  };
  /* the published list, for the suggestions (null = not reachable) */
  async function loadProfiles(){if(W.profList===undefined){W.profList=await picker.load();}}
  async function startDetect(){
    try{
      const st0=await tryGet("/api/autopid/std_scan");
      if(st0&&st0.status==="running")toast("A scan is already running","");
      else{
        /* the car named before the probe (2026-10-26): the profile, the
           name, the protocol its init pins and its first three requests
           (each with its own init when it has one) go with the job, which
           confirms a car that answers nothing standard with those */
        const pc=W.carChoice==="profile"?W.profCar:null;
        const detectBody=pc?{profile:String(pc.car_model||"").slice(0,maxName),name:String(W.vehName||"").slice(0,31),protocol:profileProtocol(pc)||"",init:String(pc.init||"").slice(0,95),
          cmds:(pc.pids||[]).map(p=>({cmd:String(p.pid||"").replace(/\s+/g,""),init:String(p.pid_init||"").slice(0,95)})).filter(x=>/^[0-9A-Fa-f]{2,15}$/.test(x.cmd)).slice(0,3).map(x=>x.init?x:x.cmd)}:{};
        try{await api("/api/autopid/vehicles/detect",{method:"POST",body:detectBody});}
        catch(e){if(/404|not found/i.test(e.message))await api("/api/autopid/std_scan",{method:"POST",body:{}});else throw e;}
      }
      const list0=entries();   /* the store before this scan: a new car is one it did not have */
      W.vehPhase="detecting";W.veh=null;W.stdPick=null;W.stdDefault=[];
      if(W.carChoice==="profile"&&W.profCar)W.profChoice="car";else{W.profChoice=null;W.profCar=null;}
      W.scanStatus=(await tryGet("/api/autopid/std_scan"))||{status:"running"};render();
      loadProfiles();
      let ticks=0;
      const t=every(1200,async()=>{
        const s=await tryGet("/api/autopid/std_scan");W.scanStatus=s;
        if(s&&s.status==="running"&&ticks++<150){render();return;}
        clearInterval(t);
        const r=await tryGet("/api/autopid/std_scan/result");
        D.vehicles=await tryGet("/api/autopid/vehicles");
        if(!s||s.status==="failed"||!r||(!(r.supported||[]).length&&!r.vin&&!r.protocol_detected&&!r.j1939)){W.vehPhase="noanswer";render();return;}
        const list=entries();
        const key=r.key||r.vin||(r.fingerprint?"fp:"+r.fingerprint:"");
        const e=list.find(x=>x.key===key)||list.find(x=>x.current)||{};
        W.veh={key:e.key||key,vin:r.vin||e.vin||"",fingerprint:r.fingerprint||e.fingerprint||"",protocol:r.protocol_detected||e.protocol||r.protocol||"",dialect:r.dialect||e.dialect||"obd2",
          std_supported:(r.supported||[]).length||e.std_supported||0,known:!!r.known||(!!e.profile&&W.carChoice!=="profile"),name:r.name||e.name||"",profile:e.profile||"",specific_init:e.specific_init||"",pending_profile:e.pending_profile,
          j1939:!!(r.j1939||e.j1939),j1939Listening:r.j1939_listening!==false,busKbps:r.bus_kbps||0,
          confirmed_by:r.confirmed_by||e.confirmed_by||"obd",probeAnswered:r.probe_answered||""};
        W.scanResult=r;W.vehName=W.veh.name||defaultName(W.veh);
        {const sup=(r.supported||[]).map(x=>norm(x.cmd));
         const cfgNow=await tryGet("/api/autopid/config");
         const inCfg=new Set(((cfgNow&&cfgNow.pids)||[]).filter(x=>x&&x.type==="std").map(x=>norm(x.cmd)));
         /* the default choice (2026-10-09, after "none chosen" came back as "16 of
            16 chosen"): a car set up before comes with its current rows ticked; a
            car whose setup never finished (pending_profile: the device stored its
            rows at the detection, nobody chose them) starts with none, like a new
            car, so a page reload between the detection and Finish cannot turn none
            into all. A J1939 vehicle's groups are heard, not asked: all of them. */
         const setUp=list0.some(x=>x.key===W.veh.key)&&W.veh.pending_profile!==true;
         W.stdDefault=W.veh.dialect==="j1939"?sup:setUp?sup.filter(c=>inCfg.has(c)):[];}
        await loadProfiles();
        W.vehPhase="result";render();
      });
    }catch(e){W.vehPhase="noanswer";W.scanStatus={error:e.message};render();}
  }
  /* Test profile: one real request per profile PID through POST /api/autopid/test,
     every parameter decoded from that reply; shown in a popup, nothing saved */
  async function testProfile(car){
    if(!car)return;
    let rows;
    if(car.fromStore){const list=(W.profList||await picker.load())||[];const c=list.find(x=>x.car_model===car.car_model);if(!c){toast("The published list does not have this profile any more: test it from Automate > Parameters","");return;}car=c;}
    rows=profileToPids(car,[]).pids;
    const tb=h("tbody",{});
    const summary=chip("Testing…","");
    const body=h("div",{},
      h("p",{class:"help",style:"margin:0 0 10px;font-size:12.5px"},"Each of the profile's requests is sent to the car once (its init chain, then the request); the values are decoded from those replies with the profile's expressions. A row with no answer means the car did not reply to that request: the parameter will stay empty if you use this profile."),
      h("div",{class:"scroll"},h("table",{class:"tbl"},h("thead",{},h("tr",{},h("th",{},"Parameter"),h("th",{class:"num"},"Value"),h("th",{},"Unit"),h("th",{},"Request"))),tb)));
    const close=modal({title:"Test: "+car.car_model,body:h("div",{},h("div",{class:"rowflex",style:"margin-bottom:8px"},summary,h("span",{class:"grow"})),body),
      actions:[{label:"Close"},{label:"Use this profile",kind:"pri",fn:()=>{W.profCar=car;W.profChoice="car";render();}}]});
    const m=document.querySelector("#modal-root .modal");if(m)m.classList.add("wide");
    let ok=0,total=0,stop=false;
    const prev=document.getElementById("modal-root").onclick;document.getElementById("modal-root").onclick=e=>{if(e.target===e.currentTarget){stop=true;prev&&prev(e);}};
    for(const row of rows){
      if(stop||!tb.isConnected)break;
      const prm=row.parameters||[];
      let res=null,errMsg="";
      for(let attempt=0;attempt<2&&!res;attempt++){
        try{res=await api("/api/autopid/test",{method:"POST",body:{cmd:row.cmd,init:row.init||"",type:"specific",expressions:prm.map(p=>p.expression)}});}
        catch(e){errMsg=e.message;if(/409|busy|another/i.test(e.message)){await new Promise(r=>setTimeout(r,1200));}else break;}
      }
      prm.forEach((p,i)=>{
        total++;
        const val=res&&res.ok&&Array.isArray(res.values)?res.values[i]:null;
        const good=val!==null&&val!==undefined&&!Number.isNaN(val);
        if(good)ok++;
        tb.append(h("tr",{style:good?"":"color:var(--text-3)"},
          h("td",{},h("b",{},p.name),p.name!==row.name?h("span",{class:"help"}," "+row.name):null),
          h("td",{class:"num mono",style:good?"":"color:var(--danger)"},good?String(Number.isInteger(val)?val:Math.round(val*100)/100):(res&&res.ok?"null":"no answer")),
          h("td",{},p.unit||""),
          h("td",{class:"mono",style:"font-size:11px;color:var(--text-3)"},row.cmd+(res&&res.raw?": "+String(res.raw).replace(/\s+/g," ").slice(0,60):errMsg?": "+errMsg.slice(0,60):""))));
      });
      summary.replaceChildren(h("span",{class:"d"}),ok+" of "+total+" parameters answered");
    }
    summary.className="chip "+(total&&ok===total?"ok":ok?"warn":"crit");
    summary.replaceChildren(h("span",{class:"d"}),ok+" of "+total+" parameters answered"+(stop?" (stopped)":""));
    /* a profile's passive CAN filters read frames the car broadcasts: nothing to
       request here; they come along at Finish like the PIDs (2026-10-27) */
    const nf=(car.filters||[]).length;
    if(nf&&tb.isConnected)tb.parentNode.parentNode.append(h("p",{class:"help",style:"margin:8px 0 0;font-size:12.5px"},"The profile also reads "+nf+" frame"+(nf===1?"":"s")+" the car broadcasts (passive CAN filter"+(nf===1?"":"s")+": "+(car.filters||[]).flatMap(f=>(f.parameters||[]).map(x=>x.name)).join(", ")+"). Nothing to request for those: they are set up with the profile."));
    void close;
  }
  if(W.vehPhase===undefined||W.vehPhase===null)W.vehPhase="idle";
  if(W.vehPhase==="result")await loadProfiles();
  render();
  /* the chip's firmware first (2026-11-08): the popup once, on an
     untouched chip; after a closed popup or a failed attempt the banner
     carries the same button */
  if(chipOld()&&W.chip.state==="unknown")chipFw.prompt(D.chip,startChipUpdate);
  return screen("Your vehicle","Say which car it is, then WiCAN detects it: the OBD protocol, the VIN and the standard PIDs it supports, with the profile's own requests for a car that answers nothing standard. Everything is stored under the car, so moving WiCAN between cars just works.",
    [body],
    [back("checks"),grow(),btn("Skip for now",()=>{W.noVeh=true;go("done");},"gh sm"),finish]);
};
/* the protocol a profile's init chains name (ATSP/ATTP n), "" when none */
function profileProtocol(car){
  const rx=/AT\s*[ST]P\s*([0-9A-C])/i;
  let m=rx.exec(car.init||"");if(m)return m[1].toUpperCase();
  for(const p of(car.pids||[])){m=rx.exec(p.pid_init||"");if(m)return m[1].toUpperCase();}
  return "";
}

/* ---- step 10: battery and sleep (2026-10-01, Ali) ----
   Two readings from the car make the Power Saving pair: the charging voltage
   with the engine running and the resting voltage after key-off. The wake
   voltage is a setting of its own since sleep_manager v3 (wake_mv). Every
   voltage shows ONE decimal (Ali). The capture machine (pwrStep) and the
   rule (pwrRecommend) are pure; the screen polls GET /api/battery once a
   second (the sampler refreshes every poll_s from an 8-sample burst). */
const r1=x=>Math.round(x*10+1e-6)/10;
const fmtV=v=>(v==null||!isFinite(v))?"--.-":Number(v).toFixed(1);
const pwrLast=(p,k)=>p.trace.slice(-k).map(x=>x.v);
const mean=a=>a.reduce((x,y)=>x+y,0)/a.length;
const spread=a=>Math.max(...a)-Math.min(...a);
/* one reading in; returns true when the phase changed */
function pwrStep(p,v,now){
  if(v==null||!isFinite(v)){p.misses=(p.misses||0)+1;if(p.misses>=5&&!p.trace.length&&p.phase==="running"){p.phase="nobatt";return true;}return false;}
  p.misses=0;
  p.trace.push({t:now,v});if(p.trace.length>90)p.trace.shift();
  if(p.base==null&&p.trace.length>=3)p.base=mean(pwrLast(p,3));
  if(p.phase==="nobatt"){if(v>=11){p.phase="running";p.trace=[{t:now,v}];p.base=null;return true;}return false;}
  if(p.phase==="running"){
    if(p.trace.length>=3&&mean(pwrLast(p,3))<9){p.phase="nobatt";return true;}
    if(p.trace.length>=6){const a=pwrLast(p,6);
      /* charging = stable and clearly above a resting battery; a smart alternator
         charges lower: a climb of 0.4 V from the first readings, then 12 s stable */
      const lowCharge=p.trace.length>=12&&p.base!=null&&spread(pwrLast(p,12))<0.1&&mean(a)>=p.base+0.4&&mean(a)>=12.6;
      if((spread(a)<0.15&&mean(a)>=13.3)||lowCharge){p.charging=mean(a);p.tC=now;p.phase="charged";return true;}}
    return false;
  }
  if(p.phase==="charged"){
    const a=pwrLast(p,2);
    if(a.length===2&&Math.max(...a)<p.charging-0.5){p.phase="dropping";p.tD=now;return true;}
    return false;
  }
  if(p.phase==="dropping"){
    const a=pwrLast(p,10);
    if(a.length===10&&((spread(a)<0.04&&mean(a)<p.charging-0.4)||now-p.tD>=60000)){p.resting=mean(pwrLast(p,5));p.tR=now;p.phase="rested";return true;}
    return false;
  }
  return false;
}
/* a little above resting for sleep, a little above sleep but well below
   charging for wake; narrow margins are flagged, never hidden */
function pwrRecommend(p,rng){
  const gap=p.charging-p.resting;
  let sleep=r1(p.resting+(gap<0.5?0.2:0.3));
  let wake=gap<0.5?r1(sleep+0.1):Math.min(r1(sleep+0.2),r1(p.charging-0.4));
  if(wake<sleep+0.1)wake=r1(sleep+0.1);
  sleep=Math.min(rng.sMax,Math.max(rng.sMin,sleep));wake=Math.min(rng.wMax,Math.max(rng.wMin,wake));
  if(wake<sleep+0.1)wake=r1(sleep+0.1);
  p.sleep=sleep;p.wake=wake;p.touched=false;p.narrow=pwrNarrow(p);
}
const pwrNarrow=p=>p.charging!=null&&p.resting!=null&&(r1(p.charging-p.wake)<=0.3||r1(p.sleep-p.resting)<0.2);
function pwrChip(p){
  if(p.phase==="nobatt")return chip("No battery","warn");
  if(p.phase==="running")return chip("Waiting for the engine","");
  if(p.phase==="charged")return chip("Charging","ok");
  if(p.phase==="dropping")return chip("Dropping","warn");
  return chip("Resting","ok");
}
/* the live trace: the last 90 readings, grid at whole volts, the two
   thresholds dashed with labels, the captured readings marked */
function pwrSvg(p){
  const W=600,H=150,L=34,R=92,T=12,B=138;
  const usb=p.phase==="nobatt";const lo=usb?0:11.5,hi=15;
  const y=v=>B-(Math.min(hi,Math.max(lo,v))-lo)/(hi-lo)*(B-T);
  const n=90,x=i=>L+(i/(n-1))*(W-L-R),off=n-p.trace.length;
  let s='<svg viewBox="0 0 '+W+' '+H+'" role="img" aria-label="Battery voltage, last 90 seconds">';
  for(const g of(usb?[0,5,10,15]:[12,13,14,15]))s+='<line class="g" x1="'+L+'" x2="'+(W-R)+'" y1="'+y(g).toFixed(1)+'" y2="'+y(g).toFixed(1)+'"/><text x="'+(L-6)+'" y="'+(y(g)+3.5).toFixed(1)+'" text-anchor="end">'+g+' V</text>';
  if(p.trace.length>1){const pts=p.trace.map((r,i)=>x(off+i).toFixed(1)+","+y(r.v).toFixed(1));
    s+='<polygon class="f" points="'+x(off).toFixed(1)+','+B+' '+pts.join(" ")+' '+x(n-1).toFixed(1)+','+B+'"/><polyline class="l" points="'+pts.join(" ")+'"/>';}
  const mark=(t,v)=>{const i=p.trace.findIndex(r=>r.t===t);return i>=0?'<circle class="m" cx="'+x(off+i).toFixed(1)+'" cy="'+y(v).toFixed(1)+'" r="4"/>':"";};
  if(p.charging!=null&&p.phase!=="running")s+=mark(p.tC,p.charging);
  if(p.phase==="rested"){s+=mark(p.tR,p.resting);
    for(const [k,v] of [["sleep",p.sleep],["wake",p.wake]]){const yy=y(v).toFixed(1);
      s+='<line class="t '+k+'" x1="'+L+'" x2="'+(W-R)+'" y1="'+yy+'" y2="'+yy+'"/><text class="'+k+'" x="'+(W-R+6)+'" y="'+(Number(yy)+(k==="sleep"?10:-3)).toFixed(1)+'">'+k+' '+fmtV(v)+' V</text>';}}
  if(p.trace.length){const r=p.trace[p.trace.length-1];s+='<circle class="m e" cx="'+x(n-1).toFixed(1)+'" cy="'+y(r.v).toFixed(1)+'" r="3.5"/>';}
  return s+'</svg>';
}
function pwrWarn(p){
  if(p.phase!=="rested")return null;
  /* a slider dragged into a mistake is named first; the car's own small margins after */
  if(p.touched&&p.wake>p.charging-0.2)return banner("warn","alert",h("b",{},"Wake is above your charging voltage. "),"WiCAN would never wake up while you drive. Keep it at least 0.3 V under "+fmtV(p.charging)+" V.");
  if(p.touched&&p.sleep<=p.resting)return banner("warn","alert",h("b",{},"Sleep is below your resting voltage. "),"Parked, the battery stays above "+fmtV(p.sleep)+" V and WiCAN would never sleep.");
  if(p.narrow)return banner("warn","alert",h("b",{},"Small margins on this car. "),"It charges at only "+fmtV(p.charging)+" V, close to its resting "+fmtV(p.resting)+" V; a smart alternator does this once the battery is full. WiCAN can still tell parked from driving with these values, but keep the sleep delay at 5 minutes or more. If WiCAN ever falls asleep while you drive, lower both voltages a little; if it never sleeps, raise the sleep voltage.");
  return null;
}
SCREENS.power=async()=>{
  if(D.sleep===undefined)D.sleep=await tryGet("/api/settings/sleep_manager");
  if(D.sleepSchema===undefined)D.sleepSchema=await tryGet("/api/settings/sleep_manager/schema");
  const props=(D.sleepSchema&&D.sleepSchema.properties)||{};
  const lim=(k,dmin,dmax)=>{const q=props[k]||{};return [q.minimum!=null?q.minimum/1000:dmin,q.maximum!=null?q.maximum/1000:dmax];};
  const [sMin,sMax]=lim("sleep_mv",12,14),[wMin,wMax]=lim("wake_mv",12.1,15);
  const rng={sMin,sMax,wMin,wMax};
  const hasWake=!!props.wake_mv; /* older firmware derives wake = sleep + 0.1 V */
  const devSleep=D.sleep&&D.sleep.sleep_mv>0?D.sleep.sleep_mv/1000:13.1;
  const devWake=D.sleep&&D.sleep.wake_mv>0?D.sleep.wake_mv/1000:r1(devSleep+0.1);
  const delayMin=(D.sleep&&D.sleep.sleep_delay_min>0)?D.sleep.sleep_delay_min:5;
  const holdTxt=((D.sleep&&D.sleep.wake_delay_ms>0?D.sleep.wake_delay_ms:500)/1000).toFixed(1).replace(/\.0$/,"")+" s";
  const p=W.pwr;
  if(p.phase==="idle"){p.phase="running";p.trace=[];p.base=null;p.charging=null;p.resting=null;p.sleep=devSleep;p.wake=devWake;p.touched=false;p.narrow=false;}
  const body=h("div",{class:"qs-body"});
  let bigEl,chipEl,traceEl,sleepVal,wakeVal,sleepIn,wakeIn,sumEl,warnEl,recoEl;
  const summary=()=>["WiCAN sleeps once the battery has stayed below ",h("b",{},fmtV(p.sleep)+" V")," for "+delayMin+" minute"+(delayMin===1?"":"s")+" and wakes as soon as it rises above ",h("b",{},fmtV(p.wake)+" V"),". Margins on your car: "+fmtV(p.sleep-p.resting)+" V above resting, "+fmtV(p.charging-p.wake)+" V below charging."];
  const slide=(k,val)=>{
    p.touched=true;
    if(k==="sleep"){p.sleep=r1(val);if(p.wake<p.sleep+0.1)p.wake=r1(p.sleep+0.1);}
    else p.wake=Math.max(r1(val),r1(p.sleep+0.1));
    p.narrow=pwrNarrow(p);paintLive();
    if(recoEl&&!recoEl.querySelector("button"))recoEl.replaceChildren(btn("Back to the recommendation",()=>{pwrRecommend(p,rng);render();},"sm"));
  };
  const paintLive=()=>{
    const v=p.trace.length?p.trace[p.trace.length-1].v:null;
    if(bigEl)bigEl.replaceChildren(fmtV(v),h("small",{},"V"));
    if(chipEl)chipEl.replaceChildren(pwrChip(p));
    if(traceEl)traceEl.innerHTML=pwrSvg(p);
    if(sleepVal)sleepVal.textContent=fmtV(p.sleep)+" V";
    if(wakeVal)wakeVal.textContent=fmtV(p.wake)+" V";
    if(sleepIn&&Number(sleepIn.value)!==p.sleep)sleepIn.value=p.sleep;
    if(wakeIn&&Number(wakeIn.value)!==p.wake)wakeIn.value=p.wake;
    if(sumEl&&p.phase==="rested")sumEl.replaceChildren(...summary());
    if(warnEl){const w=pwrWarn(p);warnEl.replaceChildren(w||"");}
  };
  const useNow=()=>{
    if(p.trace.length<3)return;
    const now=Date.now();
    if(p.phase==="running"){p.charging=mean(pwrLast(p,3));p.tC=now;p.phase="charged";}
    else if(p.phase==="charged"){p.phase="dropping";p.tD=now;}
    else if(p.phase==="dropping"){p.resting=mean(pwrLast(p,3));p.tR=now;p.phase="rested";pwrRecommend(p,rng);}
    render();
  };
  const right=(...els)=>h("div",{style:"display:flex;gap:8px;align-items:center;flex-wrap:wrap;justify-content:flex-end"},...els);
  const useBtn=()=>{const b=btn("Use the current reading",useNow,"sm");b.id="qs-pwr-use";return b;};
  const render=()=>{
    const out=[];
    const v=p.trace.length?p.trace[p.trace.length-1].v:null;
    bigEl=h("div",{class:"big",id:"qs-pwr-big"},fmtV(v),h("small",{},"V"));
    chipEl=h("div",{class:"st",id:"qs-pwr-state"},pwrChip(p));
    traceEl=h("div",{class:"qs-trace",id:"qs-pwr-trace"});traceEl.innerHTML=pwrSvg(p);
    out.push(h("div",{class:"qs-live"},h("div",{},h("div",{class:"lbl"},"Battery now"),bigEl,chipEl,h("div",{class:"help",style:"font-size:11.5px;margin-top:10px;color:var(--text-3)"},"One reading a second, the last 90 s shown.")),traceEl));
    if(p.phase==="nobatt"){
      out.push(banner("info","info",h("b",{},"WiCAN is not seeing a car battery"),v==null?" (no reading yet)":" ("+fmtV(v)+" V)",". It is probably powered over USB on a desk. Keep the current values for now (sleep below "+fmtV(devSleep)+" V, wake above "+fmtV(devWake)+" V) and run Quick Setup again once it is in the car: this step takes about a minute there."));
    }else{
      const stA=p.phase==="running"?"run":"ok";
      const stB=p.phase==="running"?"":p.phase==="rested"?"ok":"run";
      out.push(sub("1. Engine running"));
      out.push(check(stA,"check","Start the engine",["Or put an EV or hybrid in Ready. WiCAN waits for the charging voltage to settle; most cars charge the 12 V battery at 13.8 to 14.8 V. If yours charges lower (a smart alternator), press ",h("strong",{},"Use the current reading")," once the engine runs."],
        stA==="ok"?chip(fmtV(p.charging)+" V charging","ok"):right(chip(p.base!=null&&v!=null&&v>p.base+0.3?"Settling":"Waiting for the engine",""),useBtn())));
      out.push(sub("2. Engine off"));
      const rowB=check(stB,"info","Turn the engine off and wait about half a minute","Leave the doors closed and everything switched off. The voltage drops at once, then settles as the battery rests. WiCAN notices the drop by itself.",
        stB==="ok"?chip(fmtV(p.resting)+" V resting","ok"):stB==="run"?right(chip(p.phase==="dropping"?"Dropping":"Waiting for the engine to stop",""),useBtn()):h("span",{}));
      if(stB==="")rowB.style.opacity=".55";
      out.push(rowB);
      if(p.phase==="rested"){
        out.push(sub("3. The two voltages"));
        warnEl=h("div",{id:"qs-pwr-warn"});const w=pwrWarn(p);if(w)warnEl.append(w);out.push(warnEl);
        sleepVal=h("span",{class:"val",id:"qs-pwr-sleep-val"},fmtV(p.sleep)+" V");
        sleepIn=h("input",{type:"range",id:"qs-pwr-sleep",min:sMin,max:sMax,step:0.1,value:p.sleep,"aria-label":"Sleep voltage",oninput:e=>slide("sleep",Number(e.target.value))});
        wakeVal=h("span",{class:"val",id:"qs-pwr-wake-val"},fmtV(p.wake)+" V");
        wakeIn=h("input",{type:"range",id:"qs-pwr-wake",min:wMin,max:wMax,step:0.1,value:p.wake,disabled:!hasWake,"aria-label":"Wake voltage",oninput:e=>slide("wake",Number(e.target.value))});
        out.push(h("div",{class:"qs-thr"},
          h("div",{class:"qs-thr-row"},h("b",{},h("i",{style:"background:var(--warning)"}),"Sleep below"),sleepVal,sleepIn,
            h("span",{},"Parked, the battery rests at about ",h("b",{},fmtV(p.resting)+" V")," on your car. Once it has stayed below this voltage for "+delayMin+" minute"+(delayMin===1?"":"s")+", WiCAN sleeps.")),
          h("div",{class:"qs-thr-row"},h("b",{},h("i",{style:"background:var(--success)"}),"Wake above"),wakeVal,wakeIn,
            h("span",{},hasWake?["Driving, the engine charges at about ",h("b",{},fmtV(p.charging)+" V"),". Above this voltage for "+holdTxt+", WiCAN wakes up and starts polling again."]:"This firmware derives the wake voltage: 0.1 V above sleep."))));
        sumEl=h("p",{class:"qs-lead",id:"qs-pwr-sum",style:"font-size:13px"},...summary());out.push(sumEl);
        recoEl=h("div",{id:"qs-pwr-reco",style:"display:flex;gap:8px;align-items:center;flex-wrap:wrap"},
          p.touched?btn("Back to the recommendation",()=>{pwrRecommend(p,rng);render();},"sm"):h("span",{class:"help",style:"font-size:12.5px"},"Recommended from your two readings: a little above resting for sleep, well below charging for wake. Drag either slider to change it."));
        out.push(recoEl);
        out.push(note("","Both voltages and the "+delayMin+" minute delay live under Power Saving afterwards. Power Saving is switched on when you continue. WiCAN only reads the voltage; it never switches anything in the car off."));
      }else{
        out.push(note("","Why measure: with the wrong voltages WiCAN either never sleeps and slowly drains the 12 V battery, or falls asleep while you drive and your data stops. One minute here settles both."));
      }
    }
    body.replaceChildren(...out);
  };
  const tick=async()=>{
    const b=await tryGet("/api/battery");
    const v=b&&typeof b.voltage==="number"?b.voltage:null;
    if(!body.isConnected)return;
    if(pwrStep(p,v,Date.now())){if(p.phase==="rested")pwrRecommend(p,rng);render();}
    else paintLive();
  };
  render();
  await tick();
  every(1000,tick);
  const keepLabel=p.phase==="nobatt"?"Continue with the current values":"Skip, keep the defaults ("+fmtV(devSleep)+" / "+fmtV(devWake)+" V)";
  const keep=btn(keepLabel,()=>{p.skipped=true;p.measured=false;go("polling");},p.phase==="nobatt"?"pri":"",{title:"Keep the sleep and wake voltages as they are; you can measure them later under Power Saving"});keep.id="qs-pwr-keep";
  const cont=p.phase==="nobatt"?null:btn("Continue",()=>{p.measured=true;p.skipped=false;go("polling");},"pri",{disabled:p.phase!=="rested"});if(cont)cont.id="qs-pwr-continue";
  /* the footer follows the phase (Continue unlocks once both readings are in) */
  const refreshFoot=()=>{if(cont)cont.disabled=p.phase!=="rested";};
  every(1000,refreshFoot);
  return screen("When should WiCAN sleep?","WiCAN watches your car's 12 V battery. With the engine running the battery is being charged and its voltage is high; parked, it rests lower. WiCAN sleeps once the voltage has stayed below the sleep voltage for "+delayMin+" minute"+(delayMin===1?"":"s")+", and wakes when it rises above the wake voltage. Measure both on your car in about a minute.",
    [body],[back("vehicle"),grow(),keep,cont]);
};

/* ---- step 11: how WiCAN reads the car (the autopid polling rules, 2026-10-01) ----
   Plain-language versions of the Automate settings that decide how much data
   the user gets and what happens when the car is parked. Prefilled from the
   device; staged with the vehicle step's Finish (one restart). The poll rate
   is the default group's period in the LIVE config; the rest is settings. */
SCREENS.polling=async()=>{
  if(!D.autopid){D.autopid=strip(await tryGet("/api/settings/autopid"));D.autopidSchema=await tryGet("/api/settings/autopid/schema");}
  if(D.sleep===undefined)D.sleep=await tryGet("/api/settings/sleep_manager");
  if(D.cfg==null)D.cfg=await tryGet("/api/autopid/config");
  const maxName=(D.autopidSchema&&D.autopidSchema.properties&&D.autopidSchema.properties.vehicle&&D.autopidSchema.properties.vehicle.maxLength)||63;
  const a=D.autopid||{};
  /* prefill once from the device */
  if(!W.pollSeeded){
    const g=(D.cfg&&Array.isArray(D.cfg.groups)&&D.cfg.groups.length)?D.cfg.groups[0]:null;
    const sec=g&&g.period_ms?Math.round(g.period_ms/1000):0;
    const p=W.poll;
    /* a device that already polls keeps its rate; a fresh one (polling off,
       the firmware's 1 s default group) gets the recommended 5 s */
    if(a.enabled){if([1,2,5,10].includes(sec))p.rate=sec;else if(sec>0){p.rate="custom";p.rateCustom=sec;}}
    if(a.min_event_interval_ms)p.minEvent=Math.max(0.1,a.min_event_interval_ms/1000);
    if(a.pause_below_mv>0){p.pause="custom";p.pauseV=a.pause_below_mv/1000;}
    else p.pause=a.pause_follow_sleep===false?"never":"sleep";
    p.pauseAll=a.pause_mode==="all";
    if(typeof a.std_enabled==="boolean")p.std=a.std_enabled;
    if(typeof a.specific_enabled==="boolean")p.specific=a.specific_enabled;
    p.dtc=!!a.dtc_enabled;if(a.dtc_scan_period_min>0)p.dtcMin=a.dtc_scan_period_min;
    W.pollSeeded=true;
  }
  const p=W.poll;
  const devSleepV=D.sleep&&D.sleep.sleep_mv>0?D.sleep.sleep_mv/1000:13.1;
  const sleepV=fmtV(W.pwr.measured?W.pwr.sleep:devSleepV)+" V";
  const wakeV=fmtV(W.pwr.measured?W.pwr.wake:(D.sleep&&D.sleep.wake_mv>0?D.sleep.wake_mv/1000:r1(devSleepV+0.1)))+" V";
  const hasProfile=!!(W.profChoice==="car"&&W.profCar)||!!(W.veh&&W.veh.known&&W.veh.profile&&W.profChoice!=="none");
  const err=h("div",{});
  const body=h("div",{class:"qs-body"});
  const finish=btn("Finish and restart",()=>finishVehicle(finish,err,maxName),"pri",{icon:"power"});
  const render=()=>{
    const out=[];
    const sw=(id,label,desc,on,disabled,onchange,tail)=>h("div",{class:"qs-check",style:"grid-template-columns:minmax(0,1fr) auto"},
      h("div",{},h("b",{},label),h("span",{class:"d2"},desc),tail||null),
      h("label",{class:"switch",title:disabled?"Not available":""},h("input",{type:"checkbox",id,checked:on,disabled,onchange:e=>onchange(e.target.checked)}),h("span",{class:"sw"}),h("span",{class:"swl"},disabled?"n/a":on?"On":"Off")));
    out.push(sub("1. How often"));
    const rates=[[1,"Every second","For live driving data and dashboards. Keeps the car's modules busiest."],[2,"Every 2 seconds","A good middle ground when you watch values while driving."],[5,"Every 5 seconds (recommended)","Plenty for Home Assistant and MQTT dashboards, gentle on the car."],[10,"Every 10 seconds","Trip logging, charging state, long-term monitoring."]];
    const rateBox=h("div",{class:"qs-radios",style:"gap:8px"});
    rates.forEach(([v,t,d])=>rateBox.append(radio("qs-rate","qs-rate-"+v,p.rate===v,t,d,()=>{p.rate=v;render();})));
    const custIn=h("input",{type:"number",min:1,max:600,value:p.rateCustom,style:"width:80px;display:inline;padding:3px 6px",onchange:e=>{p.rateCustom=Math.max(1,Math.min(600,Number(e.target.value)||1));p.rate="custom";render();}});
    rateBox.append(radio("qs-rate","qs-rate-custom",p.rate==="custom","Custom",["Every ",custIn," seconds"],()=>{p.rate="custom";render();}));
    out.push(rateBox);
    out.push(note("","WiCAN asks the car for every parameter at this rate. Faster shows changes sooner but keeps the modules busier and the bus noisier. A profile can still give single values their own rate later, under Automate > Parameters."));
    const evIn=h("input",{id:"qs-minevent",type:"number",min:0.1,max:600,step:0.1,value:p.minEvent,style:"max-width:110px",onchange:e=>{p.minEvent=Math.max(0.1,Math.min(600,Number(e.target.value)||1));}});
    out.push(h("div",{class:"qs-row",style:"margin-top:4px"},h("label",{for:"qs-minevent"},"Report a change at most every"),h("div",{class:"ctl"},evIn,h("span",{},"seconds")),
      h("div",{class:"help"},"Limits how fast value changes are pushed to Home Assistant, MQTT and the rules. Slower than the poll rate it never does anything.")));
    out.push(sub("2. When the car is off"));
    const pauseBox=h("div",{class:"qs-radios",style:"gap:8px"});
    pauseBox.append(radio("qs-pause","qs-pause-sleep",p.pause==="sleep","Pause with Power Saving (recommended)","Requests stop when the 12 V battery drops below the Power Saving sleep voltage ("+sleepV+(W.pwr.measured?", measured on your car":"")+": engine off) and resume above the wake-up voltage ("+wakeV+"). The car's modules can sleep and the battery stays charged.",()=>{p.pause="sleep";render();}));
    const slider=h("input",{type:"range",min:12,max:14.5,step:0.1,value:p.pauseV,style:"width:260px;max-width:100%;vertical-align:middle",oninput:e=>{p.pauseV=Number(e.target.value);vLbl.textContent=p.pauseV.toFixed(1)+" V";},onchange:()=>{p.pause="custom";render();}});
    const vLbl=h("span",{class:"mono"},p.pauseV.toFixed(1)+" V");
    pauseBox.append(radio("qs-pause","qs-pause-custom",p.pause==="custom","Pause below a voltage I choose",["Stops below the voltage you set, resumes 0.3 V above it. ",h("br"),slider," ",vLbl],()=>{p.pause="custom";render();}));
    pauseBox.append(radio("qs-pause","qs-pause-never",p.pause==="never","Never pause","WiCAN keeps asking while the car is parked.",()=>{p.pause="never";render();}));
    out.push(pauseBox);
    if(p.pause==="never")out.push(banner("warn","alert",h("b",{},"This keeps the car's modules awake. "),"With the engine off for days it can drain the 12 V battery. Pick this only for a bench or a car on a charger."));
    out.push(sw("qs-pauseall","While paused, also stop listening","Off: the J1939 rows, which are read from WiCAN's own CAN listener and transmit nothing, keep running while requests are paused. On: they stop too and nothing is read at all.",p.pauseAll,false,v=>{p.pauseAll=v;render();}));
    out.push(sub("3. What to read"));
    const nSup=(W.veh&&W.veh.std_supported)||0,nStd=(W.stdPick||W.stdDefault||[]).length;
    if(!nStd)p.std=false;
    /* a choice made on the vehicle step this session reads (once per pick; the
       device's switch may well be off after an earlier "none chosen", 2026-10-09) */
    else if(W.stdPick&&W.stdPick.length&&p.stdPickedFor!==W.stdPick){p.std=true;p.stdPickedFor=W.stdPick;}
    out.push(sw("qs-std","Standard PIDs",nStd?"The "+nStd+" of the "+nSup+" the scan found that you chose on the vehicle step.":"None chosen on the vehicle step (the scan found "+nSup+"). Go back to choose some, or add them later under Automate > Parameters.",p.std,!nStd,v=>{p.std=v;render();}));
    out.push(sw("qs-specific","Vehicle-specific values",hasProfile?"Battery state of charge, charging power and the other values your profile knows how to ask for.":"No profile chosen for this car yet. Pick one on the previous step, or later under Automate > Parameters.",hasProfile&&p.specific,!hasProfile,v=>{p.specific=v;render();}));
    out.push(sub("4. Trouble codes"));
    const dtcIn=h("input",{id:"qs-dtcmin",type:"number",min:5,max:10080,value:p.dtcMin,style:"width:90px;display:inline;padding:3px 6px",onchange:e=>{p.dtcMin=Math.max(5,Math.min(10080,Number(e.target.value)||60));}});
    out.push(sw("qs-dtc","Check for trouble codes","Reads the stored fault codes (the check-engine light) and shows them under Trouble Codes and in Home Assistant. Clearing codes stays a manual action, off unless you allow it there.",p.dtc,false,v=>{p.dtc=v;render();},
      p.dtc?h("div",{style:"margin-top:8px;font-size:13px"},"Every ",dtcIn," minutes while the car is on"):null));
    out.push(err);
    body.replaceChildren(...out);
  };
  render();
  return screen("How WiCAN reads the car","These rules decide how much data you get and what happens when the car is parked. The defaults suit Home Assistant and MQTT; change them if you know what you want. Everything here can be changed later under Automate.",
    [body],[back("power"),grow(),finish]);
};

async function finishVehicle(finishBtn,errEl,maxName){
  finishBtn.disabled=true;errEl.replaceChildren();
  try{
    const v=W.veh||{};
    const car=W.profChoice==="car"?W.profCar:null;
    const none=W.profChoice==="none";
    const p=W.poll;
    const rateS=p.rate==="custom"?p.rateCustom:p.rate;
    /* 1. the car's entry: name + profile choice (the store applies the init live) */
    if(D.vehicles&&v.key){
      const bodyPut={name:W.vehName||defaultName(v)};
      if(car){bodyPut.profile=String(car.car_model||"").slice(0,maxName);bodyPut.specific_init=car.init||"";}
      else if(none){bodyPut.profile="";bodyPut.specific_init="";}
      await api("/api/autopid/vehicles/"+encodeURIComponent(v.key),{method:"PUT",body:bodyPut});
    }
    /* 2. the PID tables: the profile's rows replace the vehicle-specific ones and its
       passive CAN filters the last profile's (2026-10-27; hand-made filters stay); std +
       custom stay; the default group takes the chosen rate and the rows that sat on the
       old default inherit it */
    const cfg0=await tryGet("/api/autopid/config");
    const cfg=cfg0&&typeof cfg0==="object"?JSON.parse(JSON.stringify(cfg0)):{};
    cfg.pids=Array.isArray(cfg.pids)?cfg.pids:[];
    cfg.groups=Array.isArray(cfg.groups)&&cfg.groups.length?cfg.groups:[{name:"default",enabled_default:true,period_ms:1000}];
    cfg.filters=Array.isArray(cfg.filters)?cfg.filters:[];
    const oldPeriod=cfg.groups[0].period_ms||1000;
    if(car||none){
      const keep=cfg.pids.filter(pd=>pd.type!=="specific"),keepF=cfg.filters.filter(f=>!f.profile);
      if(car){const r=profileToPids(car,keep,keepF);cfg.pids=keep.concat(r.pids);cfg.filters=keepF.concat(r.filters);if(r.renamed.length)toast(r.renamed.length+" duplicate parameter name"+(r.renamed.length>1?"s":"")+" renamed","");}
      else{cfg.pids=keep;cfg.filters=keepF;}
    }
    /* the standard rows: the ones chosen on the vehicle step and no other
       (a new car starts with none: Ali, 2026-10-07). The firmware stored
       every row the scan found, a firmware without the store none: drop
       what was not chosen, add what is missing */
    const norm=c=>String(c||"").replace(/\s+/g,"").toUpperCase();const want=new Set(W.stdPick||W.stdDefault||[]);
    if(W.scanResult&&W.scanResult.supported){
      cfg.pids=cfg.pids.filter(pd=>pd.type!=="std"||want.has(norm(pd.cmd)));
      const have=new Set(cfg.pids.map(x=>norm(x.cmd)));
      for(const rw of W.scanResult.supported){const c=norm(rw.cmd);if(!want.has(c)||have.has(c))continue;have.add(c);
        cfg.pids.push({name:rw.name,cmd:rw.cmd,group:"default",period_ms:0,type:"std",parameters:(rw.parameters||[]).map(x=>({...x}))});}
    }else if(!want.size){
      /* no detection this session and nothing chosen (2026-10-09): on a car
         whose setup never finished the standard rows were stored by the
         device at its detection, nobody chose them: they leave too. A car
         set up before keeps its rows (an earlier choice). The store is read
         here when the wizard resumed past the vehicle step. */
      const vs=D.vehicles||(D.vehicles=await tryGet("/api/autopid/vehicles"));
      const cur=(vs&&Array.isArray(vs.vehicles))?vs.vehicles.find(x=>x&&x.current):null;
      if(cur&&cur.pending_profile===true)cfg.pids=cfg.pids.filter(pd=>pd.type!=="std");
    }
    /* a chosen row reads: the device stores a new car's rows off while the
       standard PIDs are off (2026-10-09), the tick turns them on */
    for(const pd of cfg.pids){if(pd.type==="std"&&want.has(norm(pd.cmd)))delete pd.enabled;}
    cfg.groups[0].period_ms=Math.max(1,rateS)*1000;
    for(const pd of cfg.pids){if(pd.period_ms===oldPeriod||pd.period_ms===1000)pd.period_ms=0;}
    /* no parameter name twice in what is sent: the firmware refuses a repeat
       (names key its cache), and a file written by an older table could carry
       one (OxySensor1_Volt under 0x14 and 0x24; the loader repairs those since
       2026-10-06, this is the belt): the later one gets _2, _3, ... */
    {const seen=new Set();let n=0;
     for(const pd of cfg.pids)for(const pr of (pd.parameters||[])){
       if(!pr.name)continue;
       if(seen.has(pr.name)){let k=2;while(seen.has(pr.name+"_"+k))k++;pr.name=pr.name+"_"+k;n++;}
       seen.add(pr.name);}
     if(n)toast(n+" repeated parameter name"+(n>1?"s":"")+" made unique (_2, _3)","");}
    await api("/api/autopid/config",{method:"PUT",body:cfg});
    /* 3. the settings: polling on, protocol follows the store, the reading rules, the profile name for the backup */
    const av=strip(D.autopid||await api("/api/settings/autopid"));
    av.enabled=true;av.std_protocol="0";
    av.std_enabled=!!p.std;
    if(car){av.vehicle=String(car.car_model||"").slice(0,maxName);av.specific_init=car.init||"";av.specific_enabled=!!p.specific;}
    else if(none){av.vehicle="";av.specific_init="";av.specific_enabled=false;}
    else if(v.known&&v.profile){av.specific_enabled=!!p.specific;}
    av.min_event_interval_ms=Math.round(Math.max(0.1,p.minEvent)*1000);
    av.pause_below_mv=p.pause==="custom"?Math.round(p.pauseV*1000):0;
    av.pause_follow_sleep=p.pause==="sleep";
    av.pause_mode=p.pauseAll?"all":"requests_only";
    av.dtc_enabled=!!p.dtc;
    av.dtc_scan_period_min=p.dtc?Math.max(5,p.dtcMin):0;
    store.stage("autopid",av);
    /* 3b. a J1939 network heard with the listener off: the native CAN controller
       listen-only (it never transmits) at the bitrate the detection measured, and
       the J1939 listener, in the same restart */
    if(v.j1939&&!v.j1939Listening){
      const cm=strip(await api("/api/settings/can_manager"));
      cm.enabled=true;cm.silent=true;
      const kb=String(v.busKbps||"");
      cm.baud=(cm.baud==="auto"||!kb)?"auto":kb;
      store.stage("can_manager",cm);
      const jl=strip(await api("/api/settings/j1939"));
      jl.enabled=true;
      store.stage("j1939",jl);
    }
    /* 4. the Battery and sleep step: the measured pair switches Power Saving on (wake_mv only
       when this firmware has it; an older one derives wake = sleep + 0.1 V) */
    if(W.pwr.measured){
      const sm=strip(D.sleep||await api("/api/settings/sleep_manager"));
      sm.enabled=true;sm.sleep_mv=Math.round(W.pwr.sleep*1000);
      if(D.sleepSchema&&D.sleepSchema.properties&&D.sleepSchema.properties.wake_mv)sm.wake_mv=Math.round(W.pwr.wake*1000);
      store.stage("sleep_manager",sm);
    }
    W.noVeh=false;W.cur="done";
    try{history.replaceState(null,"","#/setup/done");}catch(_){}
    paint();
    await store.commit("Quick Setup: applying vehicle settings");
  }catch(e){
    store.unstage("autopid");store.unstage("sleep_manager");store.unstage("can_manager");store.unstage("j1939");finishBtn.disabled=false;
    errEl.replaceChildren(banner("crit","alert",h("b",{},"WiCAN did not accept the vehicle setup: "),e.message||String(e)));
  }
}

SCREENS.done=async()=>{
  const body=h("div",{class:"qs-body"});
  const refresh=async()=>{
    const out=[];
    if(conn.state!=="online"){out.push(check("run",null,"Restarting","WiCAN applies the vehicle settings. About 15 s; this page waits."));}
    const[w,ap,vs,L]=await Promise.all([tryGet("/api/wifi/status"),tryGet("/api/settings/autopid"),tryGet("/api/autopid/vehicles"),usbLive()]);
    const v=vs&&Array.isArray(vs.vehicles)?(vs.vehicles.find(x=>x.current)||null):null;
    const b=(conn.status&&conn.status.bits)||{};
    const mqttOn=!!(D.mqtt&&D.mqtt.enabled)||W.use==="mqtt";
    const kv=[];
    kv.push(h("dt",{},"Reach WiCAN at"),h("dd",{},w&&w.ip?h("code",{},"http://"+w.ip):h("span",{},"its address on your network"),h("span",{class:"help"},"shown as "+routerName()+" in your router's device list")));
    kv.push(h("dt",{},"Access point"),h("dd",{},h("code",{},apName()),w&&w.ap_started===false?chip("Off","")
      :chip(w&&w.ap_default_password===false?"Your password":"Check the password",w&&w.ap_default_password===false?"ok":"warn")));
    {const us=L.kind!=="unknown"?usbSummary(L):null;
      if(us)kv.push(h("dt",{},"USB connector"),h("dd",{},chip(us.chip[0],us.chip[1]),h("span",{},us.title)));}
    if(mqttOn)kv.push(h("dt",{},"MQTT broker"),h("dd",{},chip(b.mqtt_connected?"Connected":"Not connected",b.mqtt_connected?"ok":"crit"),h("code",{},(D.mqtt&&D.mqtt.url)||W.mqtt.url||"")));
    else if(W.use!=="wifi"){const wh=await tryGet("/api/webhook");kv.push(h("dt",{},"Home Assistant"),h("dd",{},chip(wh&&wh.url?"Connected":"Waiting for discovery",wh&&wh.url?"ok":"warn")));}
    const vehOn=ap&&ap.enabled;
    kv.push(h("dt",{},"Vehicle"),h("dd",{},!vehOn?chip("Not set up",""):[v&&v.name?h("b",{},v.name):null,(v&&v.profile)||ap.vehicle?h("code",{},(v&&v.profile)||ap.vehicle):chip("Standard PIDs only",""),
      v&&v.vin?h("span",{},"VIN ",h("code",{},v.vin)):null,v&&v.dialect==="j1939"?h("span",{},"SAE J1939, listening"):v&&PROTO[String(v.protocol)]?h("span",{},PROTO[String(v.protocol)]+(DIALECT[v.dialect]?", "+DIALECT[v.dialect]:"")+(v.j1939?", J1939 network":"")):(ap.std_protocol==="0"?chip("Protocol: follows the car",""):null),
      v&&v.pending_profile?chip("Profile pending","warn"):null]));
    if(vehOn&&!W.noVeh){const pp=W.poll;const rate=pp.rate==="custom"?pp.rateCustom:pp.rate;
      const pauseTxt=pp.pause==="sleep"?"pauses with Power Saving":pp.pause==="custom"?"pauses below "+pp.pauseV.toFixed(1)+" V":"never pauses";
      kv.push(h("dt",{},"Reading the car"),h("dd",{},h("span",{},"every "+rate+" s, "+pauseTxt+(pp.dtc?", trouble codes every "+pp.dtcMin+" min":""))));}
    if(vehOn&&!W.noVeh&&W.pwr.measured){const dm=(D.sleep&&D.sleep.sleep_delay_min>0)?D.sleep.sleep_delay_min:5;
      kv.push(h("dt",{},"Battery and sleep"),h("dd",{},h("span",{},"sleeps after "+dm+" min below ",h("b",{},fmtV(W.pwr.sleep)+" V"),", wakes above ",h("b",{},fmtV(W.pwr.wake)+" V")),chip("Measured on your car","ok")));}
    out.push(h("dl",{class:"qs-kv"},...kv));
    if(W.pwr.skipped&&!W.noVeh){const sl=await tryGet("/api/sleep");
      out.push(note("",["Power Saving keeps its current values"+(sl&&sl.sleep_v?" (sleep below "+fmtV(sl.sleep_v)+" V, wake above "+fmtV(sl.wake_v)+" V)":"")+". If WiCAN sleeps while you drive or never sleeps when parked, run Quick Setup again and measure your car on the Battery and sleep step."]));}
    if(conn.sleepEnabled===false&&!W.pwr.measured)out.push(banner("warn","alert",h("b",{},"Power saving is off. "),"WiCAN stays awake and can drain the vehicle battery when the car is parked for days. Turn on sleep under Power Saving when you are happy with the setup."));
    out.push(sub("Where to go next"));
    out.push(h("div",{class:"qs-next"},
      h("a",{href:"#/dashboard"},h("b",{},"Dashboard"),h("span",{},"Live values, one tile per parameter.")),
      h("a",{href:"#/automate/parameters"},h("b",{},"Automate > Parameters"),h("span",{},"The PID lists, groups and poll rates.")),
      h("a",{href:"#/settings/wifi"},h("b",{},"Settings"),h("span",{},"WiFi, access point, Bluetooth, MQTT.")),
      h("a",{href:"#/power"},h("b",{},"Power Saving"),h("span",{},"Sleep when the car is off."))));
    out.push(note("","Run Quick Setup again any time: it is the first entry in the sidebar, and under System > Maintenance."));
    body.replaceChildren(...out);
  };
  await refresh();every(3000,refresh);
  return screen("WiCAN is set up",W.noVeh?"WiCAN is on your WiFi and secured. Vehicle data can be added any time from Automate or by running Quick Setup again.":"AutoPID is on. Values appear on the Dashboard as soon as the car is on.",
    [body],[grow(),btn("Open the Dashboard",()=>{location.hash="#/dashboard";},"pri")]);
};

/* ---------- the page ---------- */
PAGES.__setup=async(view,subId)=>{
  if(!document.getElementById("qs-css"))document.head.append(h("style",{id:"qs-css"},CSS));
  const p=page(view,null,"Quick Setup","Gets a new WiCAN onto your home WiFi, talking to Home Assistant or your MQTT broker, reading your vehicle and sleeping when the car is parked. About six minutes.",
    [h("button",{class:"btn sm gh",type:"button",onclick:()=>{location.hash="#/status";}},"Exit setup")]);
  railEl=h("aside",{class:"qs-rail","aria-label":"Setup steps"});
  scrEl=h("section",{class:"qs-screen","aria-live":"polite"});
  p.append(h("div",{class:"qs"},railEl,scrEl));
  if(IDS.includes(subId))W.cur=subId;
  else if(!W.cur)W.cur="safety";
  {const seg=(location.hash.split("/")[3]||"").toLowerCase();if(seg==="wifi"||seg==="mqtt"||seg==="ha")W.use=seg;}   /* the resume link's use case */
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  paint();
  return ()=>{stopTimers();railEl=null;scrEl=null;};
};

/* ---------- the ESPNetLink screen on its own (2026-10-13): the USB step with
   its own Save, reached from the banner, Settings > USB and #/espnetlink ---------- */
PAGES.__espnetlink=async(view)=>{
  if(!document.getElementById("qs-css"))document.head.append(h("style",{id:"qs-css"},CSS));
  if(!document.getElementById("qs-alone-css"))document.head.append(h("style",{id:"qs-alone-css"},".qs.qs-single{grid-template-columns:minmax(0,1fr);max-width:820px}.qs-offer{margin-bottom:2px}"));
  const p=page(view,null,"ESPNetLink","What WiCAN found on the cable and how it uses it. USB Ethernet needs nothing from you; the carrier settings and the GPS choice are here when they matter.",
    [h("button",{class:"btn sm gh",type:"button",onclick:()=>{location.hash="#/usb";}},"Settings > USB")]);
  railEl=null;scrEl=h("section",{class:"qs-screen","aria-live":"polite"});
  p.append(h("div",{class:"qs qs-single"},scrEl));
  W.cur="usb";W.alone=true;
  if(!D.wifi)D.wifi=strip(await tryGet("/api/settings/wifi_manager"));
  scrEl.replaceChildren(await SCREENS.usb());
  return ()=>{stopTimers();scrEl=null;W.alone=false;};
};
})();
