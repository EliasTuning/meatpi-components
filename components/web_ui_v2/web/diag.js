/* The OBD chip and the CAN self-test cards of System > Maintenance (issue
   #1023, 2026-10-28; Ali's placement). An on-demand chunk (/ui/diag.js):
   PAGES.system loads it into the Maintenance pane after the Maintenance
   card. No setting lives here (the obd_chip knobs stay API/CLI only).

   1. OBD chip: the chip's state from /api/obd_chip every 3 s (the chip's
      firmware named by its version alone, V2.3.24; the chip's own words
      stay in the API's fw.chip: Ali, 2026-11-01), a banner with the
      bring-up's wire verdicts when it is down, the update offer while the
      chip's firmware is older than the device's image (a confirm dialog,
      then POST /api/obd_chip/update: there is no forced rewrite any more,
      the device writes a chip only when its image is newer), the chip
      configuration report (GET /api/obd_chip/report: one block to copy into
      a bug report), and the wire tools folded away (POST /api/obd_chip/probe,
      polled on GET; the chip's log lines of the last start from the ring).
   2. CAN self-test: the preconditions, Run (POST /api/can/selftest, polled on
      GET), the verdict and the result as one copyable block. */
(()=>{
const mono=(t)=>h("span",{class:"mono"},t);
const fmtBaud=(b)=>b?String(b).replace(/\B(?=(\d{3})+(?!\d))/g," "):"-";
const stamp=()=>{const d=new Date();return [d.getHours(),d.getMinutes(),d.getSeconds()].map(x=>String(x).padStart(2,"0")).join(":");};
const copyBox=(text,rows)=>{
  /* plain http has no clipboard API: a selectable box plus a Select button
     that also tries the old copy command (UI audit rule) */
  const ta=h("textarea",{class:"mono",readonly:true,rows:rows||10,style:"width:100%;box-sizing:border-box;font-size:12px;white-space:pre;background:var(--term-bg);color:var(--term-text);border:1px solid var(--border);border-radius:9px;padding:10px"},text);
  const btn=h("button",{class:"btn sm gh",type:"button",onclick:()=>{ta.focus();ta.select();try{document.execCommand("copy");toast("Selected; copied where the browser allows it");}catch{toast("Selected: press Ctrl+C");}}},"Select all");
  return {ta,btn};
};
const details=(title,...kids)=>h("details",{style:"border:1px solid var(--border);border-radius:9px;margin-top:10px"},
  h("summary",{style:"cursor:pointer;padding:9px 12px;font-weight:600;color:var(--primary)"},title),
  h("div",{style:"padding:4px 12px 12px;border-top:1px solid var(--border);display:flex;flex-direction:column;gap:10px"},...kids));
/* a text-plus-action row: the text may wrap, the button stays at the right
   edge (Ali, 2026-10-11: a card's action never drops under its text) */
const actionRow=(text,btn,extra)=>h("div",{style:"display:grid;grid-template-columns:minmax(0,1fr) auto;align-items:center;gap:10px;"+(extra||"")},text,btn);

/* ---- 1. the OBD chip card ------------------------------------------------- */
async function chipCard(){
  const stateRow=h("div",{class:"rowflex",style:"gap:8px;flex-wrap:wrap"});
  const noteRow=h("div",{});
  const fwRow=h("div",{});
  let st=null,timer=null;
  /* the version alone (V2.3.24) out of the chip's answer (MIC3624 V2.3.24):
     the card names versions, the API keeps the chip's words (Ali, 2026-11-01) */
  const verOf=(s)=>{const m=/V\d+\.\d+\.\d+/.exec(s||"");return m?m[0]:"";};
  const wireWords=(w)=>w.map(x=>"at "+fmtBaud(x.baud)+" baud "+(x.bytes?x.bytes+" byte(s) without a prompt ("+x.hex+(x.bytes>12?" ...":"")+")":"nothing")).join("; ");
  /* the update: offered only while the chip's firmware is older than the
     device's image; a confirm dialog (Ali, 2026-10-28), then the job. No
     forced rewrite any more (Ali, 2026-11-01): the device writes a chip
     only when its image is newer, never the same version, never older. */
  const updBtn=h("button",{class:"btn pri sm",type:"button",onclick:async()=>{
    const to=(st&&st.fw&&st.fw.builtin)||"the packaged image";
    if(!await confirmModal("Update the chip firmware to "+to+"? About 20 seconds; AutoPID and apps lose the chip meanwhile; settings are not touched.","Update"))return;
    try{await api("/api/obd_chip/update",{method:"POST",body:{}});toast("Update started");refresh();}
    catch(e){toast(e.message,"err");}}},"Update the chip firmware");
  async function refresh(){
    const s=await tryGet("/api/obd_chip");if(!s)return;st=s;
    const fw=s.fw||{},up=fw.update||{},ver=verOf(fw.chip);
    stateRow.replaceChildren(
      s.ready?chip("ready at "+fmtBaud(s.baud)+" baud","ok"):chip("chip not up","warn"),
      ver?chip("firmware "+ver,fw.outdated?"warn":"ok"):null,
      chip("device carries "+(fw.builtin||"-"),""),
      up.running?chip("update running","info"):up.done&&up.last&&up.last!=="ESP_OK"?chip("last update: "+up.last,"warn"):null);
    const wire=(s.bringup&&s.bringup.wire)||[];
    if(!s.ready){
      const b=banner("warn","alert",h("span",{},h("b",{},"The chip did not come up at this start. "),"The bring-up saw: "+(wire.length?wireWords(wire):"nothing recorded")+". Open the wire tools below to see what the chip sends."));
      b.style.marginTop="10px";noteRow.replaceChildren(b);
    }else if(fw.outdated&&!up.running){
      updBtn.textContent="Update to "+(fw.builtin||"the packaged image");
      noteRow.replaceChildren(h("div",{class:"banner warn",style:"display:grid;grid-template-columns:auto minmax(0,1fr) auto;align-items:center;gap:10px;margin-top:10px"},ic("alert"),
        h("div",{},h("b",{},"A newer chip firmware is available. "),"The chip runs "+(ver||"an older version")+", this device carries "+(fw.builtin||"a newer image")+". The update takes about 20 seconds; AutoPID and apps lose the chip meanwhile; settings are not touched."),
        updBtn));
    }else noteRow.replaceChildren();
    if(up.running){
      const pc=up.total?Math.round(up.lines/up.total*100):0;
      fwRow.replaceChildren(h("div",{style:"margin-top:10px"},
        h("div",{style:"display:flex;justify-content:space-between;font-size:12px;color:var(--text-2)"},h("span",{},"Update running: "+fmt.num(up.lines)+" of "+fmt.num(up.total)+" records"),h("span",{},pc+"%")),
        h("div",{style:"height:8px;background:var(--surface-3);border-radius:6px;overflow:hidden;margin-top:4px"},h("i",{style:"display:block;height:100%;width:"+pc+"%;background:var(--primary);border-radius:6px;transition:width .25s"}))));
    }else fwRow.replaceChildren();
  }
  await refresh();
  timer=setInterval(()=>{if(!document.body.contains(stateRow)){clearInterval(timer);return;}refresh();},3000);
  /* the configuration report: one block to copy */
  const repHolder=h("div",{});
  const repBtn=h("button",{class:"btn sm",type:"button",onclick:async()=>{
    repBtn.disabled=true;repHolder.replaceChildren(h("span",{class:"help"},"Reading the chip (about 2 s)…"));
    try{const r=await api("/api/obd_chip/report");
      const text=(r.items||[]).map(it=>it.err?(it.cmd+"  "+it.what+": "+it.err):(it.resp.includes("\n")?(it.cmd+"  "+it.what+":\n"+it.resp.split("\n").map(l=>"    "+l).join("\n")):(it.cmd+"  "+it.what+": "+it.resp))).join("\n");
      const box=copyBox("--- OBD chip configuration ("+stamp()+") ---\n"+text+"\n--- end ---",14);
      repHolder.replaceChildren(actionRow(h("span",{class:"help"},"What the chip is and how it is set: paste this block into a bug report."),box.btn,"margin:8px 0 6px"),box.ta);}
    catch(e){repHolder.replaceChildren(banner("warn","alert","Could not read the chip: "+e.message));}
    repBtn.disabled=false;}},"Read the chip configuration");
  /* the wire tools (folded): POST a tool, poll its result */
  const TOOL_KEY="wican-diag-wire",TOOL_MAX=200;
  let lines=[];try{lines=JSON.parse(localStorage.getItem(TOOL_KEY)||"[]");}catch{}
  const term=h("div",{class:"console",style:"height:220px"});
  const renderTerm=()=>{term.replaceChildren();if(!lines.length)term.append(h("div",{class:"dim"},"Every byte the chip sent, as text and hex, lands here."));lines.forEach(l=>term.append(h("div",{class:l.kind||""},l.t)));term.scrollTop=term.scrollHeight;};
  const say=(t,kind)=>{lines.push({t:stamp()+"  "+t,kind});if(lines.length>TOOL_MAX)lines=lines.slice(-TOOL_MAX);try{localStorage.setItem(TOOL_KEY,JSON.stringify(lines));}catch{}renderTerm();};
  renderTerm();
  const baudSel=h("select",{class:"mono"},...[2000000,115200,500000,38400,9600].map(b=>h("option",{value:b},String(b))));
  const secs=h("input",{type:"number",value:"10",min:1,max:60,style:"width:70px"});
  const cmdIn=h("input",{type:"text",value:"ATI",maxlength:15,class:"mono",style:"width:120px",title:"what the probe, scan and detect tools send; VTDLMIC3422 asks a chip whose application is gone for its bootloader"});
  const busy=h("span",{class:"help"},"");
  async function tool(kind){
    const cmd=(cmdIn.value||"").trim();
    const body={kind,baud:kind==="scan"?0:Number(baudSel.value),seconds:Number(secs.value)||10};
    if(cmd&&cmd!=="ATI"&&kind!=="listen")body.cmd=cmd;
    try{await api("/api/obd_chip/probe",{method:"POST",body});}catch(e){say("the tool was refused: "+e.message,"er");return;}
    busy.textContent="running "+kind+"…";
    let r=null;for(let i=0;i<120;i++){await new Promise(res=>setTimeout(res,600));r=await tryGet("/api/obd_chip/probe");if(r&&!r.running)break;}
    busy.textContent="";if(!r){say("no result","er");return;}
    (r.lines||[]).forEach(l=>say(kind+" "+fmtBaud(l.baud)+" "+l.cmd+(l.bytes?"  prompt="+(l.prompt?1:0)+"  "+l.bytes+" B  '"+l.text+"'  "+l.hex:"  silent"),l.prompt?"":"dim"));
    {const ok=(r.lines||[]).find(l=>l.prompt);const junk=(r.lines||[]).filter(l=>!l.prompt&&l.bytes&&ok&&l.baud!==ok.baud);if(junk.length)say("the bytes at "+fmtBaud(junk[0].baud)+" baud are the chip's answer read at the wrong speed; the chip talks at "+fmtBaud(ok.baud),"dim");}
    if(kind==="auto"){if(!r.auto||!r.auto.edges)say("detect: no edges, the chip sent nothing in "+body.seconds+" s after "+(r.cmd||"ATI")+" at "+fmtBaud(body.baud)+" baud","wn");else say("detect: "+r.auto.edges+" edges, bit time says "+r.auto.measured+" baud, nearest standard "+(r.auto.detected||"none within 8 %"),"");}
    if(kind==="listen"||(kind==="auto"&&r.auto&&r.auto.detected)){const raw=r.raw||{};say("listen "+fmtBaud(raw.baud)+" baud: "+(raw.total?raw.total+" B  '"+raw.text+"'  "+raw.hex:"nothing received"),raw.total?"":"dim");}
    if(kind==="scan")say("scan: "+(r.auto&&r.auto.detected?"prompt at "+fmtBaud(r.auto.detected)+" baud":"no prompt at any baud"),"");
    if(r.err&&r.err!=="ESP_OK")say("tool ended: "+r.err,"wn");
  }
  const tbtn=(l,k,title)=>h("button",{class:"btn sm",type:"button",title,onclick:()=>tool(k)},l);
  const bootLog=h("pre",{class:"console",style:"height:160px;margin:0;white-space:pre"});
  (async()=>{const ring=await tryGet("/api/logs/ring");const ls=typeof ring==="string"?ring.split("\n").filter(l=>l.includes("obd_chip:")):[];bootLog.textContent=ls.length?ls.slice(-40).join("\n"):"no obd_chip lines in the log ring";})();
  const tools=details("Wire tools, for a chip that will not come up",
    h("p",{class:"help",style:"margin:4px 0 0"},"Each tool holds the chip for a few seconds; AutoPID and apps wait. Detect speed lets the UART hardware measure the bit time of whatever the chip sends and listens at that speed. Listen sends nothing and shows what arrives."),
    h("div",{class:"rowflex",style:"gap:8px;flex-wrap:wrap;align-items:center"},h("label",{class:"help"},"Speed"),baudSel,h("label",{class:"help"},"Send"),cmdIn,h("label",{class:"help"},"Listen"),secs,h("span",{class:"help"},"s"),busy,h("span",{style:"flex:1 1 auto"}),
      tbtn("Probe","probe","the Send word (and VTVERS after ATI) at the chosen speed"),tbtn("Scan all speeds","scan","the Send word at every standard speed, 9600 to 3 Mbaud"),tbtn("Detect speed","auto","the Send word goes out at the chosen speed, then the UART hardware measures the bit time of what the chip sends"),tbtn("Listen","listen","send nothing, show what arrives")),
    term,
    actionRow(h("span",{class:"help"},"The chip's own log lines from the last start, what a bug report needs:"),h("button",{class:"btn sm gh",type:"button",onclick:()=>{lines=[];try{localStorage.removeItem(TOOL_KEY);}catch{}renderTerm();}},"Clear the tools' lines")),
    bootLog);
  return card("OBD chip","cpu",
    actionRow(stateRow,h("span",{class:"help"},"refreshed every 3 s")),
    noteRow,
    fwRow,
    actionRow(h("span",{class:"help"},"The chip's identity, firmware, protocol, programmable parameters and sleep settings, as one block to paste into a bug report."),repBtn,"margin-top:12px"),
    repHolder,
    tools);
}

/* ---- 2. the CAN self-test card ------------------------------------------- */
async function selftestCard(){
  /* lean (Ali, 2026-10-28: the tiles, the step cards and the failure table
     were too much): the verdict chip and the result as the same copyable
     block as the chip configuration, the console's `can_selftest` text */
  const verdictEl=h("span",{});
  const resHolder=h("div",{});
  let ranAt=null;
  const text=(r)=>{
    /* the console's lines, each step's detail on its own line so the box
       reads without a sideways scroll */
    const out=[r.verdict||"-","bus:  "+(r.bus?r.bus.baud_kbps+" kbit/s, "+(r.bus.live?"live (a vehicle talking)":"silent"):"-")];
    for(const st of r.steps||[])out.push("["+(st.ok?"ok":"--")+"] "+st.name+" ("+st.ms+" ms)","     "+st.detail);
    if((r.lines||[]).length)out.push("exchange:");
    for(const l of r.lines||[])out.push("  "+l);
    out.push("took "+r.took_ms+" ms");
    return out.join("\n");
  };
  function render(r){
    if(!r||(!r.done&&!r.running)){verdictEl.replaceChildren(chip("not run yet",""));resHolder.replaceChildren();return;}
    if(r.running){verdictEl.replaceChildren(chip("running…","info"));return;}
    if(r.refusal){verdictEl.replaceChildren(chip("not run: "+r.refusal,"warn"));resHolder.replaceChildren();return;}
    verdictEl.replaceChildren(chip(r.verdict||"-",r.pass?"ok":"warn"));
    const box=copyBox("--- CAN self-test ("+(ranAt||"before this page was opened")+") ---\n"+text(r)+"\n--- end ---",12);
    resHolder.replaceChildren(actionRow(h("span",{class:"help"},"What the test saw, line by line: paste this block into a bug report."),box.btn,"margin:8px 0 6px"),box.ta);
  }
  render(await tryGet("/api/can/selftest"));
  const runBtn=h("button",{class:"btn pri sm",type:"button",onclick:async()=>{
    runBtn.disabled=true;
    try{await api("/api/can/selftest",{method:"POST"});}catch(e){toast(e.message,"err");runBtn.disabled=false;return;}
    render({running:true});
    let r=null;for(let i=0;i<30;i++){await new Promise(res=>setTimeout(res,500));r=await tryGet("/api/can/selftest");if(r&&!r.running)break;}
    ranAt=stamp();render(r);runBtn.disabled=false;}},"Run the self-test");
  return card("CAN self-test","activity",
    actionRow(h("span",{class:"help"},"The OBD chip asks for engine speed and the WiCAN's own CAN controller answers it over the shared bus: one exchange proves both transceivers and the wiring, with or without a vehicle. About 2 s; AutoPID pauses meanwhile."),verdictEl),
    resHolder,
    h("div",{class:"rowflex",style:"justify-content:flex-end;margin-top:12px"},runBtn));
}

async function cards(){return [await chipCard(),await selftestCard()];}
PAGES.__diag={cards};
})();
